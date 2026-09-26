// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Domains
{
   /// <summary>
   ///    One value, three levels (docs/SettingLevels.md): a setting a domain or an
   ///    account may hold its own value of is resolved in one place - the account's
   ///    own value, else its domain's, else the server's - and every read site asks
   ///    that one place. Every name is one of three kinds: a LIMIT, which a level
   ///    below may tighten and never loosen; a GATE, which only the server
   ///    administrator opens or closes, at any level; and a PREFERENCE, the tenant's
   ///    own, where the deepest level wins.
   ///
   ///    Each test proves what the server then DID where a request can show it, and
   ///    says so where only the answer can: the SMTP, POP3 and IMAP tests drive the
   ///    message size, the digest, the tag, the recall and the APPEND limit; the REST
   ///    tests drive the idle timeout and the ten settings whose domain level is new
   ///    in the overlay through the /api/v1/me routes that act on them; and the
   ///    column-held rest are proved by the answer. Each has its negative control: a
   ///    sibling account with nothing of its own, which must behave exactly as an
   ///    account did before the levels existed, or the step before the value.
   ///
   ///    Two things the row's brief put one way are done the way the code has
   ///    always worked, and the tests follow the code:
   ///
   ///      * max_message_size_kb has always been the SENDING domain's limit (the
   ///        envelope sender's, at MAIL FROM), so the account's level is the
   ///        sending account's - and the signed-in account's, whatever address it
   ///        names in MAIL FROM.
   ///      * the session policies govern the browser sessions (the webmail, the
   ///        portal, the Deck), not IMAP, so the idle timeout is proved on a
   ///        session cookie, as RestApiDevices proves the domain's.
   ///
   ///    Hours and windows are chosen so that no background task can act first: the
   ///    digest hours are hours the clock will not reach during the test, and the
   ///    server's recall window - the widest there is, since the recall window is a
   ///    limit - is wider than every message's age.
   ///
   ///    Ports: the REST listener on 10040, the range this row was allocated
   ///    (10040-10049). SMTP, POP3 and IMAP are the suite's own. What a test changes
   ///    it registers to be put back, and the teardown puts every one back through
   ///    the interface that clears the overlay's cache - whether or not the test
   ///    reached its end - and fails the test, after putting back the rest, when one
   ///    of them could not be.
   /// </summary>
   [TestFixture]
   public class SettingLevels : TestFixtureBase
   {
      private static int RestPort = 10040;
      private const string AdminPassword = "testar";
      private const string Password = "Levels-Passw0rd!";
      private const string Pest = "pest@example.com";
      private const string Stranger = "stranger@outside-levels.test";
      private const string ExternalHeader = "X-hMailServer-External";

      // What each test changed, put back latest first, each on its own.
      private readonly List<Action> _putBack = new List<Action>();
      private readonly List<string> _notPutBack = new List<string>();

      [SetUp]
      public void StartRestApi()
      {
         _putBack.Clear();
         _notPutBack.Clear();
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void PutTheLevelsBack()
      {
         try
         {
            for (int i = _putBack.Count - 1; i >= 0; i--)
               Guarded_(_putBack[i]);
            _putBack.Clear();
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }

         if (_notPutBack.Count > 0)
            Assert.Fail("The teardown could not put back what the test changed: " + string.Join("; ", _notPutBack));
      }

      // ---- max_message_size_kb: a limit -------------------------------------

      [Test]
      [Description("max_message_size_kb, a limit, at three levels: the server 1000 KB, the domain 500, one account 200. MAIL FROM that account with SIZE=300 KB is refused with the size sentence and its own 200; its sibling's is taken, and at 600 KB refused with the domain's 500. An account value looser than its domain's is refused when written; one the domain has since tightened past is capped by it. Signed in as the account, naming the sibling's address in MAIL FROM does not lift its own 200, while the sibling signed in takes 300 KB. Cleared, the account takes the domain's again. The answers' _effective and _source say the same at every step.")]
      public void TheMaximumMessageSizeIsALimitHeldAtEveryLevel()
      {
         Account small = AddAccount("small");
         Account sibling = AddAccount("sibling");

         int serverBefore = _settings.MaxMessageSize;
         _putBack.Add(() => _settings.MaxMessageSize = serverBefore);
         _settings.MaxMessageSize = 1000;
         _domain.MaxMessageSize = 500;
         _domain.Save();

         // Before any account has a value of its own, both are the domain's - as
         // they were before the levels.
         AssertLevel(AccountEntry(small), "max_message_size_kb", "null", "500", "domain", "small, before");
         StringAssert.StartsWith("250", MailFrom(small.Address, 300), "Under the domain's 500, 300 KB is taken.");

         PutAccount(small, "{\"max_message_size_kb\":200}");
         AssertLevel(AccountEntry(small), "max_message_size_kb", "200", "200", "account", "small, its own");
         AssertLevel(AccountEntry(sibling), "max_message_size_kb", "null", "500", "domain", "the sibling");
         AssertLevel(DomainEntry(_domain.Name), "max_message_size_kb", null, "500", "domain", "the domain");
         ClassicAssert.AreEqual("200", small.get_SettingOverride("max_message_size_kb"), "COM reads the same row.");

         string refused = MailFrom(small.Address, 300);
         StringAssert.StartsWith("552", refused, "The account's own 200 KB refuses a 300 KB message.");
         StringAssert.Contains("exceeds fixed maximum message size", refused);
         StringAssert.Contains("Max size: 200 KB", refused);

         // The negative control: the sibling is the domain's.
         StringAssert.StartsWith("250", MailFrom(sibling.Address, 300), "The sibling has no value of its own and takes the domain's 500.");
         string siblingRefused = MailFrom(sibling.Address, 600);
         StringAssert.StartsWith("552", siblingRefused, "And the domain's 500 is still in force for it.");
         StringAssert.Contains("Max size: 500 KB", siblingRefused);

         // Signed in, the account's own limit is the account's whatever MAIL FROM
         // names; the sibling signed in is the control.
         string lifted = MailFrom(sibling.Address, 300, small.Address);
         StringAssert.StartsWith("552", lifted, "Signed in as the account, naming the sibling's address does not lift the account's own 200.");
         StringAssert.StartsWith("250", MailFrom(sibling.Address, 300, sibling.Address), "The sibling signed in sends 300 KB under the domain's 500.");

         // A limit: looser than the level above is refused when written...
         (int status, string body) looser = Admin("PUT", "/api/v1/accounts/" + small.Address, "{\"max_message_size_kb\":5000}");
         ClassicAssert.AreEqual(400, looser.status, looser.body);
         StringAssert.Contains("is a limit", looser.body);
         StringAssert.Contains("The level above holds 500 (from the domain)", looser.body);
         ClassicAssert.AreEqual("200", small.get_SettingOverride("max_message_size_kb"), "The refused value stored nothing.");

         // ...and a value the domain has since tightened past is capped by it.
         _domain.MaxMessageSize = 100;
         _domain.Save();
         AssertLevel(AccountEntry(small), "max_message_size_kb", "200", "100", "domain", "small, under a domain tightened past it");
         string capped = MailFrom(small.Address, 150);
         StringAssert.StartsWith("552", capped, "The domain's 100 caps the account's own 200.");
         StringAssert.Contains("Max size: 100 KB", capped);
         _domain.MaxMessageSize = 500;
         _domain.Save();

         // Cleared: the domain's again.
         PutAccount(small, "{\"max_message_size_kb\":null}");
         AssertLevel(AccountEntry(small), "max_message_size_kb", "null", "500", "domain", "small, cleared");
         ClassicAssert.AreEqual("", small.get_SettingOverride("max_message_size_kb"), "Null over REST removed the row.");
         StringAssert.StartsWith("250", MailFrom(small.Address, 300), "Cleared, the account takes the domain's 500 again.");
      }

      [Test]
      [Description("The signed-in account's limit is its domain's as well as its own: under a server of 1000 KB and a domain of 500, an account with no value of its own signed in and naming no sender (MAIL FROM:<>) or an outside one is refused a 600 KB message with the domain's 500; the same MAIL FROM from a session that has not signed in takes the server's 1000. And an account signed in under a domain alias is held to its own 200, as under its own spelling.")]
      public void TheSignedInAccountsDomainLimitHoldsWhateverMailFromNames()
      {
         Account small = AddAccount("small");
         Account sibling = AddAccount("sibling");

         int serverBefore = _settings.MaxMessageSize;
         _putBack.Add(() => _settings.MaxMessageSize = serverBefore);
         _settings.MaxMessageSize = 1000;
         _domain.MaxMessageSize = 500;
         _domain.Save();

         // The negative controls: nobody signed in, the envelope names no domain
         // of ours, so the server's 1000 is the limit; and the sibling naming
         // itself is held to its domain's 500.
         StringAssert.StartsWith("250", MailFrom("", 600), "Unauthenticated, MAIL FROM:<> takes the server's 1000 KB.");
         StringAssert.StartsWith("552", MailFrom(sibling.Address, 600, sibling.Address), "The domain's 500 holds the sibling naming itself.");

         // Until 26 September 2026 both of these were taken: only the signed-in
         // account's OWN value was held, and the sibling has none.
         string nullSender = MailFrom("", 600, sibling.Address);
         StringAssert.StartsWith("552", nullSender, "Signed in, MAIL FROM:<> lifted the domain's limit.");
         StringAssert.Contains("Max size: 500 KB", nullSender);
         StringAssert.StartsWith("552", MailFrom("someone@outside.example", 600, sibling.Address), "Signed in, an outside MAIL FROM lifted the domain's limit.");
         StringAssert.StartsWith("250", MailFrom("", 300, sibling.Address), "Under the domain's 500 a 300 KB message is taken.");

         // Under a domain alias: until 26 September 2026 the signed-in account was
         // looked up by the name as typed, found nothing, and was held to nothing.
         PutAccount(small, "{\"max_message_size_kb\":200}");
         DomainAlias alias = _domain.DomainAliases.Add();
         alias.DomainID = _domain.ID;
         alias.AliasName = "levels-alias.test";
         alias.Save();
         EmptyTheCaches();

         StringAssert.StartsWith("552", MailFrom("", 300, "small@" + _domain.Name), "Its own 200 holds under its own spelling.");
         string aliased = MailFrom("", 300, "small@levels-alias.test");
         StringAssert.StartsWith("552", aliased, "Signed in under the domain alias, the account's own 200 was lifted.");
         StringAssert.Contains("Max size: 200 KB", aliased);
      }

      // ---- the quarantine digest: two preferences --------------------------

      [Test]
      [Description("quarantine_digest_hour, a preference: the domain at one hour, one account at another. The digest pass run as the scheduler would at the account's hour sends the account's digest and not its sibling's, though both have something held; at the domain's hour the sibling's and not the account's, though something new is held for it too. Cleared, the account's goes at the domain's hour. The hours are ones the clock does not reach during the test, so the live five-minute task cannot send first.")]
      public void TheDigestGoesAtTheAccountsOwnHourAndItsSiblingsAtTheDomains()
      {
         int domainHour = (DateTime.Now.Hour + 6) % 24;
         int accountHour = (DateTime.Now.Hour + 9) % 24;

         TurnTheDigestOn(domainOn: true, domainHour: domainHour);

         Account night = AddAccount("night");
         Account day = AddAccount("day");

         PutAccount(night, "{\"quarantine_digest_hour\":" + accountHour + "}");
         AssertLevel(AccountEntry(night), "quarantine_digest_hour", accountHour.ToString(), accountHour.ToString(), "account", "night");
         AssertLevel(AccountEntry(day), "quarantine_digest_hour", "null", domainHour.ToString(), "domain", "day");
         AssertLevel(AccountEntry(day), "quarantine_digest", "null", "true", "domain", "day's switch");

         Hold(night, "Night 1");
         Hold(day, "Day 1");

         ClassicAssert.AreEqual(1, _settings.AntiSpam.SendQuarantineDigestsAtHour(accountHour),
            "At the account's hour: the account whose own hour it is, and nobody else.");
         Pop3ClientSimulator.AssertMessageCount(night.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(day.Address, Password, 0, TimeSpan.FromSeconds(2));

         Hold(night, "Night 2");

         ClassicAssert.AreEqual(1, _settings.AntiSpam.SendQuarantineDigestsAtHour(domainHour),
            "At the domain's hour: the sibling, and not the account with an hour of its own, though something new is held for it.");
         Pop3ClientSimulator.AssertMessageCount(day.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(night.Address, Password, 1, TimeSpan.FromSeconds(2));

         PutAccount(night, "{\"quarantine_digest_hour\":null}");
         AssertLevel(AccountEntry(night), "quarantine_digest_hour", "null", domainHour.ToString(), "domain", "night, cleared");

         ClassicAssert.AreEqual(1, _settings.AntiSpam.SendQuarantineDigestsAtHour(domainHour),
            "Cleared, the account takes the domain's hour, and what was held for it since goes.");
         Pop3ClientSimulator.AssertMessageCount(night.Address, Password, 2);
      }

      [Test]
      [Description("quarantine_digest, a preference: under a domain with the digest off, one account switches it on for itself. The pass at the domain's hour sends that account its digest and not its sibling, which has as much held; cleared, the account's switch is the domain's off again and nothing is sent.")]
      public void AnAccountsOwnDigestSwitchSendsItUnderADomainThatHasItOff()
      {
         int domainHour = (DateTime.Now.Hour + 6) % 24;

         TurnTheDigestOn(domainOn: false, domainHour: domainHour);

         Account wants = AddAccount("wants");
         Account plain = AddAccount("plain");

         PutAccount(wants, "{\"quarantine_digest\":true}");
         AssertLevel(AccountEntry(wants), "quarantine_digest", "true", "true", "account", "wants");
         AssertLevel(AccountEntry(plain), "quarantine_digest", "null", "false", "domain", "plain");

         Hold(wants, "Wanted 1");
         Hold(plain, "Plain 1");

         ClassicAssert.AreEqual(1, _settings.AntiSpam.SendQuarantineDigestsAtHour(domainHour),
            "The domain has the digest off: only the account that switched it on for itself is sent one.");
         Pop3ClientSimulator.AssertMessageCount(wants.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(plain.Address, Password, 0, TimeSpan.FromSeconds(2));

         PutAccount(wants, "{\"quarantine_digest\":null}");
         Hold(wants, "Wanted 2");
         ClassicAssert.AreEqual(0, _settings.AntiSpam.SendQuarantineDigestsAtHour(domainHour),
            "Cleared, the account's switch is the domain's off, and nothing is sent to anybody.");
      }

      // ---- the external-sender tag: a preference ---------------------------

      [Test]
      [Description("external_tag_subject and external_tag_header, preferences: the domain on, one account off - its mail from outside is not tagged while its sibling's, which inherits, is; the account cleared is tagged again. Then the other way: the domain off, the account's header on - the account is stamped and its sibling is not.")]
      public void TheExternalTagIsTheRecipientAccountsOwnWhereItHasOne()
      {
         _domain.ExternalTagSubject = true;
         _domain.ExternalTagHeader = true;
         _domain.Save();

         Account quiet = AddAccount("quiet");
         Account loud = AddAccount("loud");

         PutAccount(quiet, "{\"external_tag_subject\":false,\"external_tag_header\":false}");
         AssertLevel(AccountEntry(quiet), "external_tag_header", "false", "false", "account", "quiet");
         AssertLevel(AccountEntry(loud), "external_tag_header", "null", "true", "domain", "loud");

         SmtpClientSimulator.StaticSend(Stranger, quiet.Address, "Invoice", "Body");
         string toQuiet = Pop3ClientSimulator.AssertGetFirstMessageText(quiet.Address, Password);
         StringAssert.Contains("Subject: Invoice", toQuiet, "The account said no tag for its own mail.");
         StringAssert.DoesNotContain("[EXTERNAL]", toQuiet);
         StringAssert.DoesNotContain(ExternalHeader, toQuiet);

         // The negative control: the sibling inherits the domain's on.
         SmtpClientSimulator.StaticSend(Stranger, loud.Address, "Invoice", "Body");
         string toLoud = Pop3ClientSimulator.AssertGetFirstMessageText(loud.Address, Password);
         StringAssert.Contains("Subject: [EXTERNAL] Invoice", toLoud, "The sibling has no value of its own and is tagged as the domain says.");
         StringAssert.Contains(ExternalHeader, toLoud);

         PutAccount(quiet, "{\"external_tag_subject\":null,\"external_tag_header\":null}");
         AssertLevel(AccountEntry(quiet), "external_tag_header", "null", "true", "domain", "quiet, cleared");
         SmtpClientSimulator.StaticSend(Stranger, quiet.Address, "Receipt", "Body");
         string cleared = Pop3ClientSimulator.AssertGetFirstMessageText(quiet.Address, Password);
         StringAssert.Contains("Subject: [EXTERNAL] Receipt", cleared, "Cleared, the account is tagged as its domain says.");
         StringAssert.Contains(ExternalHeader, cleared);

         // The other way round: the domain off, the account's own header on.
         _domain.ExternalTagSubject = false;
         _domain.ExternalTagHeader = false;
         _domain.Save();
         PutAccount(quiet, "{\"external_tag_header\":true}");

         SmtpClientSimulator.StaticSend(Stranger, quiet.Address, "Statement", "Body");
         string stamped = Pop3ClientSimulator.AssertGetFirstMessageText(quiet.Address, Password);
         StringAssert.Contains(ExternalHeader, stamped, "The account's own on, under a domain that is off.");
         StringAssert.Contains("Subject: Statement", stamped, "Only the header was the account's; the subject is the domain's off.");

         SmtpClientSimulator.StaticSend(Stranger, loud.Address, "Statement", "Body");
         string unstamped = Pop3ClientSimulator.AssertGetFirstMessageText(loud.Address, Password);
         StringAssert.DoesNotContain(ExternalHeader, unstamped, "The sibling is the domain's off.");
      }

      // ---- session_idle_seconds: a limit -----------------------------------

      [Test]
      [Description("session_idle_seconds, a limit: the domain 600, one account 1. The account's browser session announces 1 and is refused after two idle seconds; its sibling's announces 600 and still works. A value looser than the domain's is refused. Cleared, the account's next session announces the domain's 600 and outlives the same wait.")]
      public void TheIdleTimeoutIsTheAccountsOwnWhereItHasOne()
      {
         Account brief = AddAccount("brief");
         Account patient = AddAccount("patient");

         ClassicAssert.IsTrue(_application.Utilities.SetDomainSessionPolicy(_domain.Name, 600, 0, 0, 0));
         PutAccount(brief, "{\"session_idle_seconds\":1}");

         AssertLevel(AccountEntry(brief), "session_idle_seconds", "1", "1", "account", "brief");
         AssertLevel(AccountEntry(patient), "session_idle_seconds", "null", "600", "domain", "patient");

         (int status, string body) looser = Admin("PUT", "/api/v1/accounts/" + patient.Address, "{\"session_idle_seconds\":1200}");
         ClassicAssert.AreEqual(400, looser.status, "An account may shorten its domain's idle timeout, not lengthen it. " + looser.body);
         StringAssert.Contains("The level above holds 600 (from the domain)", looser.body);

         (string cookie, long idle) briefSession = SignIn(brief);
         (string cookie, long idle) patientSession = SignIn(patient);
         ClassicAssert.AreEqual(1, briefSession.idle, "The account's own idle timeout is what its sign-in announces.");
         ClassicAssert.AreEqual(600, patientSession.idle, "The sibling's is the domain's.");

         ClassicAssert.AreEqual(200, WithCookie(briefSession.cookie).status, "A fresh session works.");
         ClassicAssert.AreEqual(200, WithCookie(patientSession.cookie).status, "A fresh session works.");

         Thread.Sleep(2200);

         ClassicAssert.AreEqual(401, WithCookie(briefSession.cookie).status, "Idle for longer than the account's own second, the session is refused.");
         ClassicAssert.AreEqual(200, WithCookie(patientSession.cookie).status, "The sibling's session, under the domain's 600, still works.");

         PutAccount(brief, "{\"session_idle_seconds\":null}");
         AssertLevel(AccountEntry(brief), "session_idle_seconds", "null", "600", "domain", "brief, cleared");

         (string cookie, long idle) again = SignIn(brief);
         ClassicAssert.AreEqual(600, again.idle, "Cleared, the account's sessions are the domain's.");
         Thread.Sleep(2200);
         ClassicAssert.AreEqual(200, WithCookie(again.cookie).status, "And they outlive the wait that ended the last one.");
      }

      // ---- recall_days: a limit ----------------------------------------------

      [Test]
      [Description("recall_days, a limit, at three levels through the overlay: the server 30, the domain 3, one sender 2, against messages two and a bit days old. The domain's 3 reaches the sibling sender's message; the sender's own 2 does not reach its own (404) though its domain says 3; cleared, the sender takes the domain's 3 and it is recalled; the domain tightened to 2 then puts the next one out of reach. A domain value past the server's and an account value past the domain's are refused. The server's 30 is the widest window anywhere, so the minute's sweep keeps every row throughout.")]
      public void TheRecallWindowIsALimitAtEveryLevel()
      {
         Account sender = AddAccount("recall-sender");
         Account colleague = AddAccount("recall-colleague");
         Account reader = AddAccount("recall-reader");

         SetServer("", "recall_days", "30");

         SendAuthenticated(sender, reader.Address, "levels-recall-1@levels.test", "First");
         SendAuthenticated(sender, reader.Address, "levels-recall-2@levels.test", "Second");
         SendAuthenticated(colleague, reader.Address, "levels-recall-3@levels.test", "Third");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 3);

         // Two and a bit days old, as RestApiMessageRecall backdates.
         _application.Database.ExecuteSQL("update hm_recall set recallcreated = recallcreated - 200000 where recallmsgid like 'levels-recall-%@levels.test'");

         string asSender = Basic(sender.Address, Password);
         string asColleague = Basic(colleague.Address, Password);

         AssertLevel(AccountEntry(sender), "recall_days", "null", "30", "server", "sender, before");

         (int status, string body) domainTooLong = Admin("PUT", "/api/v1/domains/" + _domain.Name, "{\"active\":true,\"recall_days\":40}");
         ClassicAssert.AreEqual(400, domainTooLong.status, "A domain may shorten the server's window, not lengthen it. " + domainTooLong.body);
         StringAssert.Contains("The level above holds 30 (from the server)", domainTooLong.body);

         PutDomain(_domain.Name, "{\"active\":true,\"recall_days\":3}");
         AssertLevel(DomainEntry(_domain.Name), "recall_days", "3", "3", "domain", "the domain");
         PutAccount(sender, "{\"recall_days\":2}");
         AssertLevel(AccountEntry(sender), "recall_days", "2", "2", "account", "sender, its own");
         AssertLevel(AccountEntry(colleague), "recall_days", "null", "3", "domain", "the colleague");

         (int status, string body) accountTooLong = Admin("PUT", "/api/v1/accounts/" + sender.Address, "{\"recall_days\":5}");
         ClassicAssert.AreEqual(400, accountTooLong.status, accountTooLong.body);
         StringAssert.Contains("The level above holds 3 (from the domain)", accountTooLong.body);

         (int status, string body) own = As(asSender, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-1@levels.test>\"}");
         ClassicAssert.AreEqual(404, own.status, "The sender's own two days, though its domain says three. " + own.body);

         // The negative control: the colleague, with nothing of its own, is the domain's 3.
         (int status, string body) domains = As(asColleague, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-3@levels.test>\"}");
         ClassicAssert.AreEqual(200, domains.status, domains.body);
         StringAssert.Contains("\"recalled\":1", domains.body, "The domain's three days reach the colleague's message.");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 2);

         PutAccount(sender, "{\"recall_days\":null}");
         AssertLevel(AccountEntry(sender), "recall_days", "null", "3", "domain", "sender, cleared");
         (int status, string body) back = As(asSender, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-1@levels.test>\"}");
         ClassicAssert.AreEqual(200, back.status, back.body);
         StringAssert.Contains("\"recalled\":1", back.body, "Cleared, the domain's three again.");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1);

         PutDomain(_domain.Name, "{\"active\":true,\"recall_days\":2}");
         (int status, string body) tightened = As(asSender, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-2@levels.test>\"}");
         ClassicAssert.AreEqual(404, tightened.status, "The domain tightened to two days puts the message out of reach. " + tightened.body);
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1, TimeSpan.FromSeconds(2));
      }

      // ---- the settings whose domain level is new in the overlay --------------

      [Test]
      [Description("The ten settings whose domain level is new in the overlay, driven through the routes that act on them. Before: the sibling's /api/v1/me answers are the server's. Then the domain holds its own - four reader maximums and the recover-deleted window tightened (limits), the OpenPGP portal, zero-access and a self-edited directory opened (gates, which the server has shut) - and one account holds its own masked-address maximum and the portal shut for itself: every /me route answers the domain's, and for that account its own. The two sending limits, which only SMTP acts on, are proved by the answer.")]
      public void TheOverlayLevelsReachTheirReadSites()
      {
         Account reader = AddAccount("overlay-reader");
         Account sibling = AddAccount("overlay-sibling");

         SetServer("", "reader_fetch_accounts_max", "5");
         SetServer("", "reader_sender_addresses_max", "5");
         SetServer("", "reader_masked_addresses_max", "10");
         SetServer("", "reader_offline_days_max", "30");
         SetServer("", "recover_deleted_days", "14");
         SetServer("", "directory_user_editable", "false");
         SetServer("openpgp", "portal_enabled", "false");
         SetServer("openpgp", "zero_access_allowed", "false");

         string asReader = Basic(reader.Address, Password);
         string asSibling = Basic(sibling.Address, Password);

         // Before: the server's, for both.
         ClassicAssert.AreEqual(10, Number(MeGet(asSibling, "/api/v1/me/masked-addresses"), "max"));
         ClassicAssert.AreEqual(5, Number(MeGet(asSibling, "/api/v1/me/sender-addresses"), "max"));
         ClassicAssert.AreEqual(30, Number(MeGet(asSibling, "/api/v1/me/offline"), "days_max"));
         ClassicAssert.AreEqual(14, Number(MeGet(asSibling, "/api/v1/me/recoverable"), "days"));
         ClassicAssert.AreEqual(200, As(asSibling, "GET", "/api/v1/me/fetch-accounts", null).status, "Five fetch accounts are offered.");
         ClassicAssert.AreEqual(403, As(asSibling, "GET", "/api/v1/me/openpgp/keys", null).status, "The server has the portal shut.");
         ClassicAssert.IsFalse(Flag(MeGet(asSibling, "/api/v1/me/zero-access"), "allowed"), "The server does not allow zero access.");
         ClassicAssert.IsFalse(Flag(MeGet(asSibling, "/api/v1/me/directory"), "editable"), "The server does not let people edit their entries.");

         int sendingMessages = TighterThanServer("sending_max_messages", 50);
         int sendingRecipients = TighterThanServer("sending_max_recipients", 60);

         PutDomain(_domain.Name, "{\"active\":true,\"reader_fetch_accounts_max\":0,\"reader_sender_addresses_max\":2,\"reader_masked_addresses_max\":3," +
                                 "\"reader_offline_days_max\":7,\"recover_deleted_days\":3,\"openpgp_portal_enabled\":true,\"zero_access_allowed\":true," +
                                 "\"directory_user_editable\":true,\"sending_max_messages\":" + sendingMessages + ",\"sending_max_recipients\":" + sendingRecipients + "}");
         PutAccount(reader, "{\"reader_masked_addresses_max\":1,\"openpgp_portal_enabled\":false}");

         // The domain's, for the sibling - limits tightened, gates opened.
         ClassicAssert.AreEqual(3, Number(MeGet(asSibling, "/api/v1/me/masked-addresses"), "max"), "reader_masked_addresses_max: the domain's.");
         ClassicAssert.AreEqual(2, Number(MeGet(asSibling, "/api/v1/me/sender-addresses"), "max"), "reader_sender_addresses_max: the domain's.");
         ClassicAssert.AreEqual(7, Number(MeGet(asSibling, "/api/v1/me/offline"), "days_max"), "reader_offline_days_max: the domain's.");
         ClassicAssert.AreEqual(3, Number(MeGet(asSibling, "/api/v1/me/recoverable"), "days"), "recover_deleted_days: the domain's.");
         ClassicAssert.AreEqual(403, As(asSibling, "GET", "/api/v1/me/fetch-accounts", null).status, "reader_fetch_accounts_max: the domain's 0 offers none.");
         ClassicAssert.AreEqual(200, As(asSibling, "GET", "/api/v1/me/openpgp/keys", null).status, "openpgp_portal_enabled: the domain opened it.");
         ClassicAssert.IsTrue(Flag(MeGet(asSibling, "/api/v1/me/zero-access"), "allowed"), "zero_access_allowed: the domain opened it.");
         ClassicAssert.IsTrue(Flag(MeGet(asSibling, "/api/v1/me/directory"), "editable"), "directory_user_editable: the domain opened it.");

         // The account's own over the domain's.
         ClassicAssert.AreEqual(1, Number(MeGet(asReader, "/api/v1/me/masked-addresses"), "max"), "The account's own maximum, tighter than its domain's.");
         ClassicAssert.AreEqual(403, As(asReader, "GET", "/api/v1/me/openpgp/keys", null).status, "The account's own portal switch, off under a domain that has it on.");
         ClassicAssert.AreEqual(2, Number(MeGet(asReader, "/api/v1/me/sender-addresses"), "max"), "And the domain's where it holds nothing of its own.");

         // Answer-proved here: the behaviour is AccountSendingLimits' (SMTP) and RestApiSend's (the webmail's send and the send API).
         AssertLevel(AccountEntry(reader), "sending_max_messages", "null", sendingMessages.ToString(), "domain", "sending_max_messages");
         AssertLevel(AccountEntry(reader), "sending_max_recipients", "null", sendingRecipients.ToString(), "domain", "sending_max_recipients");
      }

      [Test]
      [Description("The levels held in the domain's own fields, and the preferences an account holds over them, proved by the answer (_effective and _source) - impersonation, the country policy, the three quota shares, the session lifetime and the two session switches, plus addressing and the retention - for the account that holds its own and for its sibling that holds nothing. A limit looser than the domain's is refused on the way.")]
      public void TheFieldHeldLevelsAnswerWhatIsInEffect()
      {
         Account own = AddAccount("fields-own");
         Account sibling = AddAccount("fields-sibling");

         PutDomainFields(_domain.Name, "{\"active\":true,\"impersonation_action\":1,\"country_policy\":\"block:CN\",\"quota_warning_percent\":80," +
                                       "\"quota_grace_percent\":20,\"quota_send_stop_percent\":95,\"session_lifetime_seconds\":7200," +
                                       "\"session_one_per_device\":2,\"plus_addressing_enabled\":true,\"message_retention_days\":60}");

         (int status, string body) looser = Admin("PUT", "/api/v1/accounts/" + own.Address, "{\"quota_send_stop_percent\":99}");
         ClassicAssert.AreEqual(400, looser.status, "A send-stop share looser than the domain's is refused. " + looser.body);

         PutAccount(own, "{\"impersonation_action\":0,\"country_policy\":\"allow:GB\",\"quota_warning_percent\":50,\"quota_grace_percent\":10," +
                         "\"quota_send_stop_percent\":90,\"session_lifetime_seconds\":3600,\"session_one_per_device\":true," +
                         "\"session_bind_to_address\":true,\"plus_addressing_enabled\":false,\"message_retention_days\":30}");

         JsonElement mine = AccountEntry(own);
         JsonElement theirs = AccountEntry(sibling);

         AssertLevel(mine, "impersonation_action", "0", "0", "account", "own");
         AssertLevel(theirs, "impersonation_action", "null", "1", "domain", "sibling");
         AssertLevel(mine, "country_policy", "\"allow:GB\"", "\"allow:GB\"", "account", "own");
         AssertLevel(theirs, "country_policy", "null", "\"block:CN\"", "domain", "sibling");
         AssertLevel(mine, "quota_warning_percent", "50", "50", "account", "own");
         AssertLevel(theirs, "quota_warning_percent", "null", "80", "domain", "sibling");
         AssertLevel(mine, "quota_grace_percent", "10", "10", "account", "own");
         AssertLevel(theirs, "quota_grace_percent", "null", "20", "domain", "sibling");
         AssertLevel(mine, "quota_send_stop_percent", "90", "90", "account", "own");
         AssertLevel(theirs, "quota_send_stop_percent", "null", "95", "domain", "sibling");
         AssertLevel(mine, "session_lifetime_seconds", "3600", "3600", "account", "own");
         AssertLevel(theirs, "session_lifetime_seconds", "null", "7200", "domain", "sibling");
         AssertLevel(mine, "session_one_per_device", "true", "true", "account", "own");
         AssertLevel(theirs, "session_one_per_device", "null", "false", "domain", "sibling: the domain switched it off");
         AssertLevel(mine, "session_bind_to_address", "true", "true", "account", "own");
         AssertLevel(theirs, "session_bind_to_address", "null", "false", "server", "sibling: the domain takes the server's");
         AssertLevel(mine, "plus_addressing_enabled", "false", "false", "account", "own");
         AssertLevel(theirs, "plus_addressing_enabled", "null", "true", "domain", "sibling");
         AssertLevel(mine, "message_retention_days", null, "30", "account", "own: the account's own field");
         AssertLevel(theirs, "message_retention_days", null, "60", "domain", "sibling");
      }

      // ---- the IMAP APPEND limit ---------------------------------------------

      [Test]
      [Description("max_message_size_kb at IMAP APPEND: the signed-in account's own 200 KB is the limit its session advertises (APPENDLIMIT) and enforces - a 300 KB literal is refused before a byte of it is sent - while its sibling's session advertises the domain's 500 KB and asks for the literal.")]
      public void AnImapAppendIsHeldToTheAccountsOwnLimit()
      {
         Account small = AddAccount("append-small");
         Account sibling = AddAccount("append-sibling");

         int serverBefore = _settings.MaxMessageSize;
         _putBack.Add(() => _settings.MaxMessageSize = serverBefore);
         _settings.MaxMessageSize = 1000;
         _domain.MaxMessageSize = 500;
         _domain.Save();
         PutAccount(small, "{\"max_message_size_kb\":200}");

         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(143), "Could not connect to the IMAP server on port 143.");
            socket.ReadUntil("* OK");
            socket.Send("A01 LOGIN \"" + small.Address + "\" \"" + Password + "\"\r\n");
            socket.ReadUntil("A01 OK");

            socket.Send("A02 CAPABILITY\r\n");
            StringAssert.Contains("APPENDLIMIT=204800", socket.ReadUntil("A02 OK"), "The account's own 200 KB is the advertised limit.");

            string refused = socket.SendAndReceive("A03 APPEND INBOX {307200}\r\n");
            ClassicAssert.IsFalse(refused.StartsWith("+"), "A 300 KB literal over the account's own 200 KB must be refused, not asked for. Got: " + refused);
            StringAssert.Contains("A03 NO", refused);
         }

         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(143), "Could not connect to the IMAP server on port 143.");
            socket.ReadUntil("* OK");
            socket.Send("A01 LOGIN \"" + sibling.Address + "\" \"" + Password + "\"\r\n");
            socket.ReadUntil("A01 OK");

            socket.Send("A02 CAPABILITY\r\n");
            StringAssert.Contains("APPENDLIMIT=512000", socket.ReadUntil("A02 OK"), "The sibling's session is the domain's 500 KB.");

            string asked = socket.SendAndReceive("A03 APPEND INBOX {307200}\r\n");
            StringAssert.StartsWith("+", asked, "Under the domain's 500 KB, the sibling's 300 KB literal is asked for.");
         }
      }

      // ---- the overlay's own rules -------------------------------------------

      [Test]
      [Description("The overlay's rows go with their owner: deleting an account deletes its rows and deleting a domain deletes the domain's, each proved by the probe - an insert of the same (scope, owner, name), which the unique index refuses while the row is there and takes once it is gone. A sibling's row stays.")]
      public void AnOwnersRowsAreDeletedWithIt()
      {
         Account leaving = AddAccount("leaving");
         Account staying = AddAccount("staying");
         long leavingId = leaving.ID;
         long stayingId = staying.ID;

         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("levels-cascade.test");
         long otherId = other.ID;

         // The probe's own rows, if it makes any, go whatever happens below.
         _putBack.Add(() => _application.Database.ExecuteSQL("delete from hm_settingoverrides where soname = 'recall_days' and ((soscope = 2 and soownerid = " + leavingId + ") or (soscope = 1 and soownerid = " + otherId + "))"));

         leaving.set_SettingOverride("recall_days", "5");
         staying.set_SettingOverride("recall_days", "6");
         _putBack.Add(() => staying.set_SettingOverride("recall_days", ""));
         other.set_SettingOverride("recall_days", "9");

         // The probe's own control: while the row is there, the insert is refused.
         ClassicAssert.IsFalse(ProbeInsert(2, leavingId), "The account's row is there before the account is deleted.");
         ClassicAssert.IsFalse(ProbeInsert(1, otherId), "The domain's row is there before the domain is deleted.");

         leaving.Delete();
         other.Delete();

         ClassicAssert.IsTrue(ProbeInsert(2, leavingId), "Deleting the account deleted its row.");
         ClassicAssert.IsTrue(ProbeInsert(1, otherId), "Deleting the domain deleted its row.");
         ClassicAssert.IsFalse(ProbeInsert(2, stayingId), "The sibling's row is not the deleted account's and stays.");
         ClassicAssert.AreEqual("6", staying.get_SettingOverride("recall_days"));
      }

      [Test]
      [Description("The XML backup carries the overlay's rows under the domain and the account: a backup, every domain deleted, a restore - and the domain's and the account's own values are back, with a sibling that had none still inheriting.")]
      public void TheBackupCarriesTheOverlay()
      {
         string address = "kept@" + _domain.Name;
         string siblingAddress = "plain@" + _domain.Name;
         Account kept = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, siblingAddress, Password);

         int recallDays = ServerInt("", "recall_days");
         int domainDays = Math.Max(1, recallDays - 1);
         int accountDays = Math.Max(1, domainDays - 1);

         _domain.set_SettingOverride("recall_days", domainDays.ToString());
         _putBack.Add(() => _domain.set_SettingOverride("recall_days", ""));
         kept.set_SettingOverride("recall_days", accountDays.ToString());
         kept.set_SettingOverride("quarantine_digest_hour", "9");
         kept.set_SettingOverride("max_message_size_kb", "200");
         foreach (string name in new[] { "recall_days", "quarantine_digest_hour", "max_message_size_kb" })
         {
            string cleared = name;
            _putBack.Add(() => kept.set_SettingOverride(cleared, ""));
         }

         string directory = Path.Combine(Path.GetTempPath(), "hm-levels-" + TestSetup.UniqueString());
         Directory.CreateDirectory(directory);
         _putBack.Add(() => Directory.Delete(directory, true));

         RunBackup(directory);

         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         Restore(directory);

         Domain restored = _application.Domains.get_ItemByName(_domain.Name);
         Account restoredKept = restored.Accounts.get_ItemByAddress(address);
         Account restoredSibling = restored.Accounts.get_ItemByAddress(siblingAddress);

         // The restored owners' rows, cleared through the interface as well; the
         // next set-up deletes the domain, which takes them anyway.
         _putBack.Add(() => restored.set_SettingOverride("recall_days", ""));
         foreach (string name in new[] { "recall_days", "quarantine_digest_hour", "max_message_size_kb" })
         {
            string cleared = name;
            _putBack.Add(() => restoredKept.set_SettingOverride(cleared, ""));
         }

         ClassicAssert.AreEqual(domainDays.ToString(), restored.get_SettingOverride("recall_days"), "The domain's own value came back.");
         ClassicAssert.AreEqual(accountDays.ToString(), restoredKept.get_SettingOverride("recall_days"), "The account's own values came back.");
         ClassicAssert.AreEqual("9", restoredKept.get_SettingOverride("quarantine_digest_hour"));
         ClassicAssert.AreEqual("200", restoredKept.get_SettingOverride("max_message_size_kb"));
         ClassicAssert.AreEqual("account", restoredKept.get_EffectiveSettingSource("recall_days"));

         // The negative control: nothing was invented for the account that had nothing.
         ClassicAssert.AreEqual("", restoredSibling.get_SettingOverride("recall_days"), "The sibling had no value of its own and has none now.");
         ClassicAssert.AreEqual(domainDays.ToString(), restoredSibling.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("domain", restoredSibling.get_EffectiveSettingSource("recall_days"));
      }

      [Test]
      [Description("COM and REST agree, and refuse alike: a domain's and an account's own values round-trip over COM with EffectiveSetting and EffectiveSettingSource; an empty string or a REST null makes a level inherit; a name that is the server's alone is refused on both doors; a value the setting refuses, a limit looser than the level above and a value of the wrong JSON type are refused with their own sentences and store nothing; a level held in a field of its own is refused over the overlay (COM - REST writes the field by design); an unsaved account's clear is refused; the answered _effective and _source are ignored when sent back, so a domain read and sent back as it is is taken; and the server's routes say which keys have levels while the domain listing leaves the level fields out.")]
      public void ComAndRestRoundTripAndRefuseAlike()
      {
         Account mine = AddAccount("mine");
         Account theirs = AddAccount("theirs");
         SetServer("", "recall_days", "20");

         _domain.set_SettingOverride("recall_days", "10");
         _putBack.Add(() => _domain.set_SettingOverride("recall_days", ""));
         ClassicAssert.AreEqual("10", _domain.get_SettingOverride("recall_days"));
         ClassicAssert.AreEqual("10", _domain.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("domain", _domain.get_EffectiveSettingSource("recall_days"));

         mine.set_SettingOverride("recall_days", "5");
         _putBack.Add(() => mine.set_SettingOverride("recall_days", ""));
         ClassicAssert.AreEqual("5", mine.get_SettingOverride("recall_days"));
         ClassicAssert.AreEqual("5", mine.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("account", mine.get_EffectiveSettingSource("recall_days"));
         ClassicAssert.AreEqual("", theirs.get_SettingOverride("recall_days"), "The sibling has none of its own.");
         ClassicAssert.AreEqual("10", theirs.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("domain", theirs.get_EffectiveSettingSource("recall_days"));
         AssertLevel(AccountEntry(mine), "recall_days", "5", "5", "account", "REST reads what COM wrote");

         // A limit looser than the level above, on both doors.
         COMException comLooser = Assert.Throws<COMException>(() => mine.set_SettingOverride("recall_days", "15"));
         StringAssert.Contains("is a limit", comLooser.Message);
         (int status, string body) restLooser = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"recall_days\":15}");
         ClassicAssert.AreEqual(400, restLooser.status, restLooser.body);
         StringAssert.Contains("The level above holds 10 (from the domain)", restLooser.body);
         ClassicAssert.AreEqual("5", mine.get_SettingOverride("recall_days"), "A refused value stored nothing.");

         // REST null and COM's empty string are the same "take the level above".
         PutAccount(mine, "{\"recall_days\":null}");
         ClassicAssert.AreEqual("", mine.get_SettingOverride("recall_days"));
         ClassicAssert.AreEqual("domain", mine.get_EffectiveSettingSource("recall_days"));
         _domain.set_SettingOverride("recall_days", "");
         ClassicAssert.AreEqual("20", mine.get_EffectiveSetting("recall_days"), "Neither level holds one: the server's.");
         ClassicAssert.AreEqual("server", mine.get_EffectiveSettingSource("recall_days"));

         // A name that is the server's alone, on both doors.
         COMException comServerOnly = Assert.Throws<COMException>(() => mine.set_SettingOverride("max_delivery_threads", "5"));
         StringAssert.Contains("GET /api/v1/settings/levels", comServerOnly.Message);
         Assert.Throws<COMException>(() => _domain.set_SettingOverride("max_delivery_threads", "5"));
         Assert.Throws<COMException>(() => mine.get_EffectiveSetting("max_delivery_threads"));
         ClassicAssert.AreEqual(400, Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"max_delivery_threads\":5}").status);
         ClassicAssert.AreEqual(400, Admin("PUT", "/api/v1/domains/" + _domain.Name, "{\"active\":true,\"max_delivery_threads\":5}").status);

         // A value the setting refuses, with its own sentence, and nothing stored.
         COMException comRange = Assert.Throws<COMException>(() => mine.set_SettingOverride("recall_days", "0"));
         StringAssert.Contains("recall_days must be between 1 and 365", comRange.Message);
         (int status, string body) restRange = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"recall_days\":366}");
         ClassicAssert.AreEqual(400, restRange.status, restRange.body);
         StringAssert.Contains("recall_days must be between 1 and 365", restRange.body);

         // A value of the wrong JSON type: true is not 1 KB, "5" is not five days.
         (int status, string body) boolForNumber = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"max_message_size_kb\":true}");
         ClassicAssert.AreEqual(400, boolForNumber.status, boolForNumber.body);
         StringAssert.Contains("must be a whole number", boolForNumber.body);
         (int status, string body) textForNumber = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"recall_days\":\"5\"}");
         ClassicAssert.AreEqual(400, textForNumber.status, textForNumber.body);
         (int status, string body) numberForSwitch = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"quarantine_digest\":1}");
         ClassicAssert.AreEqual(400, numberForSwitch.status, numberForSwitch.body);
         StringAssert.Contains("must be true or false", numberForSwitch.body);
         ClassicAssert.AreEqual("", mine.get_SettingOverride("recall_days"), "Nothing refused was stored.");
         ClassicAssert.AreEqual("", mine.get_SettingOverride("max_message_size_kb"));

         // A level held in a field of its own is set there, not in the overlay.
         COMException column = Assert.Throws<COMException>(() => _domain.set_SettingOverride("max_message_size_kb", "300"));
         StringAssert.Contains("own field", column.Message);

         // An unsaved account has no values to clear, and saying "done" would be a lie.
         Account unsaved = _domain.Accounts.Add();
         COMException notSaved = Assert.Throws<COMException>(() => unsaved.set_SettingOverride("recall_days", ""));
         StringAssert.Contains("has not been saved yet", notSaved.Message);

         // What the answer says is in effect is ignored when sent back...
         (int status, string body) readOnly = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"recall_days_effective\":7}");
         ClassicAssert.AreEqual(200, readOnly.status, readOnly.body);
         ClassicAssert.AreEqual("", mine.get_SettingOverride("recall_days"), "An answered field sent back changed nothing.");

         // ...so a domain read and sent back as it is, is taken.
         (int status, string body) read = Admin("GET", "/api/v1/domains/" + _domain.Name, null);
         ClassicAssert.AreEqual(200, read.status, read.body);
         StringAssert.Contains("\"recall_days_effective\"", read.body, "The domain itself answers the level fields.");
         (int status, string body) sentBack = Admin("PUT", "/api/v1/domains/" + _domain.Name, read.body);
         ClassicAssert.AreEqual(200, sentBack.status, "A domain read and sent back as it is must be taken. " + sentBack.body);

         (int status, string body) listing = Admin("GET", "/api/v1/domains", null);
         ClassicAssert.AreEqual(200, listing.status, listing.body);
         StringAssert.DoesNotContain("\"recall_days_effective\"", listing.body, "The listing leaves the level fields out.");

         // The server's routes say which keys have levels, and the list says what they are.
         (int status, string body) settings = Admin("GET", "/api/v1/settings", null);
         ClassicAssert.AreEqual(200, settings.status, settings.body);
         StringAssert.Contains("\"recall_days_levels\":\"account\"", settings.body);
         StringAssert.DoesNotContain("\"max_delivery_threads_levels\"", settings.body, "A server-only key says nothing of levels.");
         ClassicAssert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"recall_days_levels\":\"server\"}").status, "A mark is answered and never taken.");
         StringAssert.Contains("\"levels\":\"account\"", Admin("GET", "/api/v1/settings/ini/QuotaWarningPercent", null).body);
         StringAssert.Contains("\"levels\":\"server\"", Admin("GET", "/api/v1/settings/ini/MaxSubmissionsPerIPPerMinute", null).body);
         (int status, string body) levels = Admin("GET", "/api/v1/settings/levels", null);
         ClassicAssert.AreEqual(200, levels.status, levels.body);
         StringAssert.Contains("\"name\":\"recall_days\"", levels.body);
         StringAssert.DoesNotContain("\"name\":\"max_delivery_threads\"", levels.body);
         JsonElement listed = Parse(levels.body);
         var classes = new Dictionary<string, string>();
         foreach (JsonElement entry in listed.GetProperty("settings").EnumerateArray())
            classes[entry.GetProperty("name").GetString()] = entry.GetProperty("class").GetString();
         ClassicAssert.AreEqual(27, classes.Count);
         ClassicAssert.AreEqual("limit", classes["recall_days"]);
         ClassicAssert.AreEqual("gate", classes["zero_access_allowed"]);
         ClassicAssert.AreEqual("preference", classes["quarantine_digest_hour"]);
      }

      [Test]
      [Description("A domain administrator over COM writes the setting values of its own domain's ordinary accounts - a preference, a limit tightened - and not a gate, nor a limit loosened; and not the values of a server administrator's mailbox in its domain, neither setting one nor clearing the one the server administrator set, which is what Save refuses it too. Plus addressing, a gate, is refused it as a change and taken as the same value written back.")]
      public void ADomainAdministratorCannotAimAValueAtAServerAdministrator()
      {
         SetServer("", "recall_days", "20");

         Account deputy = AddAccount("deputy");
         deputy.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         deputy.Save();
         Account chief = AddAccount("chief");
         chief.AdminLevel = eAdminLevel.hAdminLevelServerAdmin;
         chief.Save();
         Account plain = AddAccount("plain");

         chief.set_SettingOverride("quarantine_digest_hour", "9");
         _putBack.Add(() => chief.set_SettingOverride("quarantine_digest_hour", ""));

         var asDeputy = new hMailServer.Application();
         ClassicAssert.IsNotNull(asDeputy.Authenticate(deputy.Address, Password));
         Domain domainAsDeputy = asDeputy.Domains.get_ItemByName(_domain.Name);
         Account chiefAsDeputy = domainAsDeputy.Accounts.get_ItemByAddress(chief.Address);
         Account plainAsDeputy = domainAsDeputy.Accounts.get_ItemByAddress(plain.Address);

         COMException aimed = Assert.Throws<COMException>(() => chiefAsDeputy.set_SettingOverride("quarantine_digest_hour", "3"));
         StringAssert.Contains("server administrator", aimed.Message);
         Assert.Throws<COMException>(() => chiefAsDeputy.set_SettingOverride("quarantine_digest_hour", ""),
            "Clearing the server administrator's own value is refused as well.");
         ClassicAssert.AreEqual("9", chief.get_SettingOverride("quarantine_digest_hour"), "The server administrator's value stands.");

         // The negative control: an ordinary account of the same domain.
         plainAsDeputy.set_SettingOverride("quarantine_digest_hour", "10");
         _putBack.Add(() => plain.set_SettingOverride("quarantine_digest_hour", ""));
         ClassicAssert.AreEqual("10", plain.get_SettingOverride("quarantine_digest_hour"), "A preference of an ordinary account is the domain administrator's.");

         plainAsDeputy.set_SettingOverride("recall_days", "2");
         _putBack.Add(() => plain.set_SettingOverride("recall_days", ""));
         ClassicAssert.AreEqual("2", plain.get_SettingOverride("recall_days"), "A limit tightened is the domain administrator's.");
         COMException loosened = Assert.Throws<COMException>(() => plainAsDeputy.set_SettingOverride("recall_days", "30"));
         StringAssert.Contains("is a limit", loosened.Message);

         COMException gate = Assert.Throws<COMException>(() => plainAsDeputy.set_SettingOverride("zero_access_allowed", "1"));
         StringAssert.Contains("capability the server grants", gate.Message);
         ClassicAssert.AreEqual("", plain.get_SettingOverride("zero_access_allowed"));

         bool plusBefore = domainAsDeputy.PlusAddressingEnabled;
         domainAsDeputy.PlusAddressingEnabled = plusBefore;
         Assert.Throws<COMException>(() => domainAsDeputy.PlusAddressingEnabled = !plusBefore,
            "Plus addressing is a gate: the server administrator's to switch.");
      }

      [Test]
      [Description("A key restricted to one domain tightens its own domain's and its own accounts' limits - and is refused (400, the sentence naming the level above) past the server's value; it sets a preference; it may not open a gate even on its own (403); another domain is not its to touch (403); and the server-wide list of the settings with levels is refused to it.")]
      public void ADomainRestrictedKeyTightensItsOwnAndOpensNoGate()
      {
         Account own = AddAccount("own");
         Domain elsewhere = SingletonProvider<TestSetup>.Instance.AddDomain("levels-elsewhere.test");
         Account foreign = SingletonProvider<TestSetup>.Instance.AddAccount(elsewhere, "foreign@levels-elsewhere.test", Password);
         SetServer("", "recall_days", "20");
         foreach (string name in new[] { "recall_days", "quarantine_digest_hour", "max_message_size_kb" })
         {
            string cleared = name;
            _putBack.Add(() => own.set_SettingOverride(cleared, ""));
         }
         _putBack.Add(() => _domain.set_SettingOverride("recall_days", ""));

         (int status, string body) created = Admin("POST", "/api/v1/apikeys",
            "{\"label\":\"levels - here\",\"scope\":\"full\",\"domains\":\"" + _domain.Name + "\"}");
         ClassicAssert.AreEqual(201, created.status, "POST /api/v1/apikeys must create a key. Body: " + created.body);
         JsonElement key = Parse(created.body);
         string keyId = key.GetProperty("id").ToString();
         string bearer = "Bearer " + key.GetProperty("key").GetString();
         _putBack.Add(() => Admin("DELETE", "/api/v1/apikeys/" + keyId, null));

         // A limit shortened, on its own account and its own domain.
         (int status, string body) shortened = As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"recall_days\":7}");
         ClassicAssert.AreEqual(200, shortened.status, shortened.body);
         ClassicAssert.AreEqual("7", own.get_SettingOverride("recall_days"), "Its own domain's account took the shorter window.");
         (int status, string body) ownDomain = As(bearer, "PUT", "/api/v1/domains/" + _domain.Name, "{\"active\":true,\"recall_days\":9}");
         ClassicAssert.AreEqual(200, ownDomain.status, ownDomain.body);
         ClassicAssert.AreEqual("9", _domain.get_SettingOverride("recall_days"), "Its own domain took it.");

         // ...and refused past the level above: the domain's 9 for the account,
         // the server's 20 for the domain.
         (int status, string body) lengthened = As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"recall_days\":30}");
         ClassicAssert.AreEqual(400, lengthened.status, lengthened.body);
         StringAssert.Contains("The level above holds 9 (from the domain)", lengthened.body);
         (int status, string body) domainLengthened = As(bearer, "PUT", "/api/v1/domains/" + _domain.Name, "{\"active\":true,\"recall_days\":30}");
         ClassicAssert.AreEqual(400, domainLengthened.status, domainLengthened.body);
         StringAssert.Contains("The level above holds 20 (from the server)", domainLengthened.body);
         ClassicAssert.AreEqual("7", own.get_SettingOverride("recall_days"));

         // The message size too: tightened, and not loosened past the server's.
         ClassicAssert.AreEqual(200, As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"max_message_size_kb\":100}").status);
         ClassicAssert.AreEqual(400, As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"max_message_size_kb\":2000000}").status);
         ClassicAssert.AreEqual("100", own.get_SettingOverride("max_message_size_kb"));

         // A preference is the tenant's.
         ClassicAssert.AreEqual(200, As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"quarantine_digest_hour\":6}").status);

         // A gate is not, even on its own account.
         (int status, string body) gate = As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"zero_access_allowed\":true}");
         ClassicAssert.AreEqual(403, gate.status, gate.body);
         ClassicAssert.AreEqual("", own.get_SettingOverride("zero_access_allowed"), "The refused gate stored nothing.");

         // The negative control: another domain's, through the same key.
         ClassicAssert.AreEqual(403, As(bearer, "PUT", "/api/v1/accounts/" + foreign.Address, "{\"recall_days\":7}").status);
         ClassicAssert.AreEqual(403, As(bearer, "PUT", "/api/v1/domains/" + elsewhere.Name, "{\"active\":true,\"recall_days\":9}").status);
         ClassicAssert.AreEqual("", foreign.get_SettingOverride("recall_days"), "Another domain's account was not touched.");
         ClassicAssert.AreEqual("", elsewhere.get_SettingOverride("recall_days"), "Another domain was not touched.");

         ClassicAssert.AreEqual(403, As(bearer, "GET", "/api/v1/settings/levels", null).status, "The list is server-wide.");
      }

      [Test]
      [Description("A row the interfaces would refuse - put straight into hm_settingoverrides - is treated as inheriting and reported as HM7311, once per setting until the caches are emptied. The control: the same row with a value the interfaces take is the account's own once the caches are emptied, which is also what proves an emptied cache reads a row written behind the server's back at once.")]
      public void ARowTheInterfacesWouldRefuseIsTreatedAsInheriting()
      {
         Account target = AddAccount("handmade");
         long id = target.ID;
         int server = ServerInt("", "recall_days");
         int taken = Math.Max(1, server - 1);

         _putBack.Add(() => _application.Database.ExecuteSQL("delete from hm_settingoverrides where soscope = 2 and soownerid = " + id));

         // The control: a value the interfaces take, written by hand, is the
         // account's own as soon as the caches are emptied.
         _application.Database.ExecuteSQL("insert into hm_settingoverrides (soscope, soownerid, soname, sovalue) values (2, " + id + ", 'recall_days', '" + taken + "')");
         EmptyTheCaches();
         AssertLevel(AccountEntry(target), "recall_days", taken.ToString(), taken.ToString(), "account", "a value the interfaces take");

         // The same row with a value they refuse: inheriting, and said.
         _application.Database.ExecuteSQL("update hm_settingoverrides set sovalue = '0' where soscope = 2 and soownerid = " + id + " and soname = 'recall_days'");
         EmptyTheCaches();
         AssertLevel(AccountEntry(target), "recall_days", "null", server.ToString(), "server", "a value the interfaces refuse");

         string errors = LogHandler.ReadAndDeleteErrorLog();
         StringAssert.Contains("HM7311", errors, "The refused row is reported.");
         StringAssert.Contains("recall_days", errors);
      }

      [Test]
      [Description("build/check-setting-levels.py, which holds every read of a setting with levels to the resolver and every name to one kind, passes on this checkout - and it passes only because its negative controls (a new reader, a read site with its resolver call taken out, a reader in an #else branch, a gate that tightens, each in a temporary copy) are refused on the same run. Ignored where there is no Python or no checkout.")]
      public void TheCheckerRefusesAReadSiteWithoutTheResolver()
      {
         string python = FindPython();
         if (python == null)
            Assert.Ignore("No python.exe on the PATH, so the checker cannot run here.");

         string checker = RepositoryFile(Path.Combine("build", "check-setting-levels.py"));

         var start = new ProcessStartInfo(python, "\"" + checker + "\"")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };
         start.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            ClassicAssert.IsTrue(process.WaitForExit(120 * 1000), "The checker did not finish within two minutes.");
            process.WaitForExit();

            string printed = output.Result + error.Result;
            ClassicAssert.AreEqual(0, process.ExitCode, printed);
            StringAssert.Contains("the negative controls", printed);
            StringAssert.Contains("are refused", printed);
            StringAssert.DoesNotContain("self-test:", printed, "A negative control did not hold.");
         }
      }

      // ---- helpers -------------------------------------------------------------

      private void Guarded_(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            _notPutBack.Add(e.Message);
            Console.WriteLine("SettingLevels teardown: " + e.Message);
         }
      }

      private Account AddAccount(string localPart)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, localPart + "@" + _domain.Name, Password);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) As(string authorization, string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static JsonElement Parse(string body)
      {
         return JsonDocument.Parse(body).RootElement.Clone();
      }

      private static JsonElement MeGet(string authorization, string path)
      {
         (int status, string body) answer = As(authorization, "GET", path, null);
         ClassicAssert.AreEqual(200, answer.status, "GET " + path + ": " + answer.body);
         return Parse(answer.body);
      }

      private static long Number(JsonElement document, string name)
      {
         return document.GetProperty(name).GetInt64();
      }

      private static bool Flag(JsonElement document, string name)
      {
         return document.GetProperty(name).GetBoolean();
      }

      private static JsonElement AccountEntry(Account account)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/accounts/" + account.Address, null);
         ClassicAssert.AreEqual(200, answer.status, "GET the account: " + answer.body);
         return Parse(answer.body);
      }

      private static JsonElement DomainEntry(string name)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/domains/" + name, null);
         ClassicAssert.AreEqual(200, answer.status, "GET the domain: " + answer.body);
         return Parse(answer.body);
      }

      /// <summary>
      ///    PUT /api/v1/accounts/{address} as the administrator, and every name in the
      ///    body that the overlay holds cleared again in the teardown.
      /// </summary>
      private void PutAccount(Account account, string json)
      {
         foreach (JsonProperty member in Parse(json).EnumerateObject())
         {
            if (member.Name == "message_retention_days")
               continue;
            string name = member.Name;
            _putBack.Add(() => account.set_SettingOverride(name, ""));
         }

         (int status, string body) answer = Admin("PUT", "/api/v1/accounts/" + account.Address, json);
         ClassicAssert.AreEqual(200, answer.status, "PUT the account " + json + ": " + answer.body);
      }

      /// <summary>The same for a domain's own values that the overlay holds.</summary>
      private void PutDomain(string name, string json)
      {
         Domain domain = _application.Domains.get_ItemByName(name);
         foreach (JsonProperty member in Parse(json).EnumerateObject())
         {
            if (member.Name == "active")
               continue;
            string setting = member.Name;
            _putBack.Add(() => domain.set_SettingOverride(setting, ""));
         }

         (int status, string body) answer = Admin("PUT", "/api/v1/domains/" + name, json);
         ClassicAssert.AreEqual(200, answer.status, "PUT the domain " + json + ": " + answer.body);
      }

      /// <summary>A domain's own fields, which the next set-up's new domain puts back.</summary>
      private static void PutDomainFields(string name, string json)
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/domains/" + name, json);
         ClassicAssert.AreEqual(200, answer.status, "PUT the domain " + json + ": " + answer.body);
      }

      /// <summary>
      ///    The level's own value (as JSON, or null to leave it unasked: a domain's
      ///    field keeps the key it always had), what is in effect and where it came from.
      /// </summary>
      private static void AssertLevel(JsonElement entry, string name, string own, string effective, string source, string context)
      {
         if (own != null)
            ClassicAssert.AreEqual(own, entry.GetProperty(name).GetRawText(), context + ": " + name + ", the level's own value");
         ClassicAssert.AreEqual(effective, entry.GetProperty(name + "_effective").GetRawText(), context + ": " + name + "_effective");
         ClassicAssert.AreEqual(source, entry.GetProperty(name + "_source").GetString(), context + ": " + name + "_source");
      }

      private static string GroupPath(string group)
      {
         return group.Length == 0 ? "/api/v1/settings" : "/api/v1/settings/" + group;
      }

      private static int ServerInt(string group, string key)
      {
         (int status, string body) answer = Admin("GET", GroupPath(group), null);
         ClassicAssert.AreEqual(200, answer.status, answer.body);
         return Parse(answer.body).GetProperty(key).GetInt32();
      }

      /// <summary>
      ///    Sets one key of a server settings group over REST, and puts the value it
      ///    had back in the teardown - a put-back that fails is said, not swallowed.
      /// </summary>
      private void SetServer(string group, string key, string json)
      {
         (int status, string body) before = Admin("GET", GroupPath(group), null);
         ClassicAssert.AreEqual(200, before.status, before.body);
         string old = Parse(before.body).GetProperty(key).GetRawText();

         _putBack.Add(() =>
         {
            (int status, string body) back = Admin("PUT", GroupPath(group), "{\"" + key + "\":" + old + "}");
            if (back.status != 200)
               throw new InvalidOperationException(key + " could not be put back to " + old + ": " + back.status + " " + back.body);
         });

         (int status, string body) set = Admin("PUT", GroupPath(group), "{\"" + key + "\":" + json + "}");
         ClassicAssert.AreEqual(200, set.status, "PUT " + key + "=" + json + ": " + set.body);
      }

      /// <summary>A value of a sending limit no looser than the server's, which may be 0 (no limit).</summary>
      private static int TighterThanServer(string name, int wanted)
      {
         JsonElement levels = Parse(Admin("GET", "/api/v1/settings/levels", null).body);
         foreach (JsonElement entry in levels.GetProperty("settings").EnumerateArray())
         {
            if (entry.GetProperty("name").GetString() != name)
               continue;
            int server = entry.GetProperty("server_value").GetInt32();
            return server <= 0 ? wanted : Math.Max(1, Math.Min(wanted, server));
         }

         Assert.Fail(name + " is not in GET /api/v1/settings/levels.");
         return wanted;
      }

      private static void EmptyTheCaches()
      {
         (int status, string body) answer = Admin("POST", "/api/v1/settings/cache/clear", null);
         ClassicAssert.AreEqual(200, answer.status, answer.body);
      }

      /// <summary>
      ///    MAIL FROM the address declaring the size, on a session of its own -
      ///    signed in as signInAs first when it is given; the answer.
      /// </summary>
      private static string MailFrom(string sender, int kilobytes, string signInAs = null)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25));
            ClassicAssert.IsTrue(socket.Receive().StartsWith("220"));
            socket.Send("EHLO example.test\r\n");
            string ehlo = socket.ReadUntil("250 HELP");
            ClassicAssert.IsTrue(ehlo.Contains("250"), "EHLO was not accepted. Got: " + ehlo);

            if (signInAs != null)
            {
               StringAssert.StartsWith("334", socket.SendAndReceive("AUTH LOGIN\r\n"));
               StringAssert.StartsWith("334", socket.SendAndReceive(Convert.ToBase64String(Encoding.UTF8.GetBytes(signInAs)) + "\r\n"));
               StringAssert.StartsWith("235", socket.SendAndReceive(Convert.ToBase64String(Encoding.UTF8.GetBytes(Password)) + "\r\n"), "The sign-in was refused.");
            }

            string answer = socket.SendAndReceive("MAIL FROM:<" + sender + "> SIZE=" + (kilobytes * 1024) + "\r\n");
            socket.Send("QUIT\r\n");
            return answer;
         }
      }

      private static void SendAuthenticated(Account from, string to, string messageId, string subject)
      {
         new SmtpClientSimulator().SendRaw(from.Address, Password, from.Address, to,
            "From: " + from.Address + "\r\nTo: " + to + "\r\nSubject: " + subject + "\r\nMessage-ID: <" + messageId + ">\r\n\r\n" + subject + ", sent over authenticated SMTP.\r\n");
      }

      /// <summary>
      ///    Whether an overlay row of this scope and owner could be inserted: false
      ///    while one is there (the unique index refuses it), true once it is gone.
      ///    The caller registers the delete of the probe's own row before it probes.
      /// </summary>
      private bool ProbeInsert(int scope, long ownerId)
      {
         try
         {
            _application.Database.ExecuteSQL("insert into hm_settingoverrides (soscope, soownerid, soname, sovalue) values (" +
                                             scope + ", " + ownerId + ", 'recall_days', '5')");
            return true;
         }
         catch (COMException)
         {
            // The refusal is reported to the error log as well; that line is the
            // probe's, not a fault.
            LogHandler.ReadAndDeleteErrorLog();
            return false;
         }
      }

      // ---- the quarantine digest ----------------------------------------------

      /// <summary>
      ///    The quarantine on, the digest's link address set and the domain's own
      ///    switch and hour - each server value put back as it was found.
      /// </summary>
      private void TurnTheDigestOn(bool domainOn, int domainHour)
      {
         (int status, string body) enabled = Admin("GET", "/api/v1/settings/ini/QuarantineEnabled", null);
         ClassicAssert.AreEqual(200, enabled.status, enabled.body);
         JsonElement was = Parse(enabled.body);
         bool present = was.GetProperty("present").GetBoolean();
         string value = was.GetProperty("value").GetString();
         _putBack.Add(() =>
         {
            (int status, string body) back = present
               ? Admin("PUT", "/api/v1/settings/ini/QuarantineEnabled", "{\"value\":" + JsonSerializer.Serialize(value) + "}")
               : Admin("DELETE", "/api/v1/settings/ini/QuarantineEnabled", null);
            if (back.status != 200)
               throw new InvalidOperationException("QuarantineEnabled could not be put back: " + back.status + " " + back.body);
         });
         IniFileSetting.Write("QuarantineEnabled", "1");
         _application.Reinitialize();

         EmptyTheQuarantine();
         _putBack.Add(EmptyTheQuarantine);

         string baseUrlBefore = _settings.QuarantineDigestBaseUrl;
         _putBack.Add(() => _settings.QuarantineDigestBaseUrl = baseUrlBefore);
         _settings.QuarantineDigestBaseUrl = "http://127.0.0.1:" + RestPort;

         _domain.QuarantineDigest = domainOn;
         _domain.QuarantineDigestHour = domainHour;
         _domain.Save();
      }

      // The quarantine digest's own way of holding a message: the account blocks
      // a sender and the sender's mail is kept (QuarantineDigests).
      private void Hold(Account account, string subject)
      {
         (int status, string body) blocked = As(Basic(account.Address, Password), "POST", "/api/v1/me/sender-list",
            "{\"value\":\"" + Pest + "\",\"blocked\":true}");
         ClassicAssert.IsTrue(blocked.status == 201 || blocked.status == 200, blocked.body);

         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         int before = quarantine.Count;

         new SmtpClientSimulator().Send(Pest, account.Address, subject, "Nothing wrong with this message but its sender.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         DateTime until = DateTime.UtcNow.AddSeconds(30);
         while (true)
         {
            quarantine.Refresh();
            if (quarantine.Count == before + 1 || DateTime.UtcNow > until)
               break;
            Thread.Sleep(100);
         }

         ClassicAssert.AreEqual(before + 1, quarantine.Count, "The message was held.");
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      // ---- a browser session -------------------------------------------------

      /// <summary>POST /api/v1/session as the account: the cookie and the idle timeout it announced.</summary>
      private static (string cookie, long idle) SignIn(Account account)
      {
         (int status, string setCookie, string body) started = Raw("POST", "/api/v1/session", Basic(account.Address, Password), "User-Agent: Levels/1.0\r\n");
         ClassicAssert.AreEqual(201, started.status, started.body);

         string cookie = started.setCookie;
         int start = cookie.IndexOf('=') + 1;
         int end = cookie.IndexOf(';', start);
         cookie = end > start ? cookie.Substring(start, end - start) : cookie.Substring(start);

         return (cookie, Parse(started.body).GetProperty("idle_seconds").GetInt64());
      }

      private static (int status, string body) WithCookie(string cookie)
      {
         (int status, string setCookie, string body) answer = Raw("GET", "/api/v1/me", null, "Cookie: hmailsession=" + cookie + "\r\n");
         return (answer.status, answer.body);
      }

      /// <summary>One HTTP/1.0 request with extra header lines; the status, the Set-Cookie header and the body.</summary>
      private static (int status, string setCookie, string body) Raw(string method, string path, string authorization, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (extraHeaders != null)
                  head.Append(extraHeaders);
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string headers = separator >= 0 ? raw.Substring(0, separator) : raw;
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               string[] lines = headers.Split(new[] { "\r\n" }, StringSplitOptions.None);
               int status = 0;
               string[] first = lines[0].Split(' ');
               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               string setCookie = "";
               for (int i = 1; i < lines.Length; i++)
               {
                  if (lines[i].StartsWith("Set-Cookie:", StringComparison.OrdinalIgnoreCase))
                     setCookie = lines[i].Substring("Set-Cookie:".Length).Trim();
               }

               return (status, setCookie, body);
            }
         }
      }

      // ---- a backup and a restore (BackupAccountStores' way) -------------------

      private void RunBackup(string directory)
      {
         var backup = _application.Settings.Backup;

         // What the backup settings were, put back as they were found.
         bool domainsBefore = backup.BackupDomains;
         bool messagesBefore = backup.BackupMessages;
         bool settingsBefore = backup.BackupSettings;
         bool compressBefore = backup.CompressDestinationFiles;
         string destinationBefore = backup.Destination;
         _putBack.Add(() =>
         {
            var settings = _application.Settings.Backup;
            settings.BackupDomains = domainsBefore;
            settings.BackupMessages = messagesBefore;
            settings.BackupSettings = settingsBefore;
            settings.CompressDestinationFiles = compressBefore;
            settings.Destination = destinationBefore;
         });

         backup.BackupDomains = true;
         backup.BackupMessages = false;
         backup.BackupSettings = false;
         backup.CompressDestinationFiles = true;
         backup.Destination = directory;

         CustomAsserts.AssertDeleteFile(backup.LogFile);

         _application.BackupManager.StartBackup();

         for (int attempt = 0; attempt < 120; attempt++)
         {
            try
            {
               string log = TestSetup.ReadExistingTextFile(backup.LogFile);

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

      private void Restore(string directory)
      {
         string startTime = _application.Status.StartTime;

         // Status.StartTime has one-second resolution; step past the second.
         Thread.Sleep(1100);

         FileInfo[] archives = new DirectoryInfo(directory).GetFiles("*.7z");
         ClassicAssert.AreEqual(1, archives.Length, "Expected exactly one archive in " + directory);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = true;
         backup.RestoreMessages = false;
         backup.RestoreSettings = false;
         backup.StartRestore();

         for (int attempt = 0; attempt < 600; attempt++)
         {
            try
            {
               string now = _application.Status.StartTime;
               if (now.Length > 0 && now != startTime)
               {
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

      // ---- the checkout ------------------------------------------------------

      // python.exe from the PATH, skipping the Microsoft Store's stand-in.
      private static string FindPython()
      {
         foreach (string entry in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
         {
            string directory = entry.Trim().Trim('"');
            if (directory.Length == 0 || directory.IndexOf("WindowsApps", StringComparison.OrdinalIgnoreCase) >= 0)
               continue;

            try
            {
               string candidate = Path.Combine(directory, "python.exe");
               if (File.Exists(candidate))
                  return candidate;
            }
            catch (ArgumentException)
            {
               // A PATH entry with a character a path cannot hold.
            }
         }

         return null;
      }

      // A file of the checkout, found by walking up from the test binaries.
      private static string RepositoryFile(string relativePath)
      {
         for (DirectoryInfo directory = new DirectoryInfo(TestContext.CurrentContext.TestDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Combine(directory.FullName, relativePath);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("These binaries are not inside a checkout, so " + relativePath + " cannot be found.");
         return null;
      }
   }
}
