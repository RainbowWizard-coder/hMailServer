// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    Mailbox encryption at rest (docs/EncryptionAtRest.md): an account's
   ///    messages stored encrypted to an X25519 key of its own - the message
   ///    store's stage 3 - opened by a sign-in with its password, closed when it
   ///    has gone, and recovered with the domain's recovery key when the password
   ///    is lost.
   ///
   ///    The tests prove the file on disk and what a client sees, separately: the
   ///    container's header is read here, from the doc's description of it, and
   ///    the file's bytes are searched for a word of the message, while IMAP is
   ///    asked for the message after a password LOGIN. Where a behaviour has a
   ///    refusal there is a negative control beside it - a sibling mailbox that is
   ///    not encrypted, the account's own password beside the credential that is
   ///    refused, the wrong passphrase beside the right one, the key closed beside
   ///    the key open.
   ///
   ///    The routes go through REST (the listener on 10140; the row's range is
   ///    10140-10149). Every setting a test changes is read first and put back as
   ///    it was found in the teardown; every key a test makes is removed there
   ///    with the forfeit, and every recovery key with its route, so no policy is
   ///    left on; the accounts go with the test domain at the next SetUp. A test
   ///    that has to close every key restarts the service, which is the one
   ///    thing that closes them all at once; a test that has to see one key close
   ///    sets the idle minutes to one and waits.
   /// </summary>
   [TestFixture]
   public class EncryptionAtRest : TestFixtureBase
   {
      private static int RestPort = 10140;
      private const string AdminPassword = "testar";
      private const string Password = "test";
      private const string Passphrase = "a passphrase kept elsewhere";
      private const string ZeroAccessSalt = "AAECAwQFBgcICQoLDA0ODw==";
      private const string OAuthSecret = "encryption-at-rest-oauth-secret-0123456789";

      private static readonly byte[] Magic = { 0x89, 0x48, 0x4D, 0x53, 0x0D, 0x0A, 0x1A, 0x0A };

      // What the teardown undoes, last first: each setting put back as it was
      // found, each key forfeited, each recovery key removed.
      private readonly List<Action> putBack_ = new List<Action>();
      private readonly List<string> keyed_ = new List<string>();
      private readonly List<string> recoveryDomains_ = new List<string>();

      [SetUp]
      public void StartListener()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         putBack_.Clear();
         keyed_.Clear();
         recoveryDomains_.Clear();
      }

      [TearDown]
      public void PutEverythingBack()
      {
         try
         {
            if (!TestSetup.ConsoleServer && !ServiceRunning())
               Guarded(() => RestartServerAndReacquireCom());

            // No key left on and no recovery key left made: forfeited and
            // removed here, not left to the next SetUp's domain delete.
            foreach (string address in keyed_)
               Guarded(() => Admin("DELETE", KeyPath(address) + "?confirm=forfeit", null));
            foreach (string domain in recoveryDomains_)
               Guarded(() => Admin("DELETE", "/api/v1/domains/" + domain + "/mailbox-recovery", null));

            for (int i = putBack_.Count - 1; i >= 0; i--)
               Guarded(putBack_[i]);
         }
         finally
         {
            putBack_.Clear();
            keyed_.Clear();
            recoveryDomains_.Clear();
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---- the file and what a client sees ---------------------------------------------

      [Test]
      [Description("A message delivered to a mailbox encrypted at rest while its key is closed is stored as a container whose last stage is the encryption (id 3, 93 bytes of parameters naming the account's key), with no reference, no digest of the message and no byte of its words in the clear; a password LOGIN opens the key - the switch-on opened it too, and a restart closed it - and IMAP hands out the message as it arrived. Negative control: a sibling mailbox that is not encrypted stores the same message as it came.")]
      public void ADeliveredMessageIsStoredEncryptedAndReadBackAfterAPasswordLogon()
      {
         MakeRecoveryKey();
         string encrypted = AddAccount("atrest-on");
         string plain = AddAccount("atrest-off");
         JsonElement on = SwitchOn(encrypted);
         string fingerprint = on.GetProperty("fingerprint").GetString();
         ClassicAssert.AreEqual(16, fingerprint.Length, "The key's fingerprint: " + on);
         ClassicAssert.IsTrue(on.GetProperty("open").GetBoolean(), "The switch-on leaves the key open, as the doc says.");

         // Closed, so that what opens it below is the LOGIN.
         RestartServerAndReacquireCom();
         ClassicAssert.IsFalse(KeyState(encrypted).GetProperty("open").GetBoolean(), "A restart did not close the key.");

         string marker = Word("wombat");
         var smtp = new SmtpClientSimulator();
         smtp.Send("sender@example.test", encrypted, "Kept at rest " + marker, "The body carries " + marker + " and nothing else.");
         smtp.Send("sender@example.test", plain, "Kept at rest " + marker, "The body carries " + marker + " and nothing else.");
         WaitForMessages(encrypted, 1);
         WaitForMessages(plain, 1);

         byte[] file = File.ReadAllBytes(StoredFile(encrypted, 0));
         ClassicAssert.IsTrue(HasMagic(file), "The encrypted mailbox's message is not a container.");
         ContainerHeader header = Parse(file);
         ClassicAssert.IsTrue(header.Stages.Count >= 1 && header.Stages[header.Stages.Count - 1].id == 3,
            "The container's last stage is not the encryption (3): " + string.Join(",", header.Stages.Select(s => s.id)));
         ClassicAssert.AreEqual(93, header.Stages[header.Stages.Count - 1].parameters.Length, "The encryption stage's parameters are not 93 bytes.");
         ClassicAssert.AreEqual(1, header.Stages[header.Stages.Count - 1].parameters[0], "The encryption stage's version is not 1.");
         ClassicAssert.AreEqual(0, header.References, "An encrypted container names a part held once.");
         ClassicAssert.IsTrue(header.Sha256.All(b => b == 0), "An encrypted container carries a digest of its message.");
         ClassicAssert.IsFalse(Contains(file, marker), "A word of the message is on the disk in the clear.");
         ClassicAssert.IsFalse(Contains(file, "Kept at rest"), "The subject is on the disk in the clear.");
         ClassicAssert.IsFalse(KeyState(encrypted).GetProperty("open").GetBoolean(), "A delivery opened the key.");

         string fetched = FetchLiteral(encrypted, Password, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsNotNull(fetched, "IMAP handed out no message after a password LOGIN.");
         StringAssert.Contains("The body carries " + marker, fetched, "IMAP did not hand out the message as it arrived.");
         ClassicAssert.AreEqual((long) fetched.Length, header.OriginalSize, "The container's size is not the message's.");
         ClassicAssert.IsTrue(KeyState(encrypted).GetProperty("open").GetBoolean(), "The password LOGIN did not open the key.");

         // The negative control: the same message, not encrypted.
         byte[] plainFile = File.ReadAllBytes(StoredFile(plain, 0));
         ClassicAssert.IsFalse(HasMagic(plainFile), "A mailbox that is not encrypted stored a container.");
         ClassicAssert.IsTrue(Contains(plainFile, marker), "The sibling's message is not stored as it came.");
      }

      [Test]
      [Description("With MessageStoreCompression on, a keyed mailbox's message is deflated first and encrypted after (stages 1 then 3) and holds no part once, where the plain sibling's copy of the same message - its attachment over the de-duplication minimum - is deflated and names the attachment as a blob.")]
      public void CompressedFirstAndNoPartHeldOnce()
      {
         bool compressionFound = ReadSetting("message_store_compression").GetBoolean();
         putBack_.Add(() => Admin("PUT", "/api/v1/settings", "{\"message_store_compression\":" + (compressionFound ? "true" : "false") + "}"));
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"message_store_compression\":true}").status);

         MakeRecoveryKey();
         string keyed = AddAccount("atrest-deflate");
         string plain = AddAccount("atrest-deflate-plain");
         SwitchOn(keyed);

         // The plain sibling's blob goes with its account, at once, as the message
         // store's own fixture lets go of its blobs.
         putBack_.Add(() => DeleteAccount(plain));

         string word = Word("dunnock");
         string message = WithAttachment(keyed, "Deflated " + word, "The text beside it: " + word, 300);
         new SmtpClientSimulator().SendRaw("sender@example.test", keyed, message);
         new SmtpClientSimulator().SendRaw("sender@example.test", plain, WithAttachment(plain, "Deflated " + word, "The text beside it: " + word, 300));
         WaitForMessages(keyed, 1);
         WaitForMessages(plain, 1);

         ContainerHeader keyedHeader = Parse(File.ReadAllBytes(StoredFile(keyed, 0)));
         CollectionAssert.AreEqual(new[] { 1, 3 }, keyedHeader.Stages.Select(s => s.id).ToArray(), "The keyed mailbox's message is not deflated and then encrypted.");
         ClassicAssert.AreEqual(0, keyedHeader.References, "The keyed mailbox's message names a part held once.");

         byte[] plainFile = File.ReadAllBytes(StoredFile(plain, 0));
         ClassicAssert.IsTrue(HasMagic(plainFile), "The control: with compression on, the plain sibling's message is a container.");
         ContainerHeader plainHeader = Parse(plainFile);
         CollectionAssert.AreEqual(new[] { 1 }, plainHeader.Stages.Select(s => s.id).ToArray(), "The plain sibling's message is not deflated alone.");
         ClassicAssert.Greater(plainHeader.References, 0, "The control: the plain sibling's attachment is held once.");

         StringAssert.Contains(word, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));
      }

      [Test]
      [Description("An account's key needs its domain's recovery key first (409 without it, 201 with it); the recovery key cannot be removed while an account is sealed to it; a mailbox with a key is refused a zero-access switch-on, and a zero-access mailbox and one with a second factor enrolled are refused encryption at rest, each with its sentence, while a keyed mailbox is refused a second factor over COM. Negative controls: a sibling with no key is not refused zero access for that reason, and a plain account enrols a second factor.")]
      public void WhatComesFirstAndWhatIsNeverOnOneMailbox()
      {
         string keyed = AddAccount("atrest-first");
         string other = AddAccount("atrest-other");
         string sealedAccount = AddAccount("atrest-sealed");
         string twoFactor = AddAccount("atrest-totp");

         (int status, string body) early = Admin("PUT", KeyPath(keyed), "{\"enabled\":true}");
         ClassicAssert.AreEqual(409, early.status, early.body);
         StringAssert.Contains("recovery key", early.body);

         MakeRecoveryKey();
         SwitchOn(keyed);

         JsonElement recovery = Json(Admin("GET", RecoveryPath(), null), 200);
         ClassicAssert.IsTrue(recovery.GetProperty("exists").GetBoolean(), recovery.ToString());
         ClassicAssert.AreEqual(1, recovery.GetProperty("accounts").GetInt32(), recovery.ToString());
         ClassicAssert.AreEqual("the safe in the office", recovery.GetProperty("hint").GetString(), recovery.ToString());
         ClassicAssert.IsFalse(recovery.GetProperty("replacement_unfinished").GetBoolean(), recovery.ToString());

         (int status, string body) remove = Admin("DELETE", RecoveryPath(), null);
         ClassicAssert.AreEqual(409, remove.status, "A recovery key an account is sealed to was removed: " + remove.body);

         bool zeroAccessFound = Json(Admin("GET", "/api/v1/settings/openpgp", null), 200).GetProperty("zero_access_allowed").GetBoolean();
         putBack_.Add(() => Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":" + (zeroAccessFound ? "true" : "false") + "}"));
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":true}").status);

         (int status, string body) zero = Me(keyed, Password, "PUT", "/api/v1/me/zero-access", "{}");
         ClassicAssert.AreEqual(409, zero.status, zero.body);
         StringAssert.Contains("encrypted at rest", zero.body);

         (int status, string body) sibling = Me(other, Password, "PUT", "/api/v1/me/zero-access", "{}");
         ClassicAssert.AreNotEqual(409, sibling.status, "A mailbox with no key was refused zero access as if it had one: " + sibling.body);
         StringAssert.DoesNotContain("encrypted at rest", sibling.body);

         // The other way round: a zero-access mailbox is not encrypted at rest.
         SwitchZeroAccessOn(sealedAccount);
         (int status, string body) onZero = Admin("PUT", KeyPath(sealedAccount), "{\"enabled\":true}");
         ClassicAssert.AreEqual(409, onZero.status, onZero.body);
         StringAssert.Contains("zero-access", onZero.body);

         // A second factor and a key are never on one account, both ways round.
         Account totp = AccountOverCom(twoFactor);
         string uri = totp.EnrolTOTP();
         totp.Save();
         StringAssert.StartsWith("otpauth://", uri, "The control: a plain account enrols a second factor.");
         (int status, string body) onTotp = Admin("PUT", KeyPath(twoFactor), "{\"enabled\":true}");
         ClassicAssert.AreEqual(409, onTotp.status, onTotp.body);
         StringAssert.Contains("second factor", onTotp.body);

         Account keyedOverCom = AccountOverCom(keyed);
         var refused = Assert.Throws<COMException>(() => keyedOverCom.EnrolTOTP());
         StringAssert.Contains("encrypted at rest", refused.Message);
      }

      // ---- which sign-ins open the key ---------------------------------------------------

      [Test]
      [Description("A keyed mailbox signs in only with its own password: an app password is refused by IMAP (NO [UNAVAILABLE]) and POP3 (-ERR [SYS/PERM]) with the sentence, by the webmail's session start (403, mailbox_key), and on every /api/v1/me route over Basic - even with the key open, as it is after the switch-on. Negative controls: the account's password is let in by all of them, and an app password reads a mailbox that is not encrypted.")]
      public void OnlyTheAccountPasswordOpensAKeyedMailbox()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-apppw");
         string plain = AddAccount("atrest-apppw-plain");
         SwitchOn(keyed);

         string word = Word("quokka");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "Source " + word, "Read over REST: " + word);
         new SmtpClientSimulator().Send("sender@example.test", plain, "Source " + word, "Read over REST: " + word);
         WaitForMessages(keyed, 1);
         WaitForMessages(plain, 1);
         long keyedMessage = Inbox(keyed).Messages[0].ID;
         long plainMessage = Inbox(plain).Messages[0].ID;

         string keyedAppPassword = MakeAppPassword(keyed);
         string plainAppPassword = MakeAppPassword(plain);

         var imap = new ImapClientSimulator();
         ClassicAssert.IsFalse(imap.ConnectAndLogon(keyed, keyedAppPassword, out string imapError), "IMAP let an app password into a keyed mailbox.");
         StringAssert.Contains("encrypted at rest", imapError);
         StringAssert.Contains("[UNAVAILABLE]", imapError);
         imap.Disconnect();

         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsFalse(pop3.ConnectAndLogon(keyed, keyedAppPassword, out string popError), "POP3 let an app password into a keyed mailbox.");
         StringAssert.Contains("-ERR [SYS/PERM]", popError);
         StringAssert.Contains("encrypted at rest", popError);
         pop3.Disconnect();

         // Over REST: the key is open (the switch-on opened it), and still an app
         // password reads nothing of the mailbox, nor starts a session.
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean());
         (int status, string body) source = Me(keyed, keyedAppPassword, "GET", "/api/v1/me/messages/" + keyedMessage + "/source", null);
         ClassicAssert.AreEqual(403, source.status, "An app password read a keyed mailbox's message over REST: " + source.body);
         StringAssert.Contains("mailbox_key", source.body);
         StringAssert.DoesNotContain(word, source.body);

         (int status, string headers, string body) session = HttpRaw("POST", "/api/v1/session", Basic(keyed, keyedAppPassword), null, null);
         ClassicAssert.AreEqual(403, session.status, "An app password started a webmail session on a keyed mailbox: " + session.body);
         StringAssert.Contains("mailbox_key", session.body);

         // The controls.
         var imapOk = new ImapClientSimulator();
         ClassicAssert.IsTrue(imapOk.ConnectAndLogon(keyed, Password), "IMAP refused the account's own password.");
         imapOk.Logout();
         imapOk.Disconnect();

         var popOk = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(popOk.ConnectAndLogon(keyed, Password), "POP3 refused the account's own password.");
         popOk.QUIT();

         (int status, string body) own = Me(keyed, Password, "GET", "/api/v1/me/messages/" + keyedMessage + "/source", null);
         ClassicAssert.AreEqual(200, own.status, own.body);
         StringAssert.Contains(word, own.body);

         (int status, string body) plainSource = Me(plain, plainAppPassword, "GET", "/api/v1/me/messages/" + plainMessage + "/source", null);
         ClassicAssert.AreEqual(200, plainSource.status, "An app password did not read a mailbox that is not encrypted: " + plainSource.body);

         var imapPlain = new ImapClientSimulator();
         ClassicAssert.IsTrue(imapPlain.ConnectAndLogon(plain, plainAppPassword), "An app password did not open a mailbox that is not encrypted.");
         imapPlain.Logout();
         imapPlain.Disconnect();
      }

      [Test]
      [Description("An OAuth2 bearer token (AUTHENTICATE XOAUTH2) does not open a keyed mailbox, and is answered NO [UNAVAILABLE] with the sentence. Negative control: the same token form signs in to a mailbox that is not encrypted.")]
      public void ATokenIsRefusedByImapAuthenticate()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-token");
         string plain = AddAccount("atrest-token-plain");
         SwitchOn(keyed);

         // AUTHENTICATE answers only while SASL is on (IMAPCommandAuthenticate.cpp);
         // the bench leaves it off, so the refusal asserted below was never reached.
         bool saslFound = _application.Settings.IMAPSASLPlainEnabled;
         putBack_.Add(() => { _application.Settings.IMAPSASLPlainEnabled = saslFound; });
         _application.Settings.IMAPSASLPlainEnabled = true;

         SetIni("OAuth2Enabled", "1");
         SetIni("OAuth2RequireTLS", "0");
         SetIni("OAuth2AllowedAlgorithms", "HS256");
         SetIni("OAuth2HmacSecret", OAuthSecret);
         SetIni("OAuth2UsernameClaim", "email");
         SetIni("OAuth2Issuer", "");
         SetIni("OAuth2Audience", "");
         _application.Reinitialize();

         string refused = ImapXOAuth2(keyed, MintToken(keyed));
         StringAssert.Contains("NO", refused, "A token opened a keyed mailbox: " + refused);
         StringAssert.Contains("[UNAVAILABLE]", refused);
         StringAssert.Contains("encrypted at rest", refused);

         string accepted = ImapXOAuth2(plain, MintToken(plain));
         StringAssert.Contains("OK", accepted, "The control: a token did not sign in to a mailbox that is not encrypted: " + accepted);
      }

      [Test]
      [Description("The IMAP master user - AUTHENTICATE PLAIN with the keyed mailbox as the authorization identity - is refused NO [UNAVAILABLE] even while the owner's key is open, so it never holds that key open after the owner has gone. Negative control: the same master user signs in as a mailbox that is not encrypted.")]
      public void TheMasterUserIsNotLetIntoAKeyedMailbox()
      {
         MakeRecoveryKey();
         string master = AddAccount("atrest-master");
         string keyed = AddAccount("atrest-target");
         string plain = AddAccount("atrest-target-plain");
         SwitchOn(keyed);
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean(), "The switch-on did not leave the key open.");

         string masterFound = _application.Settings.IMAPMasterUser;
         bool saslFound = _application.Settings.IMAPSASLPlainEnabled;
         putBack_.Add(() => { _application.Settings.IMAPMasterUser = masterFound; _application.Settings.IMAPSASLPlainEnabled = saslFound; });
         _application.Settings.IMAPSASLPlainEnabled = true;
         _application.Settings.IMAPMasterUser = master;

         string refused = AuthenticateAs(keyed, master, Password);
         StringAssert.Contains("A01 NO", refused, "The master user was let into a keyed mailbox with its key open: " + refused);
         StringAssert.Contains("[UNAVAILABLE]", refused);

         string accepted = AuthenticateAs(plain, master, Password);
         StringAssert.Contains("A01 OK", accepted, "The control: the master user did not sign in as a mailbox that is not encrypted: " + accepted);
      }

      [Test]
      [Description("Two IMAP sessions share one key that their LOGINs opened, and hold it past the two idle minutes and a sweep; once both have gone it is still open seventy seconds on - a server closing an unheld key at the next sweep would have closed it - and an administrator's password set a hundred and five seconds on is not a use of it: the key is closed no later than the key of a second mailbox, the witness, whose owner's session went just after, and closed before a password set that kept it open would have let it close. Inconclusive, not passed, when the scheduler made no sweep between the witness's idle minutes and that point. A message delivered while it is closed is encrypted all the same and read at the next sign-in with the new password.")]
      public void TwoSessionsShareTheKeyWhichClosesOnceBothHaveGone()
      {
         SetSetting("mailbox_key_idle_minutes", "2");

         MakeRecoveryKey();
         string keyed = AddAccount("atrest-idle");
         SwitchOn(keyed);

         // The witness: a second mailbox whose owner's last session goes just
         // after keyed's last one, so that the sweep which closes its key is a
         // sweep the test can see (see below).
         string witness = AddAccount("atrest-idle-witness");
         SwitchOn(witness);

         // Closed, so that the LOGINs are what open them.
         RestartServerAndReacquireCom();
         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("open").GetBoolean(), "A restart did not close the key.");
         ClassicAssert.IsFalse(KeyState(witness).GetProperty("open").GetBoolean(), "A restart did not close the witness's key.");

         var one = new ImapClientSimulator();
         ClassicAssert.IsTrue(one.ConnectAndLogon(keyed, Password));
         var two = new ImapClientSimulator();
         ClassicAssert.IsTrue(two.ConnectAndLogon(keyed, Password));
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean(), "The key is not open after a password LOGIN.");

         var three = new ImapClientSimulator();
         ClassicAssert.IsTrue(three.ConnectAndLogon(witness, Password));
         ClassicAssert.IsTrue(KeyState(witness).GetProperty("open").GetBoolean(), "The witness's key is not open after a password LOGIN.");

         one.Logout();
         one.Disconnect();

         // Past the two idle minutes and at least one sweep: the second session
         // holds it.
         Thread.Sleep(TimeSpan.FromSeconds(135));
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean(), "The key closed while a session still held it.");

         two.Logout();
         two.Disconnect();
         DateTime gone = DateTime.UtcNow;

         // The witness's owner goes now, after keyed's. The server lets a key go
         // (MailboxKeyHold's destructor sets its last use) before it answers
         // LOGOUT, and Logout waits for that answer, so keyed's last use is no
         // later than the witness's.
         three.Logout();
         three.Disconnect();

         // The idle minutes count from the moment the last session let go: open
         // still, seventy seconds on - past one sweep for certain, so a server
         // closing an unheld key at the next sweep would have closed it.
         SleepUntil(gone.AddSeconds(70));
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean(), "The key closed before its idle minutes were up.");

         // An administrator sets a password a hundred and five seconds on, which
         // wraps the open key and is not a use of it.
         //
         // The arithmetic. The idle clock counts whole seconds (time()), and the
         // sweep (MailboxKeys::CloseIdle, the first thing RemoveExpiredRecords
         // does) closes a key once 120 of them have passed since its last use:
         // never sooner than 119 real seconds after it. The sweep runs once a
         // minute, but the Scheduler sleeps its minute AFTER it has run every task
         // that was due (Scheduler.cpp), and ScheduledTask::SetNextRunTime counts
         // the next run from the one that was due, so the sweep is due at every
         // tick and sweeps come 60 seconds plus the run time of the tick before
         // apart. A key last used at L closes somewhere between
         // L + 119 and L + 180 plus that run time, which has no bound: a fixed
         // deadline of 205 seconds failed once on bench150. So nothing below is
         // measured against a deadline.
         //
         // Instead the witness. One sweep reads the clock once and closes every
         // idle key under one lock, and keyed's last use is no later than the
         // witness's; so, unless something used keyed after the witness's owner
         // went, keyed closes at the sweep that closes the witness's key or
         // before it - however late that sweep comes. The only thing that
         // touches keyed after that is the password set. Read in that order - the
         // witness first, then keyed - keyed found open once the witness's key
         // has closed means the password set was a use.
         //
         // That tells the two apart only when the witness's key closes before a
         // password set that was a use would let keyed close. Such a set, sent at
         // setSent, would put keyed's last use at setSent or later and keep it
         // open to setSent + 119 at least; the witness's key closes at the first
         // sweep from about gone + 120. With the set at gone + 105 - some
         // fourteen seconds before keyed could close of itself, for the set's
         // own time - the window is gone + 120 to gone + 224, 104 seconds, which
         // holds a sweep unless the tick before it ran 44 seconds or more. When
         // it held none, this run cannot tell, and says so: Inconclusive, at the
         // end, after the rest of the test has run.
         SleepUntil(gone.AddSeconds(105));
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean(), "The key closed before its idle minutes were up.");
         const string newPassword = "Idle-Passw0rd-112!";
         DateTime setSent = DateTime.UtcNow;
         (int status, string body) reset = Admin("POST", "/api/v1/accounts/" + keyed + "/password", "{\"password\":\"" + newPassword + "\"}");
         ClassicAssert.AreEqual(200, reset.status, "An administrator's password set was refused while the key was open: " + reset.body);

         // The witness first, then keyed, every half second. Six minutes is not a
         // timing assertion: only that the sweep runs at all.
         bool keyedOpen = true;
         DateTime keyedRead = DateTime.MinValue;
         DateTime giveUp = gone.AddSeconds(360);
         while (true)
         {
            if (!KeyState(witness).GetProperty("open").GetBoolean())
            {
               keyedOpen = KeyState(keyed).GetProperty("open").GetBoolean();
               keyedRead = DateTime.UtcNow;
               break;
            }

            if (DateTime.UtcNow >= giveUp)
               Assert.Fail("The witness's key did not close within six minutes of its owner's last session: the idle sweep did not run.");
            Thread.Sleep(500);
         }

         ClassicAssert.IsFalse(keyedOpen,
            "The key was still open after the witness's had closed, though its owner had gone first - an administrator's password set kept it open.");

         // Read closed before setSent + 119, it was closed before a set that was
         // a use could have let it close. Read later, a sweep had not come in
         // time, and a set that was a use would have looked the same.
         bool told = keyedRead < setSent.AddSeconds(119);
         string untold = "No sweep came between the witness's idle minutes and " + (setSent.AddSeconds(119) - gone).TotalSeconds.ToString("0") +
            " seconds after the last session went (keyed was read closed at " + (keyedRead - gone).TotalSeconds.ToString("0") +
            " seconds): the scheduler's ticks ran long, and this run cannot tell whether the password set was a use of the key.";

         string marker = Word("numbat");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "While closed", "Delivered while the key was closed: " + marker);
         WaitForMessages(keyed, 1);
         ClassicAssert.IsFalse(Contains(File.ReadAllBytes(StoredFile(keyed, 0)), marker), "A message delivered while the key was closed is on the disk in the clear.");
         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("open").GetBoolean(), "A delivery opened the key.");

         string fetched = FetchLiteral(keyed, newPassword, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsNotNull(fetched, "The message delivered while the key was closed could not be read at the next sign-in with the password the administrator set.");
         StringAssert.Contains(marker, fetched);

         if (!told)
            Assert.Inconclusive(untold);
      }

      [Test]
      [Description("A delegate reading a shared folder of a mailbox encrypted at rest while its owner's key is closed is refused NO [UNAVAILABLE] for everything that reads the message - ENVELOPE, BODYSTRUCTURE, HEADER, TEXT, the whole - with no untagged FETCH, and a non-PEEK section does not set \\Seen. They were answered OK with NILs and empty strings, which a client caches by UID for good. What needs no file (UID, FLAGS, RFC822.SIZE) is still answered. Negative control: with the owner signed in the same FETCH hands the delegate the message.")]
      public void ADelegatesFetchWhileTheOwnersKeyIsClosedIsRefusedNotBlank()
      {
         MakeRecoveryKey();
         string owner = AddAccount("atrest-sharer");
         string reader = AddAccount("atrest-reader");
         SwitchOn(owner);

         // Closed, as it is while the owner is not signed in.
         RestartServerAndReacquireCom();
         ClassicAssert.IsFalse(KeyState(owner).GetProperty("open").GetBoolean(), "A restart did not close the key.");

         string marker = Word("quokka");
         new SmtpClientSimulator().Send("sender@example.test", owner, "Shared at rest " + marker, "The body carries " + marker + " and nothing else.");
         WaitForMessages(owner, 1);

         // Lookup, read and write-seen on the owner's INBOX (RFC 4314 l, r, s).
         _application.Database.ExecuteSQL(string.Format(
            "insert into hm_acl (aclsharefolderid, aclpermissiontype, aclpermissiongroupid, aclpermissionaccountid, aclvalue) values ({0}, 0, 0, {1}, {2})",
            Inbox(owner).ID, AccountOverCom(reader).ID, 1 | 2 | 4));

         string shared = "#Users." + owner + ".INBOX";

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(reader, Password));
         ClassicAssert.IsTrue(imap.SelectFolder(shared), "The delegate could not select " + shared);

         string noFile = imap.SendSingleCommand("F1 FETCH 1 (UID FLAGS RFC822.SIZE)");
         StringAssert.Contains("F1 OK", noFile, "What needs no file is answered while the key is closed: " + noFile);

         foreach (string item in new[] { "ENVELOPE", "BODYSTRUCTURE", "BODY.PEEK[HEADER]", "BODY.PEEK[TEXT]", "BODY.PEEK[]", "BODY[TEXT]" })
         {
            string answer = imap.SendSingleCommand("F2 FETCH 1 (" + item + ")");
            StringAssert.Contains("F2 NO [UNAVAILABLE]", answer, item + " of a message whose key is closed was not refused: " + answer);
            StringAssert.DoesNotContain("* 1 FETCH", answer, item + " sent something of a message it could not read: " + answer);
         }

         string flags = imap.SendSingleCommand("F3 FETCH 1 (FLAGS)");
         StringAssert.DoesNotContain("\\Seen", flags, "A refused BODY[TEXT] set \\Seen on the owner's message: " + flags);
         imap.Disconnect();
         ClassicAssert.IsFalse(KeyState(owner).GetProperty("open").GetBoolean(), "The delegate's reading opened the owner's key.");

         // The owner signed in: the key is open, and the delegate reads the message.
         var ownerSession = new ImapClientSimulator();
         ClassicAssert.IsTrue(ownerSession.ConnectAndLogon(owner, Password));
         try
         {
            ClassicAssert.IsTrue(KeyState(owner).GetProperty("open").GetBoolean(), "The owner's LOGIN did not open the key.");
            string text = FetchLiteral(reader, Password, shared, 1, "BODY.PEEK[TEXT]");
            ClassicAssert.IsNotNull(text, "The delegate could not read the message with the owner's key open.");
            StringAssert.Contains("The body carries " + marker, text);
         }
         finally
         {
            ownerSession.Disconnect();
         }
      }

      [Test]
      [Description("A webmail session lives under the opening of the key it was made under. Two sessions made while the key was open ask nothing while the idle minute closes it; a fresh sign-in with the password opens it again - a new opening - and then one stale session asks: it is refused (401) and not brought back to life by the open key, the other stale one is ended with it, and the fresh session - made a moment before the stale request - is not. Once the fresh session has gone quiet and the key has closed again, it is refused too. Negative control: the fresh session works before and after the stale one's request.")]
      public void AWebmailSessionWhoseKeyClosedIsEnded()
      {
         SetSetting("mailbox_key_idle_minutes", "1");

         MakeRecoveryKey();
         string keyed = AddAccount("atrest-session");
         SwitchOn(keyed);
         RestartServerAndReacquireCom();

         string stale = StartSession(keyed, Password);
         string staleToo = StartSession(keyed, Password);
         ClassicAssert.AreEqual(200, HttpRaw("GET", "/api/v1/me", null, null, stale).status, "The session did not work while the key was open.");
         ClassicAssert.AreEqual(200, HttpRaw("GET", "/api/v1/me", null, null, staleToo).status, "The second session did not work while the key was open.");

         // Nothing uses the key now: neither session makes a request.
         WaitFor(() => !KeyState(keyed).GetProperty("open").GetBoolean(), "The key did not close within the idle minute and the sweep.", 150);

         // A fresh sign-in opens the key again, under a new opening.
         string fresh = StartSession(keyed, Password);
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean(), "The fresh password sign-in did not open the key.");
         ClassicAssert.AreEqual(200, HttpRaw("GET", "/api/v1/me", null, null, fresh).status, "The control: a session started with the password did not work.");

         // The stale session asks now, with the key open: it was made under an
         // opening that has ended, and does not come back to life.
         ClassicAssert.AreEqual(401, HttpRaw("GET", "/api/v1/me", null, null, stale).status,
            "A session whose key had closed came back to life when a sign-in opened the key again.");

         // Its request ended the other session of that opening, and not the
         // fresh one, made a moment before it.
         ClassicAssert.AreEqual(200, HttpRaw("GET", "/api/v1/me", null, null, fresh).status, "A stale session's request ended the session signed in after the key opened again.");
         ClassicAssert.AreEqual(401, HttpRaw("GET", "/api/v1/me", null, null, staleToo).status, "A session of an earlier opening of the key was left standing.");

         // And a session whose key has closed is refused while it stays closed.
         WaitFor(() => !KeyState(keyed).GetProperty("open").GetBoolean(), "The key did not close again within the idle minute and the sweep.", 150);
         ClassicAssert.AreEqual(401, HttpRaw("GET", "/api/v1/me", null, null, fresh).status, "A session went on after its key closed.");
      }

      [Test]
      [Description("The switch-on ends every session the account had: an IMAP session signed in with an app password and idling - which sends nothing, so only the switch-on's drop can end it - is closed, an IMAP and a POP3 session are ended, and a webmail session made before it is refused (401) - each was signed in before the mailbox was encrypted, and the switch-on leaves the key open. Negative control: each worked just before the switch, and a sign-in with the password after it works.")]
      public void TheSwitchOnEndsTheSessionsSignedInBeforeIt()
      {
         MakeRecoveryKey();
         string account = AddAccount("atrest-before");
         string appPassword = MakeAppPassword(account);

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(account, appPassword), "The control: an app password did not sign in before the switch.");
         StringAssert.Contains("A01 OK", imap.SendSingleCommand("A01 NOOP"), "The control: the IMAP session did not answer before the switch.");

         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3.ConnectAndLogon(account, Password), "The control: POP3 did not sign in before the switch.");
         StringAssert.StartsWith("+OK", pop3.CAPA(), "The control: the POP3 session did not answer before the switch.");

         string cookie = StartSession(account, Password);
         ClassicAssert.AreEqual(200, HttpRaw("GET", "/api/v1/me", null, null, cookie).status, "The control: the webmail session did not work before the switch.");

         // Idling, with the app password: it sends no command after the switch.
         using (var idling = new TcpClient())
         {
            idling.Connect("127.0.0.1", TestPorts.Imap);
            idling.ReceiveTimeout = 15000;
            NetworkStream idle = idling.GetStream();
            ReadUntilText(idle, "* OK");
            WriteLine(idle, "I1 LOGIN \"" + account + "\" \"" + appPassword + "\"");
            StringAssert.Contains("I1 OK", ReadUntilText(idle, "I1 "), "The control: an app password did not sign the idling session in before the switch.");
            WriteLine(idle, "I2 SELECT INBOX");
            ReadUntilText(idle, "I2 ");
            WriteLine(idle, "I3 IDLE");
            ReadUntilText(idle, "+ ");

            SwitchOn(account);

            ClassicAssert.IsTrue(ClosedWithoutACommand(idle), "An idling IMAP session signed in with an app password before the switch-on was not closed by it.");
         }

         int command = 1;
         ClassicAssert.IsTrue(Ended(() => imap.Send("B" + (command++) + " NOOP")), "An IMAP session signed in with an app password before the switch-on went on reading the mailbox.");
         ClassicAssert.IsTrue(Ended(() => pop3.CAPA()), "A POP3 session signed in before the switch-on went on reading the mailbox.");
         ClassicAssert.AreEqual(401, HttpRaw("GET", "/api/v1/me", null, null, cookie).status, "A webmail session made before the switch-on went on.");

         // The control: a sign-in with the password after the switch works.
         var after = new ImapClientSimulator();
         ClassicAssert.IsTrue(after.ConnectAndLogon(account, Password), "A password sign-in after the switch-on was refused.");
         StringAssert.Contains("A01 OK", after.SendSingleCommand("A01 NOOP"));
         after.Logout();
         after.Disconnect();

         Guarded(() => imap.Disconnect());
         Guarded(() => pop3.Disconnect());
      }

      // ---- when the database cannot be read ------------------------------------------------

      [Test]
      [Description("With every read of the keys' table failing (SimulateDatabaseFailureFor) while the set of keyed accounts is in memory, nothing changes for any mailbox: a plain mailbox is delivered to and stored as it came, POP3 signs it in, and a keyed one's delivery is encrypted to the key the set holds. No message is refused, bounced or left queued.")]
      public void AFailedReadOfTheKeysChangesNothingForAnyMailbox()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-db-keyed");
         string plain = AddAccount("atrest-db-plain");
         SwitchOn(keyed);

         try
         {
            // Applied by a reinitialise, whose start reads the table again - and
            // fails, keeping the set it had.
            FailDatabaseStatementsContaining("from hm_accountkeys");

            string word = Word("pangolin");
            var smtp = new SmtpClientSimulator();
            smtp.Send("sender@example.test", plain, "Plain " + word, "Delivered while the keys could not be read: " + word);
            smtp.Send("sender@example.test", keyed, "Keyed " + word, "Delivered while the keys could not be read: " + word);
            WaitForMessages(plain, 1);
            WaitForMessages(keyed, 1);

            ClassicAssert.IsTrue(Contains(File.ReadAllBytes(StoredFile(plain, 0)), word), "A plain mailbox's message was not stored as it came while the keys could not be read.");
            AssertEncrypted(StoredFile(keyed, 0), word, "A keyed mailbox's message delivered while the keys could not be read");

            var pop3 = new Pop3ClientSimulator();
            ClassicAssert.IsTrue(pop3.ConnectAndLogon(plain, Password, out string popError), "POP3 refused a plain mailbox while the keys could not be read: " + popError);
            pop3.QUIT();

            ClassicAssert.AreEqual(0, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "A message was left in the queue, or a bounce made, while the keys could not be read.");
         }
         finally
         {
            FailDatabaseStatementsContaining("");
            LogHandler.ClearErrorLogUntilSettled();
         }
      }

      [Test]
      [Description("When the keys' table could not be read at the start and has not been since, whether any mailbox is encrypted is unknown, and unknown is temporary: a delivery to a plain mailbox waits in the queue - not delivered on a guess, not bounced, not discarded - IMAP refuses the sign-in NO [UNAVAILABLE], POP3 -ERR [SYS/TEMP], and the webmail's sign-in 503. Once the table reads again the same message is delivered, and nothing was bounced.")]
      public void WhenNothingIsKnownADeliveryWaitsAndSignInsAreRefusedForNow()
      {
         string plain = AddAccount("atrest-unknown");

         int triesFound = _settings.SMTPNoOfTries;
         putBack_.Add(() => _settings.SMTPNoOfTries = triesFound);
         _settings.SMTPNoOfTries = 10;

         string word = Word("tamandua");
         long queued = 0;

         IniFileSetting.Write("SimulateDatabaseFailureFor", "from hm_accountkeys");
         try
         {
            // The start reads the table, and cannot.
            RestartServerAndReacquireCom();

            var message = new hMailServer.Message();
            message.FromAddress = "sender@example.test";
            message.AddRecipient("", plain);
            message.Subject = "Held " + word;
            message.Body = "Held while nothing was known: " + word;
            message.Save();
            queued = message.ID;

            WaitFor(() => LogHandler.ReadCurrentDefaultLog().Contains("not delivered to " + plain + " yet"),
               "The delivery was not attempted, or not held, while nothing was known of the keys.", 60);

            ClassicAssert.AreEqual(0, Inbox(plain).Messages.Count, "A message was delivered on a guess while nothing was known of the keys.");
            ClassicAssert.AreEqual(1, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "The held message is not in the queue - bounced or discarded.");

            var imap = new ImapClientSimulator();
            ClassicAssert.IsFalse(imap.ConnectAndLogon(plain, Password, out string imapError), "IMAP signed in while nothing was known of the keys.");
            StringAssert.Contains("[UNAVAILABLE]", imapError);
            StringAssert.Contains("could not be read", imapError);
            imap.Disconnect();

            var pop3 = new Pop3ClientSimulator();
            ClassicAssert.IsFalse(pop3.ConnectAndLogon(plain, Password, out string popError), "POP3 signed in while nothing was known of the keys.");
            StringAssert.Contains("-ERR [SYS/TEMP]", popError);
            pop3.Disconnect();

            (int status, string headers, string body) session = StartSessionRaw(plain, Password);
            ClassicAssert.AreEqual(503, session.status, "The webmail's sign-in was not refused for now while nothing was known of the keys: " + session.body);
         }
         finally
         {
            FailDatabaseStatementsContaining("");
         }

         // Read again: the same message is delivered.
         for (int attempt = 0; attempt < 10 && Inbox(plain).Messages.Count == 0; attempt++)
         {
            var queue = _application.GlobalObjects.DeliveryQueue;
            queue.ResetDeliveryTime(queued);
            queue.StartDelivery();
            Thread.Sleep(1000);
         }

         WaitForMessages(plain, 1);
         StringAssert.Contains("Held while nothing was known: " + word, FetchLiteral(plain, Password, "INBOX", 1, "BODY.PEEK[]"));
         ClassicAssert.AreEqual(0, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "A bounce was queued for a message held while nothing was known.");
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      [Description("A key row that holds no key is damage, answered at once and for good, for its own account and no other: one message to it and to a plain mailbox is delivered to the plain one and bounced for it at once - 4.3.0, to a sender this server hosts with the sentence naming the forfeit, nothing held in the queue - and IMAP (NO [CONTACTADMIN]), POP3 (-ERR [SYS/PERM]), the webmail's sign-in (409) and the switch-off refuse it with the same sentence. The forfeit removes the row, and mail is then stored as it comes.")]
      public void AKeyRowThatHoldsNoKeyIsRefusedAtOnceUntilItIsForfeited()
      {
         MakeRecoveryKey();
         string damaged = AddAccount("atrest-damaged");
         string plain = AddAccount("atrest-damaged-plain");
         string sender = AddAccount("atrest-damaged-sender");
         SwitchOn(damaged);
         long damagedId = AccountOverCom(damaged).ID;

         _application.Database.ExecuteSQL("update hm_accountkeys set akpublic = 'AAAA' where akaccountid = " + damagedId);
         _application.Reinitialize();

         try
         {
            string word = Word("kinkajou");
            var message = new hMailServer.Message();
            message.FromAddress = sender;
            message.AddRecipient("", damaged);
            message.AddRecipient("", plain);
            message.Subject = "Damaged " + word;
            message.Body = "To a row with no key: " + word;
            message.Save();

            WaitForMessages(plain, 1);

            // Bounced at once, with the way out named - to this sender, whose
            // domain the server hosts; one outside is told only that the
            // mailbox cannot take mail now (not tested here). Not held.
            WaitForMessages(sender, 1);
            string report = FetchLiteral(sender, Password, "INBOX", 1, "BODY.PEEK[]");
            ClassicAssert.IsNotNull(report, "The report to the sender could not be read.");
            StringAssert.Contains(damaged, report);
            // The bounce's text part is quoted-printable: its "=" is =3D and its long
            // lines end in soft breaks, so the raw message never holds the phrase.
            string reportText = report.Replace("=\r\n", "").Replace("=3D", "=");
            StringAssert.Contains("confirm=forfeit", reportText, "The report does not name the forfeit.");
            ClassicAssert.AreEqual(0, Inbox(damaged).Messages.Count, "A message was stored for an account whose key row holds no key.");
            ClassicAssert.AreEqual(0, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "The damaged recipient's message was held in the queue rather than bounced at once.");

            var imap = new ImapClientSimulator();
            ClassicAssert.IsFalse(imap.ConnectAndLogon(damaged, Password, out string imapError), "IMAP signed in to an account whose key row holds no key.");
            StringAssert.Contains("[CONTACTADMIN]", imapError, "IMAP answered damage with a code that says to try again.");
            StringAssert.Contains("confirm=forfeit", imapError);
            StringAssert.DoesNotContain("Try again in a moment", imapError, "Damage was answered as a temporary refusal.");
            imap.Disconnect();

            var pop3 = new Pop3ClientSimulator();
            ClassicAssert.IsFalse(pop3.ConnectAndLogon(damaged, Password, out string popError), "POP3 signed in to an account whose key row holds no key.");
            StringAssert.Contains("-ERR [SYS/PERM]", popError);
            StringAssert.Contains("confirm=forfeit", popError);
            pop3.Disconnect();

            (int status, string headers, string body) session = StartSessionRaw(damaged, Password);
            ClassicAssert.AreEqual(409, session.status, "The webmail's sign-in to a damaged row was not refused as permanent: " + session.body);
            StringAssert.Contains("confirm=forfeit", session.body);

            (int status, string body) off = Admin("PUT", KeyPath(damaged), "{\"enabled\":false}");
            ClassicAssert.AreEqual(409, off.status, "Switching a damaged row off was not refused naming the forfeit: " + off.body);
            StringAssert.Contains("confirm=forfeit", off.body);

            ClassicAssert.IsTrue(KeyState(damaged).GetProperty("damaged").GetBoolean(), "The account's state does not say its key row is damaged.");

            // The forfeit removes it, and mail is stored as it comes again.
            (int status, string body) forfeited = Admin("DELETE", KeyPath(damaged) + "?confirm=forfeit", null);
            ClassicAssert.AreEqual(200, forfeited.status, "The forfeit did not remove a damaged key row: " + forfeited.body);
            keyed_.Remove(damaged);
            ClassicAssert.IsFalse(KeyState(damaged).GetProperty("keyed").GetBoolean(), "The damaged row is still there after the forfeit.");

            string after = Word("olingo");
            new SmtpClientSimulator().Send(sender, damaged, "After " + after, "Stored as it comes: " + after);
            WaitForMessages(damaged, 1);
            ClassicAssert.IsTrue(Contains(File.ReadAllBytes(StoredFile(damaged, 0)), after), "Mail to the account after the forfeit is not stored as it comes.");
         }
         finally
         {
            _application.Database.ExecuteSQL("delete from hm_accountkeys where akaccountid = " + damagedId);
            _application.Reinitialize();
            LogHandler.ClearErrorLogUntilSettled();
         }
      }

      // ---- what a crash leaves -----------------------------------------------------------

      [Test]
      [Description("The start's sweep reads file names only: keyed mailboxes named x.plaintext-y and x.encrypting-y, beside a mailbox named x whose directory exists, keep every message across a restart, byte for byte and readable - where a sweep matching its marks anywhere in the path took every one of them for a leftover, moved the first out to x's directory and zeroed or removed the rest.")]
      public void MailboxesNamedWithTheSweepsMarksKeepTheirMail()
      {
         MakeRecoveryKey();
         string plainMarked = AddAccount("x.plaintext-y");
         string stagedMarked = AddAccount("x.encrypting-y");
         string sibling = AddAccount("x");
         SwitchOn(plainMarked);
         SwitchOn(stagedMarked);

         // The sibling's directory exists, which is what made the old sweep
         // take the marked mailboxes' messages for leftovers of x's.
         var smtp = new SmtpClientSimulator();
         smtp.Send("sender@example.test", sibling, "Sibling", "The sibling's own.");
         WaitForMessages(sibling, 1);

         var words = new Dictionary<string, string[]>();
         foreach (string address in new[] { plainMarked, stagedMarked })
         {
            string first = Word("marked");
            string second = Word("marked");
            smtp.Send("sender@example.test", address, "First", "The first: " + first);
            WaitForMessages(address, 1);
            smtp.Send("sender@example.test", address, "Second", "The second: " + second);
            WaitForMessages(address, 2);
            words[address] = new[] { first, second };
         }

         var files = new Dictionary<string, byte[]>();
         foreach (string address in new[] { plainMarked, stagedMarked })
         {
            for (int i = 0; i < 2; i++)
            {
               string path = StoredFile(address, i);
               files[path] = File.ReadAllBytes(path);
            }
         }

         RestartServerAndReacquireCom();

         foreach (KeyValuePair<string, byte[]> file in files)
         {
            ClassicAssert.IsTrue(File.Exists(file.Key), "The start's sweep removed a message of a mailbox named with one of its marks: " + file.Key);
            CollectionAssert.AreEqual(file.Value, File.ReadAllBytes(file.Key), "The start's sweep changed a message of a mailbox named with one of its marks: " + file.Key);
         }

         foreach (string address in new[] { plainMarked, stagedMarked })
         {
            ClassicAssert.AreEqual(2, Inbox(address).Messages.Count, "The mailbox " + address + " lost a message across the restart.");
            StringAssert.Contains("The first: " + words[address][0], FetchLiteral(address, Password, "INBOX", 1, "BODY.PEEK[]"));
            StringAssert.Contains("The second: " + words[address][1], FetchLiteral(address, Password, "INBOX", 2, "BODY.PEEK[]"));
         }

         ClassicAssert.AreEqual(1, Inbox(sibling).Messages.Count, "The sibling mailbox's messages changed across the restart.");
      }

      [Test]
      [Description("What an encryption cut short by a crash leaves beside a keyed mailbox's files is put right at the next start: with the name empty, the container staged beside it is renamed into it and the plaintext moved aside is removed; a plaintext beside a name that holds its message is removed; and a plaintext beside a name nothing holds is put back under the name as it was, never overwritten. The mail reads as before.")]
      public void WhatACrashLeftBesideAKeyedMailboxIsPutRightAtTheStart()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-crash");
         SwitchOn(keyed);

         string first = Word("aardwolf");
         string second = Word("binturong");
         var smtp = new SmtpClientSimulator();
         smtp.Send("sender@example.test", keyed, "First", "The first: " + first);
         WaitForMessages(keyed, 1);
         smtp.Send("sender@example.test", keyed, "Second", "The second: " + second);
         WaitForMessages(keyed, 2);

         string firstFile = StoredFile(keyed, 0);
         string secondFile = StoredFile(keyed, 1);
         byte[] firstContainer = File.ReadAllBytes(firstFile);
         byte[] secondContainer = File.ReadAllBytes(secondFile);

         // Between the two renames: the name empty, the container staged beside
         // it and the plaintext moved aside.
         string staged = firstFile + ".encrypting-" + Guid.NewGuid().ToString("B").ToUpperInvariant();
         File.Move(firstFile, staged);
         string firstAside = firstFile + ".plaintext-" + Guid.NewGuid().ToString("B").ToUpperInvariant();
         File.WriteAllText(firstAside, "Subject: First\r\n\r\nThe first: " + first + "\r\n");

         // After them: the name holds the message, and the plaintext is still beside it.
         string secondAside = secondFile + ".plaintext-" + Guid.NewGuid().ToString("B").ToUpperInvariant();
         File.WriteAllText(secondAside, "Subject: Second\r\n\r\nThe second: " + second + "\r\n");

         // A plaintext beside a name nothing holds: the only copy there is.
         string orphan = Path.Combine(Path.GetDirectoryName(firstFile), "orphan-" + Guid.NewGuid().ToString("N") + ".eml");
         string orphanAside = orphan + ".plaintext-" + Guid.NewGuid().ToString("B").ToUpperInvariant();
         const string orphanText = "Subject: Orphan\r\n\r\nNothing else holds this.\r\n";
         File.WriteAllText(orphanAside, orphanText);

         try
         {
            RestartServerAndReacquireCom();

            ClassicAssert.IsTrue(File.Exists(firstFile), "The container staged beside an empty name was not renamed into it.");
            CollectionAssert.AreEqual(firstContainer, File.ReadAllBytes(firstFile), "The name does not hold the staged container.");
            ClassicAssert.IsFalse(File.Exists(staged), "The staged container is still beside the name.");
            ClassicAssert.IsFalse(File.Exists(firstAside), "The plaintext moved aside was left beside the name the container went back into.");
            ClassicAssert.IsFalse(File.Exists(secondAside), "A plaintext beside a stored message was left for good.");
            CollectionAssert.AreEqual(secondContainer, File.ReadAllBytes(secondFile), "The stored message beside the leftover plaintext changed.");

            ClassicAssert.IsTrue(File.Exists(orphan), "A plaintext nothing else holds was not put back under its name.");
            ClassicAssert.AreEqual(orphanText, File.ReadAllText(orphan), "A plaintext nothing else holds was not put back as it was.");
            ClassicAssert.IsFalse(File.Exists(orphanAside));

            StringAssert.Contains("The first: " + first, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));
            StringAssert.Contains("The second: " + second, FetchLiteral(keyed, Password, "INBOX", 2, "BODY.PEEK[]"));
         }
         finally
         {
            foreach (string leftover in new[] { orphan, orphanAside, staged, firstAside, secondAside })
               Guarded(() => { if (File.Exists(leftover)) File.Delete(leftover); });
         }
      }

      [Test]
      [Description("The switch-on holds the key for the first sign-in (pending); a restart closes every key; the next sign-in with the password opens it and wraps it by the password, which removes the pending copy; the mail is read as it was. Negative control: before that sign-in the key is closed and the file on disk is the same ciphertext.")]
      public void ARestartClosesTheKeyAndTheNextPasswordSignInOpensIt()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-restart");
         SwitchOn(keyed);
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("pending").GetBoolean(), "The switch-on did not hold the key for the first sign-in.");

         string marker = Word("quoll");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "Before the restart", "Kept across a restart: " + marker);
         WaitForMessages(keyed, 1);

         StringAssert.Contains(marker, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));
         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("pending").GetBoolean(), "The first password sign-in did not wrap the key.");

         string path = StoredFile(keyed, 0);
         byte[] before = File.ReadAllBytes(path);

         RestartServerAndReacquireCom();

         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("open").GetBoolean(), "A restart did not close the key.");
         CollectionAssert.AreEqual(before, File.ReadAllBytes(path), "The file changed across the restart.");

         string fetched = FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsNotNull(fetched, "The password did not open the key after a restart.");
         StringAssert.Contains(marker, fetched);
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean());
      }

      // ---- the index, and what reading puts on the disk ----------------------------------

      [Test]
      [Description("SEARCH finds a word of an encrypted mailbox while its owner is signed in, by reading the messages - and the reading puts no copy of them in the temporary directory; the full-text index holds none of its words, so with a 1 MB examine ceiling over 2.1 MB of mail a word it does not hold is still read for, and refused on the ceiling - where the sibling that is not encrypted answers the same search from its index without reading a byte. The sort's copy of the headers is not written for it either.")]
      public void SearchReadsAnEncryptedMailboxWhileSignedInAndTheIndexHoldsNoneOfIt()
      {
         int originalMaxMessageSize = _application.Settings.MaxMessageSize;
         putBack_.Add(() => _application.Settings.MaxMessageSize = originalMaxMessageSize);

         _application.Settings.MaxMessageSize = 0;
         _domain.MaxMessageSize = 0;
         _domain.Save();

         MakeRecoveryKey();
         string keyed = AddAccount("atrest-search");
         string plain = AddAccount("atrest-search-plain");
         SwitchOn(keyed);

         string word = Word("bilby");
         var smtp = new SmtpClientSimulator();
         for (int i = 1; i <= 3; i++)
         {
            smtp.Send("sender@example.test", keyed, "Search " + i, LargeBody(700 * 1024, word));
            smtp.Send("sender@example.test", plain, "Search " + i, LargeBody(700 * 1024, word));
         }
         WaitForMessages(keyed, 3);
         WaitForMessages(plain, 3);

         string temp = _application.Settings.Directories.TempDirectory;

         // While signed in, the word is found by reading the messages - and the
         // reading creates nothing in the temporary directory, not even a copy
         // written and deleted again, which a FileSystemWatcher sees and a
         // listing afterwards would not: every message SEARCH parsed was parsed
         // in memory.
         List<string> created = CreatedWhile(temp, () =>
         {
            var finder = new ImapClientSimulator();
            ClassicAssert.IsTrue(finder.ConnectAndLogon(keyed, Password));
            ClassicAssert.IsTrue(finder.SelectFolder("INBOX"));
            ClassicAssert.AreEqual("1 2 3", finder.Search("BODY " + word), "SEARCH did not find the word in the encrypted mailbox while its owner was signed in.");
            finder.Logout();
            finder.Disconnect();
         });
         ClassicAssert.AreEqual(0, created.Count, "SEARCH over an encrypted mailbox created files in the temporary directory: " + string.Join(", ", created));

         SetIni("IMAPSearchTimeout", "60");
         SetIni("IMAPSearchMaxMegabytes", "1");
         SetIni("IndexerFullText", "1");
         _application.Reinitialize();

         var indexing = _application.Settings.MessageIndexing;
         bool indexingFound = indexing.Enabled;
         putBack_.Add(() => _application.Settings.MessageIndexing.Enabled = indexingFound);
         indexing.Enabled = true;
         indexing.Index();

         // The sibling's words are indexed: a word it does not hold is answered
         // from the index, reading nothing, under the ceiling.
         RetryHelper.TryAction(TimeSpan.FromSeconds(60), () =>
         {
            _application.Settings.MessageIndexing.Index();
            string absent = SearchRaw(plain, "A01 SEARCH BODY \"AbsentAtRestNeedle\"");
            ClassicAssert.IsTrue(absent.Contains("* SEARCH\r\n") && absent.Contains("A01 OK"),
               "The sibling's index did not answer an absent word under the ceiling: " + absent);
         });

         // The encrypted mailbox's words are not: the same search has to read
         // every message, and the ceiling refuses it.
         string keyedAbsent = SearchRaw(keyed, "A01 SEARCH BODY \"AbsentAtRestNeedle\"");
         StringAssert.Contains("A01 NO", keyedAbsent, "The index answered for an encrypted mailbox, so it holds its words: " + keyedAbsent);
         StringAssert.Contains("[LIMIT]", keyedAbsent);

         // The sort's copy of the headers (TotalIndexedCount counts its rows):
         // the sibling's three messages, and not the encrypted mailbox's.
         WaitFor(() =>
         {
            _application.Settings.MessageIndexing.Index();
            long waitingTotal = indexing.TotalMessageCount;
            long waitingIndexed = indexing.TotalIndexedCount;
            return waitingTotal - waitingIndexed <= 3;
         }, "The sibling's messages were not given the sort's copy of their headers.", 60);
         indexing.Index();
         Thread.Sleep(TimeSpan.FromSeconds(5));
         long total = indexing.TotalMessageCount;
         long indexed = indexing.TotalIndexedCount;
         ClassicAssert.AreEqual(3, total - indexed, "The sort's copy of the headers was written for the encrypted mailbox's messages (total " + total + ", indexed " + indexed + ").");
      }

      // ---- the writers -------------------------------------------------------------------

      [Test]
      [Description("Every writer into a keyed mailbox encrypts: a message COM adds to its INBOX (Messages.Add, then Save), an IMAP APPEND, and a COM rewrite of a stored message (Message.Save) are each stored as an encryption container with no word in the clear, and read back as written; nothing of them is left in the temporary directory. Negative control: the same COM add to a mailbox that is not encrypted is stored as it was written.")]
      public void EveryWriterIntoAKeyedMailboxEncrypts()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-writers");
         string plain = AddAccount("atrest-writers-plain");
         SwitchOn(keyed);

         string temp = _application.Settings.Directories.TempDirectory;
         DateTime from = DateTime.UtcNow.AddSeconds(-2);

         // COM: a new message for the INBOX.
         string added = Word("potto");
         Message message = Inbox(keyed).Messages.Add();
         message.Subject = "Added " + added;
         message.Body = "Added over COM: " + added;
         message.Save();

         Message control = Inbox(plain).Messages.Add();
         control.Subject = "Added " + added;
         control.Body = "Added over COM: " + added;
         control.Save();

         WaitForMessages(keyed, 1);
         AssertEncrypted(StoredFile(keyed, 0), added, "A message COM added to a keyed mailbox");
         ClassicAssert.IsTrue(Contains(File.ReadAllBytes(StoredFile(plain, 0)), added), "The control: COM's message to a plain mailbox is not stored as written.");
         StringAssert.Contains("Added over COM: " + added, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));

         // COM: the stored message rewritten, which is read from the container
         // in memory and written back encrypted.
         string changed = Word("tarsier");
         Message stored = Inbox(keyed).Messages[0];
         stored.Subject = "Changed " + changed;
         stored.Save();
         AssertEncrypted(StoredFile(keyed, 0), changed, "A COM rewrite of a keyed mailbox's message");
         StringAssert.Contains("Changed " + changed, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));

         // IMAP APPEND.
         string appended = Word("galago");
         string raw = "From: a@example.test\r\nTo: " + keyed + "\r\nSubject: Appended " + appended + "\r\n\r\nAppended over IMAP: " + appended + "\r\n";
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(keyed, Password));
         StringAssert.Contains("OK", imap.SendSingleCommandWithLiteral("A01 APPEND INBOX {" + raw.Length + "}", raw));
         imap.Disconnect();
         WaitForMessages(keyed, 2);
         AssertEncrypted(StoredFile(keyed, 1), appended, "An APPEND to a keyed mailbox");
         StringAssert.Contains("Appended over IMAP: " + appended, FetchLiteral(keyed, Password, "INBOX", 2, "BODY.PEEK[]"));

         foreach (string word in new[] { added, changed, appended })
         {
            string[] copies = FilesWritten(temp, from).Where(f => Contains(File.ReadAllBytes(f), word)).ToArray();
            ClassicAssert.AreEqual(0, copies.Length, "A keyed mailbox's message was left in the clear in the temporary directory: " + string.Join(", ", copies));
         }
      }

      [Test]
      [Description("A webmail draft with an attachment, saved to a keyed mailbox, is stored as an encryption container with no word of its text or its attachment in the clear, and the save creates no file in the temporary directory - the attachment is made into its part in memory, where it was written to a directory of its own there and deleted. The draft reads back with its attachment.")]
      public void ADraftsAttachmentNeverReachesTheTemporaryDirectory()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-draft");
         SwitchOn(keyed);

         string word = Word("draftling");
         string attachmentName = "note-" + word + ".txt";
         string data = Convert.ToBase64String(Encoding.ASCII.GetBytes("The attachment says " + word));
         string temp = _application.Settings.Directories.TempDirectory;

         (int status, string body) saved = (0, null);
         List<string> created = CreatedWhile(temp, () =>
            saved = Me(keyed, Password, "POST", "/api/v1/me/drafts",
               "{\"to\":\"someone@example.test\",\"subject\":\"Draft " + word + "\",\"text\":\"The text of " + word + "\"," +
               "\"attachments\":[{\"name\":\"" + attachmentName + "\",\"type\":\"text/plain\",\"data\":\"" + data + "\"}]}"));

         ClassicAssert.AreEqual(201, saved.status, saved.body);
         string[] attachmentCopies = created.Where(path => path.IndexOf(word, StringComparison.OrdinalIgnoreCase) >= 0).ToArray();
         ClassicAssert.AreEqual(0, attachmentCopies.Length, "The draft's attachment was written to the temporary directory: " + string.Join(", ", created));

         WaitFor(() => AccountOverCom(keyed).IMAPFolders.get_ItemByName("Drafts").Messages.Count == 1, "The draft was not stored in Drafts.", 30);
         string draftFile = AccountOverCom(keyed).IMAPFolders.get_ItemByName("Drafts").Messages[0].Filename;
         AssertEncrypted(draftFile, word, "A draft with an attachment");
         ClassicAssert.IsFalse(Contains(File.ReadAllBytes(draftFile), data), "The draft's attachment is on the disk in the clear.");

         string fetched = FetchLiteral(keyed, Password, "Drafts", 1, "BODY.PEEK[]");
         ClassicAssert.IsNotNull(fetched, "The draft could not be read back.");
         StringAssert.Contains(data, fetched, "The draft's attachment did not read back.");
         StringAssert.Contains(attachmentName, fetched);
      }

      [Test]
      [Description("A copy out of a keyed mailbox into a public folder is stored there as it arrived - a public folder is nobody's mailbox - and a copy from the public folder into the keyed mailbox is encrypted to its key on the way in.")]
      public void ACopyIsStoredAsTheMailboxItGoesIntoStoresMail()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-copy");
         SwitchOn(keyed);
         Account keyedAccount = AccountOverCom(keyed);

         IMAPFolder share = _application.Settings.PublicFolders.Add("AtRestShare");
         share.Save();
         IMAPFolderPermission permission = share.Permissions.Add();
         permission.PermissionAccountID = keyedAccount.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionLookup, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.Save();

         string word = Word("aye-aye");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "Copied " + word, "Copied between folders: " + word);
         WaitForMessages(keyed, 1);

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(keyed, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         ClassicAssert.IsTrue(imap.Copy(1, "#Public.AtRestShare"), "COPY into the public folder was refused.");

         WaitFor(() => PublicFolder("AtRestShare").Messages.Count == 1, "The copy did not reach the public folder.", 30);
         byte[] publicFile = File.ReadAllBytes(PublicFolder("AtRestShare").Messages[0].Filename);
         ClassicAssert.IsFalse(HasMagic(publicFile) && Parse(publicFile).Stages.Any(s => s.id == 3), "A public folder's copy was left under the owner's key.");
         ClassicAssert.IsTrue(Contains(publicFile, word), "The public folder's copy is not the message as it arrived.");

         // And back: into the keyed mailbox, encrypted.
         ClassicAssert.IsTrue(imap.SelectFolder("#Public.AtRestShare"));
         ClassicAssert.IsTrue(imap.Copy(1, "INBOX"), "COPY into the keyed mailbox was refused.");
         imap.Logout();
         imap.Disconnect();

         WaitForMessages(keyed, 2);
         AssertEncrypted(StoredFile(keyed, 1), word, "A copy into a keyed mailbox");
      }

      [Test]
      [Description("An import into a keyed mailbox while its key is closed (Utilities.ImportMessageFromFile, the Data Directory Synchronizer's and the Import Tool's door) reads the message before it is encrypted, gives it its row, and stores it encrypted, readable at the next sign-in with the password.")]
      public void AnImportWithTheKeyClosedIsReadThenEncrypted()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-import");
         SwitchOn(keyed);

         RestartServerAndReacquireCom();
         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("open").GetBoolean());

         // Read after the restart: a COM object from before it belongs to the
         // process that was.
         Account account = AccountOverCom(keyed);

         // The domain from the address, not from _domain: a COM object from
         // before the restart belongs to the process that was, and every call
         // on it fails.
         string word = Word("fossa");
         string domainName = keyed.Substring(keyed.IndexOf('@') + 1);
         string directory = Paths.Combine(Paths.Combine(_application.Settings.Directories.DataDirectory, domainName), keyed.Substring(0, keyed.IndexOf('@')));
         Directory.CreateDirectory(directory);
         string fileName = Paths.Combine(directory, "imported-" + word + ".eml");
         File.WriteAllText(fileName, "From: importer@example.test\r\nTo: " + keyed + "\r\nSubject: Imported " + word + "\r\n\r\nImported with the key closed: " + word + "\r\n");

         ClassicAssert.IsTrue(_application.Utilities.ImportMessageFromFile(fileName, account.ID), "The import into a keyed mailbox with its key closed failed.");
         WaitForMessages(keyed, 1);
         AssertEncrypted(StoredFile(keyed, 0), word, "An imported message");
         ClassicAssert.AreEqual("importer@example.test", Inbox(keyed).Messages[0].FromAddress, "The row was saved without the sender read before the file was encrypted.");

         StringAssert.Contains("Imported with the key closed: " + word, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));
      }

      [Test]
      [Description("A keyed mailbox's deleted items are kept as its mail is: a message deleted after the switch-on is kept encrypted, with no subject or sender in the database - the list reads them from the kept copy while the key is open - and a message deleted before the switch-on, kept in the clear, is encrypted on the way back when it is put back. Negative control: a plain sibling's deleted item keeps its subject in the database.")]
      public void DeletedItemsAreKeptAndPutBackAsTheMailboxKeepsMail()
      {
         int daysFound = ReadSetting("recover_deleted_days").GetInt32();
         putBack_.Add(() => Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":" + daysFound + "}"));
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":14}").status);

         MakeRecoveryKey();
         string keyed = AddAccount("atrest-deleted");
         string plain = AddAccount("atrest-deleted-plain");
         Account keyedAccount = AccountOverCom(keyed);
         Account plainAccount = AccountOverCom(plain);

         // Deleted before the switch-on: kept in the clear, as the mailbox was.
         string before = Word("tenrec");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "Before " + before, "Deleted before the switch: " + before);
         WaitForMessages(keyed, 1);
         ExpungeFirst(keyed);

         SwitchOn(keyed);

         // Deleted after it: kept encrypted.
         string after = Word("solenodon");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "After " + after, "Deleted after the switch: " + after);
         new SmtpClientSimulator().Send("sender@example.test", plain, "After " + after, "Deleted, plain: " + after);
         WaitForMessages(keyed, 1);
         WaitForMessages(plain, 1);
         ExpungeFirst(keyed);
         ExpungeFirst(plain);

         List<string> kept = KeptFiles(keyedAccount);
         ClassicAssert.AreEqual(2, kept.Count, "Two deleted items kept for the keyed mailbox.");
         string keptAfter = kept.Single(f => !Contains(File.ReadAllBytes(f), before));
         AssertEncrypted(keptAfter, after, "A keyed mailbox's deleted item");

         // The list reads the subject from the kept copy while the key is open.
         JsonElement items = Json(Me(keyed, Password, "GET", "/api/v1/me/recoverable", null), 200).GetProperty("items");
         ClassicAssert.AreEqual(2, items.GetArrayLength(), items.ToString());
         JsonElement afterItem = items.EnumerateArray().Single(i => i.GetProperty("subject").GetString() == "After " + after);
         JsonElement beforeItem = items.EnumerateArray().Single(i => i.GetProperty("subject").GetString() == "Before " + before);

         // Put back: the item kept in the clear before the switch comes back
         // encrypted.
         (int status, string body) restored = Me(keyed, Password, "POST", "/api/v1/me/recoverable/" + beforeItem.GetProperty("id").GetInt64() + "/restore", null);
         ClassicAssert.AreEqual(200, restored.status, restored.body);
         WaitForMessages(keyed, 1);
         AssertEncrypted(StoredFile(keyed, 0), before, "A deleted item put back into a keyed mailbox");
         StringAssert.Contains("Deleted before the switch: " + before, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));

         // The database holds no subject of the keyed mailbox's item: with its
         // kept copy gone, the list has nothing to show for it. The control: the
         // plain sibling's item shows its subject from the database alone.
         File.Delete(keptAfter);
         JsonElement keyedAgain = Json(Me(keyed, Password, "GET", "/api/v1/me/recoverable", null), 200).GetProperty("items");
         JsonElement afterAgain = keyedAgain.EnumerateArray().Single(i => i.GetProperty("id").GetInt64() == afterItem.GetProperty("id").GetInt64());
         ClassicAssert.AreEqual("", afterAgain.GetProperty("subject").GetString(), "The keyed mailbox's deleted item has its subject in the database in the clear.");
         ClassicAssert.AreEqual("", afterAgain.GetProperty("from").GetString(), "The keyed mailbox's deleted item has its sender in the database in the clear.");

         foreach (string file in KeptFiles(plainAccount))
            File.Delete(file);
         JsonElement plainItems = Json(Me(plain, Password, "GET", "/api/v1/me/recoverable", null), 200).GetProperty("items");
         ClassicAssert.AreEqual("After " + after, plainItems[0].GetProperty("subject").GetString(), "The control: the plain sibling's item did not keep its subject.");
      }

      // ---- passwords and the recovery ------------------------------------------------------

      [Test]
      [Description("With the key closed, an administrator's password reset is refused (409) naming the recovery; the recovery with a wrong passphrase is refused (403); with the domain's passphrase it opens the key and sets the new password, which opens the mail after a restart - and the old password no longer does. The switch-on and the recovery are on the audit trail.")]
      public void AnAdministratorsResetIsRefusedWhileTheKeyIsClosedAndTheRecoveryOpensIt()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-recover");
         SwitchOn(keyed);

         string marker = Word("dunnart");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "Recovered", "Read again after the recovery: " + marker);
         WaitForMessages(keyed, 1);
         StringAssert.Contains(marker, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));

         RestartServerAndReacquireCom();

         const string newPassword = "Recovered-Passw0rd!";
         (int status, string body) reset = Admin("POST", "/api/v1/accounts/" + keyed + "/password", "{\"password\":\"" + newPassword + "\"}");
         ClassicAssert.AreEqual(409, reset.status, "A password was set while the key was closed: " + reset.body);
         StringAssert.Contains("recover", reset.body);

         (int status, string body) wrong = Admin("POST", KeyPath(keyed) + "/recover",
            "{\"passphrase\":\"not the passphrase at all\",\"password\":\"" + newPassword + "\"}");
         ClassicAssert.AreEqual(403, wrong.status, wrong.body);

         (int status, string body) recovered = Admin("POST", KeyPath(keyed) + "/recover",
            "{\"passphrase\":\"" + Passphrase + "\",\"password\":\"" + newPassword + "\"}");
         ClassicAssert.AreEqual(200, recovered.status, recovered.body);

         // Closed again, so that what opens the mail is the new password's wrap.
         RestartServerAndReacquireCom();

         string fetched = FetchLiteral(keyed, newPassword, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsNotNull(fetched, "The new password did not open the recovered key.");
         StringAssert.Contains(marker, fetched);

         var old = new ImapClientSimulator();
         ClassicAssert.IsFalse(old.ConnectAndLogon(keyed, Password), "The password from before the recovery still signs in.");
         old.Disconnect();

         (int status, string body) switched = Admin("GET", "/api/v1/audit?action=mailbox-encryption-on&limit=20", null);
         ClassicAssert.AreEqual(200, switched.status, switched.body);
         StringAssert.Contains(keyed, switched.body, "The switch-on is not on the audit trail.");
         (int status, string body) recoveredRow = Admin("GET", "/api/v1/audit?action=mailbox-key-recovered&limit=20", null);
         ClassicAssert.AreEqual(200, recoveredRow.status, recoveredRow.body);
         StringAssert.Contains(keyed, recoveredRow.body, "The recovery is not on the audit trail.");
      }

      [Test]
      [Description("The person's own password change from the webmail carries the key: the first sign-in wrapped it (pending no longer); after a restart the key is closed, the new password opens it and the mail is read as it was, and the old password is refused.")]
      public void ThePersonsOwnPasswordChangeCarriesTheKey()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-change");
         SwitchOn(keyed);

         string marker = Word("potoroo");
         new SmtpClientSimulator().Send("sender@example.test", keyed, "Changed", "Read after the change: " + marker);
         WaitForMessages(keyed, 1);
         StringAssert.Contains(marker, FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]"));
         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("pending").GetBoolean(), "The first sign-in did not wrap the key by the password - the machine's copy would open it for any password.");

         const string newPassword = "Changed-Passw0rd!";
         (int status, string body) changed = Me(keyed, Password, "POST", "/api/v1/me/password", "{\"current\":\"" + Password + "\",\"new\":\"" + newPassword + "\"}");
         ClassicAssert.AreEqual(200, changed.status, changed.body);

         RestartServerAndReacquireCom();
         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("open").GetBoolean(), "A restart did not close the key.");

         string fetched = FetchLiteral(keyed, newPassword, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsNotNull(fetched, "The changed password did not open the key.");
         StringAssert.Contains(marker, fetched);
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("open").GetBoolean(), "The changed password's sign-in did not open the key.");

         var old = new ImapClientSimulator();
         ClassicAssert.IsFalse(old.ConnectAndLogon(keyed, Password), "The password from before the change still signs in.");
         old.Disconnect();
      }

      [Test]
      [Description("Replacing the domain's recovery key seals every account's key of the domain to the new one: with two keyed accounts, the replacement answers 200 with nothing left unfinished, and after a restart the new passphrase recovers either account while the old passphrase is refused (403).")]
      public void AReplacedRecoveryKeyOpensEveryAccountOfTheDomain()
      {
         MakeRecoveryKey();
         string first = AddAccount("atrest-replace-1");
         string second = AddAccount("atrest-replace-2");
         SwitchOn(first);
         SwitchOn(second);

         const string newPassphrase = "a second passphrase kept elsewhere";
         JsonElement replaced = Json(Admin("PUT", RecoveryPath(), "{\"passphrase\":\"" + newPassphrase + "\",\"hint\":\"the other safe\",\"current_passphrase\":\"" + Passphrase + "\"}"), 200);
         ClassicAssert.AreEqual(2, replaced.GetProperty("accounts").GetInt32(), replaced.ToString());
         ClassicAssert.IsFalse(replaced.GetProperty("replacement_unfinished").GetBoolean(), "The replacement left the old key beside the new one: " + replaced);
         ClassicAssert.IsTrue(KeyState(first).GetProperty("recovery_current").GetBoolean());
         ClassicAssert.IsTrue(KeyState(second).GetProperty("recovery_current").GetBoolean());

         RestartServerAndReacquireCom();

         (int status, string body) oldPassphrase = Admin("POST", KeyPath(first) + "/recover",
            "{\"passphrase\":\"" + Passphrase + "\",\"password\":\"Replaced-Passw0rd-1!\"}");
         ClassicAssert.AreEqual(403, oldPassphrase.status, "The replaced passphrase still opens an account: " + oldPassphrase.body);

         foreach (string address in new[] { first, second })
         {
            (int status, string body) recovered = Admin("POST", KeyPath(address) + "/recover",
               "{\"passphrase\":\"" + newPassphrase + "\",\"password\":\"Replaced-Passw0rd-2!\"}");
            ClassicAssert.AreEqual(200, recovered.status, address + ": " + recovered.body);
         }
      }

      [Test]
      [Description("Switching encryption on or off, forfeiting and recovering are taking an account over: a domain administrator is refused them for a server administrator's mailbox in its own domain (403) and for any account of another domain, while it switches an ordinary account of its domain on (201).")]
      public void ACredentialCannotSwitchAnAccountWithMoreAuthority()
      {
         MakeRecoveryKey();
         string domainAdmin = AddAccount("atrest-dadmin");
         string serverAdmin = AddAccount("atrest-sadmin");
         string ordinary = AddAccount("atrest-ordinary");

         Account dadmin = AccountOverCom(domainAdmin);
         dadmin.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         dadmin.Save();
         Account sadmin = AccountOverCom(serverAdmin);
         sadmin.AdminLevel = eAdminLevel.hAdminLevelServerAdmin;
         sadmin.Save();

         Domain otherDomain = SingletonProvider<TestSetup>.Instance.AddDomain("atrest-elsewhere.test");
         string far = "far@" + otherDomain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(otherDomain, far, Password);

         (int status, string body) onAdmin = Me(domainAdmin, Password, "PUT", KeyPath(serverAdmin), "{\"enabled\":true}");
         ClassicAssert.AreEqual(403, onAdmin.status, "A domain administrator switched a server administrator's mailbox: " + onAdmin.body);
         ClassicAssert.IsFalse(KeyState(serverAdmin).GetProperty("keyed").GetBoolean(), "The refused switch made a key.");

         (int status, string body) recoverFar = Me(domainAdmin, Password, "POST", KeyPath(far) + "/recover",
            "{\"passphrase\":\"" + Passphrase + "\",\"password\":\"Far-Passw0rd!\"}");
         ClassicAssert.AreEqual(403, recoverFar.status, "A domain administrator reached another domain's recovery: " + recoverFar.body);

         (int status, string body) onOrdinary = Me(domainAdmin, Password, "PUT", KeyPath(ordinary), "{\"enabled\":true}");
         ClassicAssert.AreEqual(201, onOrdinary.status, "The control: a domain administrator could not switch an ordinary account of its domain on: " + onOrdinary.body);
         keyed_.Add(ordinary);
      }

      // ---- moves, holds and the forfeit ------------------------------------------------

      [Test]
      [Description("A keyed mailbox is not moved to another domain - its key is sealed to its own domain's recovery key - and the move is refused 409 with the sentence; a rename inside its domain goes ahead. Negative control: a mailbox that is not encrypted moves to the other domain.")]
      public void AKeyedMailboxStaysInItsDomain()
      {
         MakeRecoveryKey();
         string keyed = AddAccount("atrest-move");
         string plain = AddAccount("atrest-move-plain");
         SwitchOn(keyed);

         Domain otherDomain = SingletonProvider<TestSetup>.Instance.AddDomain("atrest-moved.test");

         (int status, string body) moved = Admin("POST", "/api/v1/accounts/" + keyed + "/move", "{\"new_address\":\"atrest-move@" + otherDomain.Name + "\"}");
         ClassicAssert.AreEqual(409, moved.status, "A keyed mailbox was moved to another domain: " + moved.body);
         StringAssert.Contains("encrypted at rest", moved.body);

         string renamed = "atrest-renamed@" + _domain.Name;
         (int status, string body) rename = Admin("POST", "/api/v1/accounts/" + keyed + "/move", "{\"new_address\":\"" + renamed + "\"}");
         ClassicAssert.AreEqual(200, rename.status, "A rename inside the domain was refused: " + rename.body);
         keyed_.Remove(keyed);
         keyed_.Add(renamed);
         ClassicAssert.IsTrue(KeyState(renamed).GetProperty("recovery_current").GetBoolean(), "The renamed mailbox's key is not sealed to its domain's recovery key.");

         (int status, string body) plainMoved = Admin("POST", "/api/v1/accounts/" + plain + "/move", "{\"new_address\":\"atrest-move-plain@" + otherDomain.Name + "\"}");
         ClassicAssert.AreEqual(200, plainMoved.status, "The control: a mailbox that is not encrypted did not move: " + plainMoved.body);
      }

      [Test]
      [Description("Removing a key wants ?confirm=forfeit (400 without it, the key kept) and is refused while a legal hold covers the account (409, the key kept); released, the forfeit leaves the account unkeyed and the recovery key removable. The idle setting keeps its bounds (0 and 1441 refused, 30 kept).")]
      public void TheForfeitWantsTheWordAndNoHoldAndTheIdleSettingItsBounds()
      {
         (int status, string body) zero = Admin("PUT", "/api/v1/settings", "{\"mailbox_key_idle_minutes\":0}");
         ClassicAssert.AreEqual(400, zero.status, zero.body);
         (int status, string body) many = Admin("PUT", "/api/v1/settings", "{\"mailbox_key_idle_minutes\":1441}");
         ClassicAssert.AreEqual(400, many.status, many.body);
         SetSetting("mailbox_key_idle_minutes", "30");
         ClassicAssert.AreEqual(30, ReadSetting("mailbox_key_idle_minutes").GetInt32());

         MakeRecoveryKey();
         string keyed = AddAccount("atrest-forfeit");
         SwitchOn(keyed);

         (int status, string body) unconfirmed = Admin("DELETE", KeyPath(keyed), null);
         ClassicAssert.AreEqual(400, unconfirmed.status, unconfirmed.body);
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("keyed").GetBoolean(), "A key was removed without the word.");

         // A legal hold on the account keeps its key, the only way to its mail.
         (int status, string body) opened = Admin("POST", "/api/v1/discovery/cases", "{\"name\":\"At rest " + Word("case") + "\",\"description\":\"Opened by the encryption at rest fixture.\"}");
         ClassicAssert.AreEqual(201, opened.status, opened.body);
         long caseId = JsonDocument.Parse(opened.body).RootElement.GetProperty("id").GetInt64();
         putBack_.Add(() =>
         {
            Admin("POST", "/api/v1/discovery/cases/" + caseId + "/close", "{}");
            _application.Database.ExecuteSQL("delete from hm_discoveryholds where holdcaseid = " + caseId);
            _application.Database.ExecuteSQL("delete from hm_discoverycases where caseid = " + caseId);
         });

         (int status, string body) placed = Admin("POST", "/api/v1/discovery/cases/" + caseId + "/holds", "{\"scope\":\"account\",\"target\":\"" + keyed + "\"}");
         ClassicAssert.AreEqual(201, placed.status, placed.body);
         long holdId = JsonDocument.Parse(placed.body).RootElement.GetProperty("holds")[0].GetProperty("id").GetInt64();

         (int status, string body) held = Admin("DELETE", KeyPath(keyed) + "?confirm=forfeit", null);
         ClassicAssert.AreEqual(409, held.status, "A held account's key was forfeited: " + held.body);
         StringAssert.Contains("legal hold", held.body);
         ClassicAssert.IsTrue(KeyState(keyed).GetProperty("keyed").GetBoolean(), "The refused forfeit removed the key.");

         ClassicAssert.AreEqual(200, Admin("DELETE", "/api/v1/discovery/cases/" + caseId + "/holds/" + holdId, null).status, "The hold could not be released.");

         (int status, string body) forfeited = Admin("DELETE", KeyPath(keyed) + "?confirm=forfeit", null);
         ClassicAssert.AreEqual(200, forfeited.status, forfeited.body);
         ClassicAssert.IsFalse(KeyState(keyed).GetProperty("keyed").GetBoolean(), "The key is still there after the forfeit.");

         // Nothing is sealed to the recovery key now: it can go.
         (int status, string body) removed = Admin("DELETE", RecoveryPath(), null);
         ClassicAssert.AreEqual(200, removed.status, removed.body);
         recoveryDomains_.Remove(_domain.Name);
      }

      // ---- the command line --------------------------------------------------------------

      [Test]
      [Description("--encrypt-store encrypts the mail stored before the switch, which is then read back exactly; --decrypt-store without the passphrase in the environment is refused (exit 2) and changes nothing, and with it puts every file back byte for byte.")]
      public void EncryptStoreAndDecryptStorePutEveryMessageBackByteForByte()
      {
         if (TestSetup.ConsoleServer)
            Assert.Ignore("The server under test is a console server, which the walk's service check cannot see stopped.");

         MakeRecoveryKey();
         string keyed = AddAccount("atrest-walk");

         var smtp = new SmtpClientSimulator();
         smtp.Send("sender@example.test", keyed, "Before the switch 1", "Stored before the switch, the first.");
         smtp.Send("sender@example.test", keyed, "Before the switch 2", "Stored before the switch, the second.");
         WaitForMessages(keyed, 2);

         var paths = new List<string> { StoredFile(keyed, 0), StoredFile(keyed, 1) };
         var originals = paths.Select(File.ReadAllBytes).ToList();
         ClassicAssert.IsTrue(originals.All(b => !HasMagic(b)), "Mail stored before the switch is not plain.");

         SwitchOn(keyed);

         ServerExecutable();
         StopService();
         try
         {
            Verb encrypt = RunVerb("--encrypt-store --account " + keyed, null);
            ClassicAssert.AreEqual(0, encrypt.ExitCode, encrypt.Output);
            StringAssert.Contains("Encrypted 2 stored files in " + keyed, encrypt.Output);
            for (int i = 0; i < 2; i++)
               AssertEncrypted(paths[i], "Stored before the switch", "--encrypt-store's rewrite of " + paths[i]);
         }
         finally
         {
            RestartServerAndReacquireCom();
         }

         string first = FetchLiteral(keyed, Password, "INBOX", 1, "BODY.PEEK[]");
         string second = FetchLiteral(keyed, Password, "INBOX", 2, "BODY.PEEK[]");
         var fetched = new List<string> { first, second };
         var expected = originals.Select(b => Encoding.ASCII.GetString(b)).ToList();
         CollectionAssert.AreEquivalent(expected, fetched, "The encrypted files do not read back as the messages they were.");

         StopService();
         try
         {
            var encryptedBytes = paths.Select(File.ReadAllBytes).ToList();

            Verb refused = RunVerb("--decrypt-store --account " + keyed, null);
            ClassicAssert.AreEqual(2, refused.ExitCode, refused.Output);
            StringAssert.Contains("HMAILSERVER_RECOVERY_PASSPHRASE", refused.Output);
            for (int i = 0; i < 2; i++)
               CollectionAssert.AreEqual(encryptedBytes[i], File.ReadAllBytes(paths[i]), "A refused --decrypt-store changed " + paths[i]);

            Verb decrypt = RunVerb("--decrypt-store --account " + keyed, new Dictionary<string, string> { { "HMAILSERVER_RECOVERY_PASSPHRASE", Passphrase } });
            ClassicAssert.AreEqual(0, decrypt.ExitCode, decrypt.Output);
            StringAssert.Contains("Decrypted 2 stored files in " + keyed, decrypt.Output);
            for (int i = 0; i < 2; i++)
               CollectionAssert.AreEqual(originals[i], File.ReadAllBytes(paths[i]), "--decrypt-store did not put " + paths[i] + " back byte for byte.");
         }
         finally
         {
            RestartServerAndReacquireCom();
         }

         // Off, so that nothing is encrypted again once the service is back.
         ClassicAssert.AreEqual(200, Admin("PUT", KeyPath(keyed), "{\"enabled\":false}").status);
      }

      // ---- the container, from the doc ---------------------------------------------------

      private sealed class ContainerHeader
      {
         public int HeaderLength;
         public long OriginalSize;
         public byte[] Sha256;
         public readonly List<(int id, byte[] parameters)> Stages = new List<(int, byte[])>();
         public int References;
      }

      private static bool HasMagic(byte[] file)
         => file.Length >= Magic.Length && file.Take(Magic.Length).SequenceEqual(Magic);

      private static ContainerHeader Parse(byte[] file)
      {
         var header = new ContainerHeader
         {
            HeaderLength = BitConverter.ToInt32(file, 12),
            OriginalSize = BitConverter.ToInt64(file, 16),
            Sha256 = file.Skip(24).Take(32).ToArray()
         };

         int at = 56;
         int count = file[at++];
         for (int i = 0; i < count; i++)
         {
            int id = file[at];
            int length = file[at + 1];
            at += 2;
            header.Stages.Add((id, file.Skip(at).Take(length).ToArray()));
            at += length;
         }

         header.References = BitConverter.ToInt32(file, at);
         return header;
      }

      // A stored file that is an encryption container (its last stage 3), with
      // no reference and no word of the message in the clear.
      private static void AssertEncrypted(string path, string word, string what)
      {
         byte[] file = File.ReadAllBytes(path);
         ClassicAssert.IsTrue(HasMagic(file), what + " is not a container: " + path);
         ContainerHeader header = Parse(file);
         ClassicAssert.IsTrue(header.Stages.Count > 0 && header.Stages[header.Stages.Count - 1].id == 3,
            what + " is a container, but not encrypted: stages " + string.Join(",", header.Stages.Select(s => s.id)));
         ClassicAssert.AreEqual(0, header.References, what + " names a part held once.");
         ClassicAssert.IsFalse(Contains(file, word), what + " has a word of the message in the clear.");
      }

      private static bool Contains(byte[] haystack, string needle)
      {
         byte[] bytes = Encoding.ASCII.GetBytes(needle);
         for (int i = 0; i + bytes.Length <= haystack.Length; i++)
         {
            int j = 0;
            while (j < bytes.Length && haystack[i + j] == bytes[j])
               j++;
            if (j == bytes.Length)
               return true;
         }
         return false;
      }

      private static string LargeBody(int approximateBytes, string word)
      {
         var body = new StringBuilder();
         body.Append("The word of this message is ").Append(word).Append(".\r\n");
         while (body.Length < approximateBytes)
            body.Append("0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvw\r\n");
         return body.ToString();
      }

      // A message with a base64 attachment of kilobytes, different for each
      // mailbox's copy only in its To.
      private static string WithAttachment(string to, string subject, string text, int kilobytes)
      {
         var content = new byte[kilobytes * 1024];
         new Random(112).NextBytes(content);
         return "From: sender@example.test\r\nTo: " + to + "\r\nSubject: " + subject + "\r\nMIME-Version: 1.0\r\n" +
                "Content-Type: multipart/mixed; boundary=\"atrest-boundary\"\r\n\r\n" +
                "--atrest-boundary\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\n" + text + "\r\n" +
                "--atrest-boundary\r\nContent-Type: application/octet-stream; name=\"blob.bin\"\r\nContent-Transfer-Encoding: base64\r\n\r\n" +
                Convert.ToBase64String(content, Base64FormattingOptions.InsertLineBreaks) + "\r\n" +
                "--atrest-boundary--\r\n";
      }

      private static string Word(string stem) => stem + Guid.NewGuid().ToString("N").Substring(0, 10);

      // The server names its temporary files and directories by a braced GUID
      // (FileUtilities::GetTempFileName, the attachment directories it used to
      // make): what CreatedWhile keeps, so that a file somebody else puts in the
      // directory meanwhile does not fail a test.
      private static readonly Regex ServerTemporaryName = new Regex(@"\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}");

      // The server's files and directories created under a directory while an
      // action runs, as a FileSystemWatcher sees them - which catches a copy
      // written and deleted again before anybody could list it.
      private static List<string> CreatedWhile(string directory, Action action)
      {
         var created = new List<string>();
         Directory.CreateDirectory(directory);

         using (var watcher = new FileSystemWatcher(directory))
         {
            watcher.IncludeSubdirectories = true;
            watcher.NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName;
            watcher.InternalBufferSize = 64 * 1024;
            watcher.Created += (sender, e) => { lock (created) created.Add(e.FullPath); };
            watcher.Renamed += (sender, e) => { lock (created) created.Add(e.FullPath); };
            watcher.Error += (sender, e) => { lock (created) created.Add("(the watcher lost events: " + e.GetException().Message + ")"); };
            watcher.EnableRaisingEvents = true;

            action();

            // The events arrive a moment after what caused them.
            Thread.Sleep(1500);
            watcher.EnableRaisingEvents = false;
         }

         lock (created)
            return created.Where(path => ServerTemporaryName.IsMatch(path) || path.StartsWith("(the watcher", StringComparison.Ordinal)).ToList();
      }

      private static void WriteLine(NetworkStream stream, string line)
      {
         byte[] bytes = Encoding.ASCII.GetBytes(line + "\r\n");
         stream.Write(bytes, 0, bytes.Length);
      }

      // What arrives until the text has, within the stream's receive timeout.
      private static string ReadUntilText(NetworkStream stream, string text)
      {
         var seen = new StringBuilder();
         var buffer = new byte[4096];
         while (!seen.ToString().Contains(text))
         {
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read == 0)
               break;
            seen.Append(Encoding.ASCII.GetString(buffer, 0, read));
         }
         return seen.ToString();
      }

      // Whether the server closes a session that sends nothing - within the
      // stream's receive timeout. A BYE or the connection closing is closed; an
      // untagged update is read past; the timeout is not closed.
      private static bool ClosedWithoutACommand(NetworkStream stream)
      {
         var seen = new StringBuilder();
         var buffer = new byte[4096];
         try
         {
            while (true)
            {
               int read = stream.Read(buffer, 0, buffer.Length);
               if (read == 0)
                  return true;

               seen.Append(Encoding.ASCII.GetString(buffer, 0, read));
               if (seen.ToString().Contains("BYE"))
                  return true;
            }
         }
         catch (IOException e)
         {
            var socket = e.InnerException as SocketException;
            return socket == null || socket.SocketErrorCode != SocketError.TimedOut;
         }
      }

      // Whether the server has ended a session: a command sent on it is answered
      // with nothing, with a BYE or an -ERR, or with an error at the socket -
      // within a few seconds, since a drop is queued on the connection's own
      // strand. False when it is still answered as usual.
      private static bool Ended(Func<string> command)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(15);
         while (DateTime.UtcNow < until)
         {
            string answer;
            try
            {
               answer = command();
            }
            catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
            {
               return true;
            }

            if (string.IsNullOrEmpty(answer) || answer.Contains("BYE") || answer.StartsWith("-ERR", StringComparison.Ordinal))
               return true;

            Thread.Sleep(500);
         }

         return false;
      }

      private static void SleepUntil(DateTime until)
      {
         TimeSpan left = until - DateTime.UtcNow;
         if (left > TimeSpan.Zero)
            Thread.Sleep(left);
      }

      // The database fault injector (DatabaseFailureHandling.cs): every statement
      // containing the fragment fails, from the reinitialise on; empty clears it.
      private void FailDatabaseStatementsContaining(string fragment)
      {
         IniFileSetting.Write("SimulateDatabaseFailureFor", fragment);
         _application.Reinitialize();
      }

      // The files written under a directory since a moment, recursively.
      private static IEnumerable<string> FilesWritten(string directory, DateTime since)
      {
         if (!Directory.Exists(directory))
            return Enumerable.Empty<string>();

         return Directory.GetFiles(directory, "*", SearchOption.AllDirectories)
            .Where(f =>
            {
               try { return File.GetLastWriteTimeUtc(f) >= since; }
               catch (IOException) { return false; }
               catch (UnauthorizedAccessException) { return false; }
            })
            .ToList();
      }

      // ---- the server --------------------------------------------------------------------

      private string AddAccount(string name)
      {
         string address = name + "@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         return address;
      }

      private Account AccountOverCom(string address)
      {
         Domain domain = _application.Domains.get_ItemByName(address.Substring(address.IndexOf('@') + 1));
         return domain.Accounts.get_ItemByAddress(address);
      }

      private void DeleteAccount(string address)
      {
         AccountOverCom(address).Delete();

         string blobs = Paths.Combine(_application.Settings.Directories.DataDirectory, "Blobs");
         if (Directory.Exists(blobs) && Directory.GetFiles(blobs, "*", SearchOption.AllDirectories).Length == 0)
            Directory.Delete(blobs, true);
      }

      private string KeyPath(string address) => "/api/v1/accounts/" + address + "/mailbox-encryption";

      private string RecoveryPath() => "/api/v1/domains/" + _domain.Name + "/mailbox-recovery";

      private void MakeRecoveryKey()
      {
         (int status, string body) made = Admin("PUT", RecoveryPath(), "{\"passphrase\":\"" + Passphrase + "\",\"hint\":\"the safe in the office\"}");
         ClassicAssert.AreEqual(201, made.status, made.body);
         recoveryDomains_.Add(_domain.Name);
      }

      private JsonElement SwitchOn(string address)
      {
         JsonElement on = Json(Admin("PUT", KeyPath(address), "{\"enabled\":true}"), 201);
         keyed_.Add(address);
         return on;
      }

      private JsonElement KeyState(string address) => Json(Admin("GET", KeyPath(address), null), 200);

      private string MakeAppPassword(string address)
      {
         JsonElement created = Json(Admin("POST", "/api/v1/accounts/" + address + "/app-passwords", "{\"name\":\"Phone\"}"), 201);
         return created.GetProperty("password").GetString();
      }

      private JsonElement ReadSetting(string name) => Json(Admin("GET", "/api/v1/settings", null), 200).GetProperty(name);

      // A setting of /api/v1/settings, read first and put back in the teardown.
      private void SetSetting(string name, string jsonValue)
      {
         string found = ReadSetting(name).GetRawText();
         putBack_.Add(() => Admin("PUT", "/api/v1/settings", "{\"" + name + "\":" + found + "}"));
         (int status, string body) put = Admin("PUT", "/api/v1/settings", "{\"" + name + "\":" + jsonValue + "}");
         ClassicAssert.AreEqual(200, put.status, put.body);
      }

      // A [Settings] key through the settings store, read first and put back as
      // it was - removed when it was not there.
      private void SetIni(string key, string value)
      {
         string found = IniFileSetting.Read(key);
         putBack_.Add(() =>
         {
            if (string.IsNullOrEmpty(found))
               IniFileSetting.Delete(key);
            else
               IniFileSetting.Write(key, found);
            _application.Reinitialize();
         });
         IniFileSetting.Write(key, value);
      }

      // Zero access switched on as the webmail switches it on, less the
      // derivations: the fixture key stored as the mailbox's own, then the switch
      // (Discovery.cs and SMTP/ZeroAccess.cs do the same).
      private void SwitchZeroAccessOn(string address)
      {
         string ownKey = "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\"," +
                         "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"," +
                         "\"address\":\"" + address + "\",\"name\":\"Sealed\",\"source\":\"generated\",\"revoked\":false}";
         (int status, string body) stored = Me(address, Password, "PUT", "/api/v1/me/openpgp/keys/own", ownKey);
         ClassicAssert.AreEqual(201, stored.status, stored.body);

         string switchBody = "{\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\",\"salt\":\"" + ZeroAccessSalt + "\",\"passes\":3,\"memory_kib\":65536,\"lanes\":4," +
                             "\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"recovery_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"}";
         (int status, string body) on = Me(address, Password, "PUT", "/api/v1/me/zero-access", switchBody);
         ClassicAssert.AreEqual(201, on.status, on.body);
      }

      private static string Escaped(string armoured)
         => armoured.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");

      // Asked of the server each time: a COM object from before a restart belongs
      // to the process that was.
      private IMAPFolder Inbox(string address) => AccountOverCom(address).IMAPFolders.get_ItemByName("INBOX");

      private IMAPFolder PublicFolder(string name) => _application.Settings.PublicFolders.get_ItemByName(name);

      // Counted over COM, which reads the rows and not the files: it opens no key.
      private void WaitForMessages(string address, int count)
      {
         WaitFor(() => Inbox(address).Messages.Count == count, "The INBOX of " + address + " did not reach " + count + " message(s).", 30);
      }

      private string StoredFile(string address, int index) => Inbox(address).Messages[index].Filename;

      private List<string> KeptFiles(Account account)
      {
         string directory = Paths.Combine(Paths.Combine(_application.Settings.Directories.DataDirectory, "Recoverable"), account.ID.ToString());
         return Directory.Exists(directory) ? Directory.GetFiles(directory).ToList() : new List<string>();
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

      private static string SearchRaw(string address, string command)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         string response = imap.SendSingleCommand(command);
         imap.Disconnect();
         return response;
      }

      /// <summary>
      ///    One FETCH's literal, read to the tagged reply after it (not to the first
      ///    line that begins with the tag, which a line of a message may do).
      ///    Null when the LOGIN is refused or no literal comes.
      /// </summary>
      private static string FetchLiteral(string address, string password, string folder, int index, string item)
      {
         var imap = new ImapClientSimulator();
         if (!imap.ConnectAndLogon(address, password))
         {
            imap.Disconnect();
            return null;
         }

         ClassicAssert.IsTrue(imap.SelectFolder(folder), "SELECT " + folder + " failed for " + address);

         var all = new StringBuilder();
         string literal = null;
         DateTime until = DateTime.UtcNow.AddSeconds(60);

         try
         {
            all.Append(imap.Send("F1 FETCH " + index + " " + item));

            while (DateTime.UtcNow < until)
            {
               string text = all.ToString();
               int brace = text.IndexOf('{');
               int close = brace >= 0 ? text.IndexOf("}\r\n", brace, StringComparison.Ordinal) : -1;

               if (close > brace)
               {
                  int length = int.Parse(text.Substring(brace + 1, close - brace - 1));
                  int start = close + 3;
                  if (text.Length >= start + length)
                  {
                     int tagged = text.IndexOf("\r\nF1 ", start + length, StringComparison.Ordinal);
                     if (tagged >= 0 && text.IndexOf("\r\n", tagged + 2, StringComparison.Ordinal) >= 0)
                     {
                        literal = text.Substring(start, length);
                        break;
                     }
                  }
               }
               else if (text.StartsWith("F1 ", StringComparison.Ordinal) || text.Contains("\r\nF1 "))
                  break;   // answered with no literal: refused

               string more = imap.Receive();
               if (more.Length == 0)
                  break;
               all.Append(more);
            }
         }
         catch (IOException)
         {
            // The server stopped answering: no literal.
         }

         try
         {
            imap.Disconnect();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
         }

         return literal;
      }

      // AUTHENTICATE PLAIN with an authorization identity: RFC 4616, authzid NUL
      // authcid NUL password. The tagged reply.
      private static string AuthenticateAs(string authzid, string authcid, string password)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(TestPorts.Imap), "Could not connect to the IMAP server.");
            socket.ReadUntil("* OK");
            socket.Send("A01 AUTHENTICATE PLAIN\r\n");
            string continuation = socket.Receive();
            StringAssert.StartsWith("+", continuation, "The server did not ask for the SASL response: " + continuation);
            socket.Send(Convert.ToBase64String(Encoding.UTF8.GetBytes(authzid + "\0" + authcid + "\0" + password)) + "\r\n");
            string result = socket.ReadUntil("A01 ");
            socket.Disconnect();
            return result;
         }
      }

      private static string MintToken(string email)
      {
         long exp = (long) (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds + 3600;
         string header = Base64Url(Encoding.UTF8.GetBytes("{\"alg\":\"HS256\",\"typ\":\"JWT\"}"));
         string payload = Base64Url(Encoding.UTF8.GetBytes("{\"email\":\"" + email + "\",\"exp\":" + exp + "}"));
         using (var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(OAuthSecret)))
            return header + "." + payload + "." + Base64Url(hmac.ComputeHash(Encoding.ASCII.GetBytes(header + "." + payload)));
      }

      private static string Base64Url(byte[] data)
         => Convert.ToBase64String(data).TrimEnd('=').Replace('+', '-').Replace('/', '_');

      // AUTHENTICATE XOAUTH2 in its continuation form; the tagged reply.
      private static string ImapXOAuth2(string email, string token)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(TestPorts.Imap), "Could not connect to the IMAP server.");
            socket.Receive();
            socket.Send("A01 AUTHENTICATE XOAUTH2\r\n");
            string challenge = socket.Receive();
            if (!challenge.TrimStart().StartsWith("+"))
               return challenge;

            string sasl = "user=" + email + "\u0001auth=Bearer " + token + "\u0001\u0001";
            socket.Send(Convert.ToBase64String(Encoding.ASCII.GetBytes(sasl)) + "\r\n");
            return socket.ReadUntil("A01 ");
         }
      }

      // A webmail session started with a password: the Cookie header line that
      // carries it.
      private (int status, string headers, string body) StartSessionRaw(string address, string password)
         => HttpRaw("POST", "/api/v1/session", Basic(address, password), null, null);

      private string StartSession(string address, string password)
      {
         (int status, string headers, string body) created = StartSessionRaw(address, password);
         ClassicAssert.AreEqual(201, created.status, created.body);

         const string needle = "Set-Cookie: hmailsession=";
         int at = created.headers.IndexOf(needle, StringComparison.Ordinal);
         ClassicAssert.GreaterOrEqual(at, 0, "No session cookie: " + created.headers);
         int start = at + needle.Length;
         int end = created.headers.IndexOfAny(new[] { ';', '\r' }, start);
         string cookie = end < 0 ? created.headers.Substring(start) : created.headers.Substring(start, end - start);
         return "Cookie: hmailsession=" + cookie + "\r\n";
      }

      private sealed class Verb
      {
         public int ExitCode;
         public string Output;
      }

      /// <summary>hMailServer.exe with these arguments, no HM_ variable of the runner's, and the given environment beside.</summary>
      private Verb RunVerb(string arguments, Dictionary<string, string> environment)
      {
         var start = new ProcessStartInfo(ServerExecutable(), arguments)
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };

         foreach (string name in start.EnvironmentVariables.Keys.Cast<string>().ToList())
         {
            if (name.StartsWith("HM_", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "HMAILSERVER_RECOVERY_PASSPHRASE", StringComparison.OrdinalIgnoreCase))
               start.EnvironmentVariables.Remove(name);
         }

         if (environment != null)
         {
            foreach (KeyValuePair<string, string> variable in environment)
               start.EnvironmentVariables[variable.Key] = variable.Value;
         }

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(180 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
                  // It finished between the wait and the kill.
               }

               Assert.Fail(arguments + " did not finish within three minutes, and was stopped.");
            }
            process.WaitForExit();

            return new Verb { ExitCode = process.ExitCode, Output = output.Result + error.Result };
         }
      }

      private string executable_;

      // Asked of the server once, over COM, and kept: a verb run with the service
      // stopped has no server to ask.
      private string ServerExecutable()
      {
         if (executable_ != null)
            return executable_;

         string program = _application.Settings.Directories.ProgramDirectory;
         foreach (string candidate in new[] { Path.Combine(program, "hMailServer.exe"), Path.Combine(program, "Bin", "hMailServer.exe") })
         {
            if (File.Exists(candidate))
               return executable_ = candidate;
         }

         Assert.Ignore("hMailServer.exe is not in the program directory the server reports (" + program + "), so the verb cannot be run here.");
         return null;
      }

      /// <summary>The service stopped, and its process gone.</summary>
      private static void StopService()
      {
         using (Process process = Process.Start(new ProcessStartInfo("net", "stop hMailServer")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
         }))
         {
            process.StandardOutput.ReadToEnd();
            process.StandardError.ReadToEnd();
            process.WaitForExit(90000);
         }

         DateTime deadline = DateTime.UtcNow.AddSeconds(60);
         while (DateTime.UtcNow < deadline && Process.GetProcessesByName("hmailserver").Length > 0)
            Thread.Sleep(250);

         ClassicAssert.IsFalse(ServiceRunning(), "The hMailServer service did not stop.");
      }

      private static bool ServiceRunning()
      {
         using (var controller = new System.ServiceProcess.ServiceController("hMailServer"))
         {
            controller.Refresh();
            return controller.Status != System.ServiceProcess.ServiceControllerStatus.Stopped;
         }
      }

      private static void WaitFor(Func<bool> condition, string failure, int seconds)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(seconds);
         while (DateTime.UtcNow < until)
         {
            if (condition())
               return;
            Thread.Sleep(500);
         }

         Assert.Fail(failure);
      }

      private static JsonElement Json((int status, string body) answer, int status)
      {
         ClassicAssert.AreEqual(status, answer.status, answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      private static void Guarded(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("EncryptionAtRest teardown: " + e.Message);
         }
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private (int status, string body) Me(string address, string password, string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic(address, password), requestBody);

      /// <summary>
      ///    One exchange with the response's header block and room for request
      ///    headers of the caller's own - a Cookie going out, a Set-Cookie coming
      ///    back - as RestApiCoverage's. extraHeaders is complete header lines,
      ///    each ending in a carriage return and a line feed.
      /// </summary>
      private static (int status, string headers, string body) HttpRaw(string method, string path, string authorization,
                                                                       string requestBody, string extraHeaders)
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
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               if (extraHeaders != null)
                  headers.Append(extraHeaders);

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int statusCode = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string responseHeaders = separator >= 0 ? raw.Substring(0, separator + 2) : raw;
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";
               return (statusCode, responseHeaders, body);
            }
         }
      }
   }
}
