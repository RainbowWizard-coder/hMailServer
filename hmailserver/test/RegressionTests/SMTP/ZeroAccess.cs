// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Zero-access mailboxes (Common/Util/ZeroAccess.h, docs/ZeroAccess.md):
   ///    what this server does once an account has switched itself to one.
   ///
   ///    The switch is thrown here the way the page throws it - the account's
   ///    own key in the ring, then PUT /api/v1/me/zero-access with the two
   ///    sealed copies - except that the copies are the fixture's own
   ///    passphrase-sealed secret key rather than copies sealed under an
   ///    Argon2id passphrase and a recovery key. The server cannot tell the
   ///    difference and is not meant to: what it checks is that each copy is an
   ///    encrypted secret key of the fingerprint named, which these are. The
   ///    derivations are the page's, held to RFC 9106 in
   ///    build/check-portal-openpgp.js.
   ///
   ///    What is under test is the delivery: that a message to such a mailbox
   ///    is stored as a PGP/MIME message whose outer headers are the original's
   ///    and whose body is nowhere in the file; that IMAP hands out that
   ///    ciphertext and SEARCH finds the subject and not the body; that the
   ///    administrator's read and export of the mailbox are refused by name; that
   ///    a journal rule's copy, made at acceptance, is the plaintext; that the
   ///    opt-in is refused while the server does not allow it; that a password
   ///    change carries the key sealed again or is refused; and that with the
   ///    switch off nothing changes; and, after the adversarial review of 22
   ///    September 2026: that a key which expires is refused as the mailbox
   ///    key, that a seal which cannot be made refuses the delivery and tells
   ///    the sender, that a message which arrived encrypted is stored as it
   ///    arrived, that an RSA key is written to as well, that the Sent copy of a
   ///    message sent from the webmail is sealed, and that the mailbox key can
   ///    be neither removed nor revoked while the switch is on. Each with its
   ///    negative control - the same message to an ordinary mailbox - so that a
   ///    delivery path that stopped sealing would fail here and not merely
   ///    deliver.
   ///
   ///    Where a gpg is found (Git for Windows carries one), the sealed file is
   ///    handed to it with the fixture's secret key and its plaintext compared
   ///    to the body sent: GnuPG's word that the message the server wrote is
   ///    the message it was given. The Linux CI step and the module's own check
   ///    make the same proof without a bench; here it runs when it can and says
   ///    so when it cannot.
   /// </summary>
   [TestFixture]
   public class ZeroAccess : TestFixtureBase
   {
      private static int RestPort = 9786;
      private const string AdminPassword = "testar";
      private const string Password = "ZeroAccess-Passw0rd!";
      private const string Salt = "AAECAwQFBgcICQoLDA0ODw==";

      private Account _sealed;
      private Account _plain;
      private string _me;
      private string _other;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         _sealed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sealed@" + _domain.Name, Password);
         _plain = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "plain@" + _domain.Name, Password);
         _me = Basic(_sealed.Address, Password);
         _other = Basic(_plain.Address, Password);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":true}").status, "The server-wide switch could not be turned on.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // Off, which is what every other fixture finds it, and what a
            // fresh installation has.
            Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":false}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A message delivered to a zero-access mailbox is stored as a PGP/MIME message: the outer headers the original's, the body nowhere in the file, the server's mark on it; the same message to an ordinary mailbox is stored in the clear.")]
      public void ADeliveryToAZeroAccessMailboxIsSealedOnDisk()
      {
         SwitchOn();

         const string subject = "The subject stays in the clear";
         const string body = "The body goes under the key: nothing in this sentence may be on the disk.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, subject, body);
         SmtpClientSimulator.StaticSend("sender@example.org", _plain.Address, subject, body);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_plain.Address, Password, 1);

         string sealedFile = FileOfFirstMessage(_sealed);
         string stored = File.ReadAllText(sealedFile);
         StringAssert.Contains("Subject: " + subject + "\r\n", stored, "The subject is a kept header.");
         StringAssert.Contains("From: sender@example.org", stored);
         StringAssert.Contains("Return-Path: <sender@example.org>", stored, "The trace headers written at delivery are kept.");
         StringAssert.Contains("X-hMailServer-Zero-Access: sealed on delivery\r\n", stored);
         StringAssert.Contains("Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";", stored);
         StringAssert.Contains("Content-Type: application/pgp-encrypted", stored);
         StringAssert.Contains("Version: 1\r\n", stored);
         StringAssert.Contains("-----BEGIN PGP MESSAGE-----", stored);
         StringAssert.Contains("-----END PGP MESSAGE-----", stored);
         StringAssert.DoesNotContain(body, stored, "The plaintext body is in the sealed file.");
         StringAssert.DoesNotContain("nothing in this sentence", stored);
         Assert.AreEqual(1, Regex.Matches(stored, "^MIME-Version:", RegexOptions.Multiline).Count, "MIME-Version appears exactly once.");
         StringAssert.DoesNotContain("Content-Transfer-Encoding: 7bit\r\n\r\n" + body, stored);

         // The negative control: the ordinary mailbox's file is the message as
         // it arrived, body and all, and carries no mark.
         string plainStored = File.ReadAllText(FileOfFirstMessage(_plain));
         StringAssert.Contains(body, plainStored);
         StringAssert.DoesNotContain("X-hMailServer-Zero-Access", plainStored);
         StringAssert.DoesNotContain("multipart/encrypted", plainStored);

         // The sealed message is a valid OpenPGP message to the fixture key:
         // GnuPG's word, where a gpg is to be had.
         string gpg = FindGpg();
         if (gpg == null)
         {
            TestContext.WriteLine("No gpg found on this machine: the Linux CI step and build/check-portal-openpgp.js are the GnuPG proof of the writer.");
            return;
         }
         string opened = DecryptWithGpg(gpg, ArmourOf(stored), OpenPgpTestSupport.RingOwnSecretArmoured, out string why);
         if (opened == null)
         {
            TestContext.WriteLine("gpg is here but could not use a secret key (" + why + "): the Linux CI step and build/check-portal-openpgp.js are the GnuPG proof of the writer.");
            return;
         }
         StringAssert.Contains(body, opened, "GnuPG's plaintext carries the body that was sent.");
         StringAssert.Contains("Subject: " + subject, opened, "The whole original message, headers and all, is inside.");
      }

      [Test]
      [Description("IMAP hands out the ciphertext, which any client holding the key opens, and SEARCH finds the subject and not the body; the ordinary mailbox's body is searchable.")]
      public void ImapHandsOutTheCiphertextAndSearchFindsTheHeadersOnly()
      {
         SwitchOn();

         const string subject = "Quarterly figures";
         const string body = "The word xylophone is in the body and nowhere else.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, subject, body);
         SmtpClientSimulator.StaticSend("sender@example.org", _plain.Address, subject, body);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_plain.Address, Password, 1);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_sealed.Address, Password));
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         string fetched = imap.Fetch("1 BODY.PEEK[]");
         StringAssert.Contains("-----BEGIN PGP MESSAGE-----", fetched, "FETCH hands out the ciphertext.");
         StringAssert.Contains("multipart/encrypted", fetched);
         StringAssert.DoesNotContain("xylophone", fetched, "The body is not in what IMAP hands out.");
         Assert.AreEqual("1", imap.Search("SUBJECT Quarterly").Trim(), "The subject, a kept header, is found.");
         Assert.AreEqual("", imap.Search("BODY xylophone").Trim(), "The body is not found: the server does not have it.");
         Assert.AreEqual("", imap.Search("TEXT xylophone").Trim());
         imap.Disconnect();

         // The negative control: the same searches on the ordinary mailbox.
         var plain = new ImapClientSimulator();
         Assert.IsTrue(plain.ConnectAndLogon(_plain.Address, Password));
         Assert.IsTrue(plain.SelectFolder("INBOX"));
         Assert.AreEqual("1", plain.Search("SUBJECT Quarterly").Trim());
         Assert.AreEqual("1", plain.Search("BODY xylophone").Trim(), "An ordinary mailbox's body is searchable.");
         plain.Disconnect();

         // POP3 too hands out the file as it is.
         string popped = Pop3ClientSimulator.AssertGetFirstMessageText(_sealed.Address, Password);
         StringAssert.Contains("-----BEGIN PGP MESSAGE-----", popped);
         StringAssert.DoesNotContain("xylophone", popped);
      }

      [Test]
      [Description("The administrator's read of a zero-access mailbox is refused by name - the message source and both exports answer 403 - while the row and its headers, which the server kept, are answered; an ordinary mailbox's source is answered.")]
      public void TheAdministratorsReadAndExportAreRefused()
      {
         SwitchOn();

         const string body = "What the administrator may not read.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "Kept header", body);
         SmtpClientSimulator.StaticSend("sender@example.org", _plain.Address, "Kept header", body);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_plain.Address, Password, 1);

         string prefix = "/api/v1/accounts/" + Uri.EscapeDataString(_sealed.Address);
         (int status, string body) listed = Admin("GET", prefix + "/messages", null);
         Assert.AreEqual(200, listed.status, listed.body);
         long id = IdOf(listed.body);

         (int status, string body) row = Admin("GET", prefix + "/messages/" + id, null);
         Assert.AreEqual(200, row.status, "The row and the kept headers are answered: " + row.body);
         StringAssert.Contains("Kept header", row.body);
         StringAssert.DoesNotContain(body, row.body);

         (int status, string body) source = Admin("GET", prefix + "/messages/" + id + "/source", null);
         Assert.AreEqual(403, source.status, source.body);
         StringAssert.Contains("\"code\":\"zero_access\"", source.body);
         StringAssert.Contains("zero-access mailbox", source.body);
         StringAssert.DoesNotContain(body, source.body);

         string exportDirectory = Path.Combine(Path.GetTempPath(), "za-export-" + Guid.NewGuid().ToString("N"));
         (int status, string body) export = Admin("POST", prefix + "/export", "{\"directory\":\"" + exportDirectory.Replace("\\", "\\\\") + "\"}");
         Assert.AreEqual(403, export.status, export.body);
         StringAssert.Contains("\"code\":\"zero_access\"", export.body);
         Assert.IsFalse(Directory.Exists(exportDirectory), "Nothing was exported.");

         (int status, string body) archive = Admin("GET", prefix + "/export.zip", null);
         Assert.AreEqual(403, archive.status, "The archive download is the export by another name.");

         // The negative control: the ordinary mailbox's source is answered,
         // body and all, and its export goes ahead.
         string plainPrefix = "/api/v1/accounts/" + Uri.EscapeDataString(_plain.Address);
         long plainId = IdOf(Admin("GET", plainPrefix + "/messages", null).body);
         (int status, string body) plainSource = Admin("GET", plainPrefix + "/messages/" + plainId + "/source", null);
         Assert.AreEqual(200, plainSource.status, plainSource.body);
         StringAssert.Contains(body, plainSource.body);
         Assert.AreEqual(200, Admin("GET", plainPrefix + "/export.zip", null).status);

         // The person's own export is theirs and stays open.
         Assert.AreEqual(200, Send("GET", "/api/v1/me/export.zip", _me, null).status, "The person's own export is not the administrator's.");
      }

      [Test]
      [Description("The opt-in is refused while the server-wide switch is off, then taken; a second switch-on is 409; the copies are checked to be the key; the mailbox key cannot leave the ring while the switch is on; and the switch-off decrypts nothing.")]
      public void TheOptInIsRefusedUntilTheServerAllowsItAndCheckedWhenItIsMade()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":false}").status);
         StoreOwnKey();

         (int status, string body) off = Send("GET", "/api/v1/me/zero-access", _me, null);
         Assert.AreEqual(200, off.status, off.body);
         StringAssert.Contains("\"allowed\":false", off.body);
         StringAssert.Contains("\"enabled\":false", off.body);

         (int status, string body) refused = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured));
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("\"code\":\"zero_access_not_allowed\"", refused.body);
         Assert.IsFalse(ZeroAccessOf(_sealed.Address), "Nothing switched.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":true}").status);

         // A copy that is not the key, and a fingerprint of no key of the
         // account's own, are refused before anything is written.
         (int status, string body) wrongKey = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingPeerSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured));
         Assert.AreEqual(400, wrongKey.status, wrongKey.body);
         StringAssert.Contains("password_wrapped", wrongKey.body);
         (int status, string body) naked = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingNakedSecretArmoured));
         Assert.AreEqual(400, naked.status, "A copy in the clear is refused: " + naked.body);
         StringAssert.Contains("recovery_wrapped", naked.body);
         (int status, string body) noSuchKey = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingPeerFingerprint, OpenPgpTestSupport.RingPeerSecretArmoured, OpenPgpTestSupport.RingPeerSecretArmoured));
         Assert.AreEqual(404, noSuchKey.status, noSuchKey.body);
         (int status, string body) weak = Send("PUT", "/api/v1/me/zero-access", _me,
            "{\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\",\"salt\":\"" + Salt + "\",\"passes\":3,\"memory_kib\":1024,\"lanes\":4,"
            + "\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"recovery_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"}");
         Assert.AreEqual(400, weak.status, "Parameters below the floor are refused: " + weak.body);
         StringAssert.Contains("memory_kib", weak.body);
         StringAssert.Contains("\"enabled\":false", Send("GET", "/api/v1/me/zero-access", _me, null).body, "Nothing was stored by a refused request.");

         (int status, string body) on = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured));
         Assert.AreEqual(201, on.status, on.body);
         StringAssert.Contains("\"enabled\":true", on.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", on.body);
         StringAssert.Contains("\"argon2\":{\"passes\":3,\"memory_kib\":65536,\"lanes\":4}", on.body);
         StringAssert.Contains("\"salt\":\"" + Salt + "\"", on.body);
         StringAssert.Contains("\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"", on.body, "The copy comes back byte for byte, which is the only way it is any use.");
         Assert.IsTrue(ZeroAccessOf(_sealed.Address), "Account.ZeroAccess over COM says on.");
         StringAssert.Contains("\"zero_access\":true", Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(_sealed.Address), null).body, "The administrator's view says on.");
         StringAssert.Contains("\"zero_access\":false", Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(_plain.Address), null).body);

         Assert.AreEqual(409, Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured)).status, "Switching on twice is refused.");

         // The key the mailbox stands on stays in the ring while the switch is on.
         (int status, string body) keyRefused = Send("DELETE", "/api/v1/me/openpgp/keys/own/" + OpenPgpTestSupport.RingOwnFingerprint, _me, null);
         Assert.AreEqual(409, keyRefused.status, keyRefused.body);
         StringAssert.Contains("\"code\":\"zero_access_key\"", keyRefused.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, "The key is still there.");
         // Nor revoked through the key page's PUT, which is the other way to a
         // key that encrypts nothing.
         (int status, string body) revokeRefused = Send("PUT", "/api/v1/me/openpgp/keys/own", _me, OwnKeyBody(OpenPgpTestSupport.RingOwnPublicBase64, true));
         Assert.AreEqual(409, revokeRefused.status, revokeRefused.body);
         StringAssert.Contains("\"code\":\"zero_access_key\"", revokeRefused.body);
         StringAssert.Contains("\"revoked\":false", Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, "The key is not revoked.");

         // An app password reads the row and writes nothing.
         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", _me, "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(_sealed.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);
         Assert.AreEqual(200, Send("GET", "/api/v1/me/zero-access", asProgram, null).status);
         Assert.AreEqual(403, Send("DELETE", "/api/v1/me/zero-access", asProgram, null).status, "An app password cannot switch it off.");
         Assert.AreEqual(403, Send("PUT", "/api/v1/me/zero-access/rewrap", asProgram, RewrapBody(OpenPgpTestSupport.RingOwnSecretArmoured)).status);
         StringAssert.Contains("\"enabled\":true", Send("GET", "/api/v1/me/zero-access", _me, null).body);

         // A re-wrap replaces the password copy and its parameters, and nothing else.
         (int status, string body) rewrapped = Send("PUT", "/api/v1/me/zero-access/rewrap", _me,
            "{\"salt\":\"EBESExQVFhcYGRobHB0eHw==\",\"passes\":2,\"memory_kib\":32768,\"lanes\":2,\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"}");
         Assert.AreEqual(200, rewrapped.status, rewrapped.body);
         StringAssert.Contains("\"salt\":\"EBESExQVFhcYGRobHB0eHw==\"", rewrapped.body);
         StringAssert.Contains("\"argon2\":{\"passes\":2,\"memory_kib\":32768,\"lanes\":2}", rewrapped.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", rewrapped.body, "The key stays the key.");

         // Off: the switch clears, the row goes, the key stays in the ring.
         (int status, string body) offAgain = Send("DELETE", "/api/v1/me/zero-access", _me, null);
         Assert.AreEqual(200, offAgain.status, offAgain.body);
         StringAssert.Contains("\"enabled\":false", offAgain.body);
         Assert.IsFalse(ZeroAccessOf(_sealed.Address));
         StringAssert.Contains("\"enabled\":false", Send("GET", "/api/v1/me/zero-access", _me, null).body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, "The key is still in the ring.");
         Assert.AreEqual(404, Send("DELETE", "/api/v1/me/zero-access", _me, null).status, "Switching off twice is a 404.");
         Assert.AreEqual(404, Send("PUT", "/api/v1/me/zero-access/rewrap", _me, RewrapBody(OpenPgpTestSupport.RingOwnSecretArmoured)).status);
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/openpgp/keys/own", _me, OwnKeyBody(OpenPgpTestSupport.RingOwnPublicBase64, true)).status, "With the switch off the key may be revoked.");
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/openpgp/keys/own/" + OpenPgpTestSupport.RingOwnFingerprint, _me, null).status, "With the switch off the key may leave the ring.");
      }

      [Test]
      [Description("With the switch off nothing changes: a delivery to a mailbox that holds a key but never opted in is stored in the clear, and one that opted out is stored in the clear from then on while what was sealed stays sealed.")]
      public void WithTheSwitchOffNothingChanges()
      {
         StoreOwnKey();
         const string body = "A body stored as it arrived.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "Before", body);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         StringAssert.Contains(body, File.ReadAllText(FileOfFirstMessage(_sealed)), "A key in the ring alone seals nothing.");

         SwitchOn(false);
         const string sealedBody = "A body under the key.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "During", sealedBody);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 2);

         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/zero-access", _me, null).status);
         const string afterBody = "A body stored as it arrived again.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "After", afterBody);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 3);

         var inbox = _sealed.IMAPFolders.get_ItemByName("INBOX");
         string during = File.ReadAllText(inbox.Messages[1].Filename);
         string after = File.ReadAllText(inbox.Messages[2].Filename);
         StringAssert.DoesNotContain(sealedBody, during, "What was sealed stays sealed after the switch-off.");
         StringAssert.Contains("-----BEGIN PGP MESSAGE-----", during);
         StringAssert.Contains(afterBody, after, "New mail is stored as it arrives.");
         StringAssert.DoesNotContain("X-hMailServer-Zero-Access", after);
      }

      [Test]
      [Description("A journal rule's copy is made at acceptance, in the clear: the organisation's journal is the organisation's, and the row is about the mailbox.")]
      public void AJournalRulesCopyIsPlaintext()
      {
         SwitchOn();
         Account journal = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "journal@" + _domain.Name, Password);
         (int status, string body) rule = Admin("POST", "/api/v1/journal-rules",
            "{\"scope\":\"account\",\"target\":\"" + _sealed.Address + "\",\"journal_address\":\"" + journal.Address + "\",\"direction\":\"inbound\",\"mode\":\"copy\",\"enabled\":true}");
         Assert.AreEqual(201, rule.status, rule.body);
         try
         {
            const string body = "The journal holds this sentence in the clear.";
            SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "Journalled", body);
            Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
            Pop3ClientSimulator.AssertMessageCount(journal.Address, Password, 1);

            string sealedStored = File.ReadAllText(FileOfFirstMessage(_sealed));
            StringAssert.DoesNotContain(body, sealedStored, "The mailbox's copy is sealed.");
            string journalled = File.ReadAllText(FileOfFirstMessage(journal));
            StringAssert.Contains(body, journalled, "The journal's copy is the message as it arrived.");
            StringAssert.Contains("X-hMailServer-Journal:", journalled);
            StringAssert.DoesNotContain("X-hMailServer-Zero-Access", journalled);
         }
         finally
         {
            Admin("DELETE", "/api/v1/journal-rules/" + IdOf(rule.body), null);
         }
      }

      [Test]
      [Description("A password change for a zero-access mailbox carries the key sealed again under the new password, or is refused; an ordinary mailbox's change carries nothing.")]
      public void APasswordChangeCarriesTheKeySealedAgainOrIsRefused()
      {
         SwitchOn();
         const string newPassword = "ZeroAccess-Passw1rd!";

         (int status, string body) refused = Send("POST", "/api/v1/me/password", _me, "{\"current\":\"" + Password + "\",\"new\":\"" + newPassword + "\"}");
         Assert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains("\"code\":\"zero_access_rewrap\"", refused.body);
         Assert.AreEqual(200, Send("GET", "/api/v1/me", _me, null).status, "The password did not change.");

         (int status, string body) badCopy = Send("POST", "/api/v1/me/password", _me,
            "{\"current\":\"" + Password + "\",\"new\":\"" + newPassword + "\",\"zero_access\":{\"salt\":\"EBESExQVFhcYGRobHB0eHw==\",\"passes\":3,\"memory_kib\":65536,\"lanes\":4,\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingPeerSecretArmoured) + "\"}}");
         Assert.AreEqual(400, badCopy.status, "A copy that is not the mailbox's key is refused before the password changes: " + badCopy.body);
         Assert.AreEqual(200, Send("GET", "/api/v1/me", _me, null).status, "The password did not change.");

         (int status, string body) changed = Send("POST", "/api/v1/me/password", _me,
            "{\"current\":\"" + Password + "\",\"new\":\"" + newPassword + "\",\"zero_access\":{\"salt\":\"EBESExQVFhcYGRobHB0eHw==\",\"passes\":3,\"memory_kib\":65536,\"lanes\":4,\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"}}");
         Assert.AreEqual(200, changed.status, changed.body);
         string asChanged = Basic(_sealed.Address, newPassword);
         Assert.AreEqual(200, Send("GET", "/api/v1/me", asChanged, null).status, "The new password signs in.");
         (int status, string body) row = Send("GET", "/api/v1/me/zero-access", asChanged, null);
         Assert.AreEqual(200, row.status, row.body);
         StringAssert.Contains("\"salt\":\"EBESExQVFhcYGRobHB0eHw==\"", row.body, "The new copy's salt is the one stored.");

         // The negative control: an ordinary mailbox changes its password with
         // nothing else in the request.
         Assert.AreEqual(200, Send("POST", "/api/v1/me/password", _other, "{\"current\":\"" + Password + "\",\"new\":\"" + newPassword + "\"}").status);
      }

      [Test]
      [Description("A key that expires is refused as the mailbox key at the switch-on, with a sentence, because every delivery would be refused from the day it expired; the same key without the expiry is taken.")]
      public void AKeyThatExpiresIsRefusedAsTheMailboxKey()
      {
         // No fixture key expires, and the server reads a signature's hashed
         // subpackets without verifying the signature, so Alice's key is given
         // a subkey expiry of a year here and nothing is re-signed.
         string expiring = WithSubkeyExpiry(OpenPgpTestSupport.RingOwnPublicBase64, 86400 * 365);
         (int status, string body) stored = Send("PUT", "/api/v1/me/openpgp/keys/own", _me, OwnKeyBody(expiring, false));
         Assert.AreEqual(201, stored.status, stored.body);

         (int status, string body) refused = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured));
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("expires", refused.body);
         StringAssert.Contains("does not expire", refused.body, "The sentence says what to do instead.");
         Assert.IsFalse(ZeroAccessOf(_sealed.Address), "Nothing switched.");
         StringAssert.Contains("\"enabled\":false", Send("GET", "/api/v1/me/zero-access", _me, null).body);

         // The negative control: the key as GnuPG made it, with no expiry,
         // replaces the row under the same fingerprint and is taken.
         (int status, string body) replaced = Send("PUT", "/api/v1/me/openpgp/keys/own", _me, OwnKeyBody(OpenPgpTestSupport.RingOwnPublicBase64, false));
         Assert.AreEqual(200, replaced.status, replaced.body);
         (int status, string body) on = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured));
         Assert.AreEqual(201, on.status, on.body);
         Assert.IsTrue(ZeroAccessOf(_sealed.Address));
      }

      [Test]
      [Description("A seal that cannot be made - here the key row gone from under a switch that is on - refuses the delivery rather than storing the message in the clear: the sender is told, the log says why, the row says broken, and the switch-off is the way out; the ordinary mailbox beside it is delivered to as ever.")]
      public void AFailureToSealRefusesTheDeliveryAndSaysSo()
      {
         SwitchOn();
         // The state the routes make impossible, made: a restore that dropped
         // the table, a database edited by hand.
         _application.Database.ExecuteSQL("delete from hm_zeroaccess");

         (int status, string body) state = Send("GET", "/api/v1/me/zero-access", _me, null);
         Assert.AreEqual(200, state.status, state.body);
         StringAssert.Contains("\"enabled\":true", state.body, "The switch is still on, and the row says so rather than 'off' beside a form that would answer 409.");
         StringAssert.Contains("\"broken\":true", state.body);
         StringAssert.Contains("switch zero access off and on again", state.body);

         const string text = "This sentence must reach nobody's disk in the clear.";
         SmtpClientSimulator.StaticSend(_plain.Address, _sealed.Address, "Refused at the seal", text);
         // The evidence in the order it is made: nothing stored for the sealed
         // mailbox once the queue has drained, the log naming the refusal, and
         // then - the sender being local - the delivery status notification in
         // its own mailbox, where it can be read. The notification is queued
         // for the next pass of the delivery queue, and asserting the queue
         // empty is what makes that pass happen now rather than in a minute.
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 0);
         CustomAsserts.AssertReportedError("7070", "could not be encrypted for the zero-access mailbox");
         Pop3ClientSimulator.AssertMessageCount(_plain.Address, Password, 1);
         string notice = Pop3ClientSimulator.AssertGetFirstMessageText(_plain.Address, Password);
         StringAssert.Contains("zero-access mailbox", notice, "The sender is told what refused the message.");
         StringAssert.Contains("4.3.0", notice, "Class 4: the recipient's address has nothing wrong with it.");

         // The negative control: the ordinary mailbox is delivered to as ever
         // (the notice above was deleted when it was read).
         SmtpClientSimulator.StaticSend("sender@example.org", _plain.Address, "Delivered", text);
         Pop3ClientSimulator.AssertMessageCount(_plain.Address, Password, 1);
         StringAssert.Contains(text, File.ReadAllText(FileOfFirstMessage(_plain)));

         // The way out is the switch-off, after which the mailbox is ordinary.
         (int status, string body) off = Send("DELETE", "/api/v1/me/zero-access", _me, null);
         Assert.AreEqual(200, off.status, off.body);
         StringAssert.Contains("\"enabled\":false", Send("GET", "/api/v1/me/zero-access", _me, null).body);
         Assert.IsFalse(ZeroAccessOf(_sealed.Address));
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "Ordinary again", text);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         StringAssert.Contains(text, File.ReadAllText(FileOfFirstMessage(_sealed)));
      }

      [Test]
      [Description("A message that arrives already encrypted - PGP/MIME from a correspondent - is stored as it arrived, one layer and no mark; a plain message to the same mailbox is sealed.")]
      public void AMessageThatArrivesEncryptedIsStoredAsItArrived()
      {
         SwitchOn();
         const string armour = "-----BEGIN PGP MESSAGE-----\r\n\r\nwV4DfixtureNotARealPacketButTheShapeOfOneAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n=za00\r\n-----END PGP MESSAGE-----\r\n";
         string raw = "From: sender@example.org\r\nTo: " + _sealed.Address + "\r\nSubject: Already under a key\r\nMIME-Version: 1.0\r\n"
                      + "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"za-fixture\"\r\n\r\n"
                      + "--za-fixture\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n"
                      + "--za-fixture\r\nContent-Type: application/octet-stream\r\n\r\n" + armour + "--za-fixture--\r\n";
         SmtpClientSimulator.StaticSendRaw("sender@example.org", _sealed.Address, raw);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         string stored = File.ReadAllText(FileOfFirstMessage(_sealed));
         StringAssert.Contains(armour, stored, "The message is stored as it arrived.");
         Assert.AreEqual(1, Regex.Matches(stored, "BEGIN PGP MESSAGE").Count, "One layer, not a second one to the mailbox key.");
         StringAssert.DoesNotContain("X-hMailServer-Zero-Access", stored, "The mark says this server sealed it; it did not.");

         // The negative control: a plain message to the same mailbox is sealed.
         const string text = "Sealed here, because it arrived in the clear.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "Plain", text);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 2);
         string second = File.ReadAllText(FileOfMessage(_sealed, "INBOX", 1));
         StringAssert.Contains("X-hMailServer-Zero-Access: sealed on delivery\r\n", second);
         StringAssert.DoesNotContain(text, second);
      }

      [Test]
      [Description("An RSA key is written to as a Curve25519 key is: the delivery is sealed, and GnuPG opens it with the RSA secret key where a gpg is to be had.")]
      public void AnRsaKeyIsWrittenToAsWell()
      {
         (int status, string body) stored = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingPeerSecretArmoured) + "\","
            + "\"address\":\"" + _sealed.Address + "\",\"name\":\"RSA\",\"source\":\"imported\"}");
         Assert.AreEqual(201, stored.status, stored.body);
         (int status, string body) on = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingPeerFingerprint, OpenPgpTestSupport.RingPeerSecretArmoured, OpenPgpTestSupport.RingPeerSecretArmoured));
         Assert.AreEqual(201, on.status, on.body);

         const string text = "Under an RSA key this time.";
         SmtpClientSimulator.StaticSend("sender@example.org", _sealed.Address, "RSA", text);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         string sealedFile = File.ReadAllText(FileOfFirstMessage(_sealed));
         StringAssert.Contains("X-hMailServer-Zero-Access: sealed on delivery\r\n", sealedFile);
         StringAssert.Contains("-----BEGIN PGP MESSAGE-----", sealedFile);
         StringAssert.DoesNotContain(text, sealedFile, "The plaintext body is in the sealed file.");

         string gpg = FindGpg();
         if (gpg == null)
         {
            TestContext.WriteLine("No gpg found on this machine: the Linux CI step is the GnuPG proof of the RSA path.");
            return;
         }
         string opened = DecryptWithGpg(gpg, ArmourOf(sealedFile), OpenPgpTestSupport.RingPeerSecretArmoured, out string why);
         if (opened == null)
         {
            TestContext.WriteLine("gpg is here but could not use the RSA secret key (" + why + "): the Linux CI step is the GnuPG proof of the RSA path.");
            return;
         }
         StringAssert.Contains(text, opened, "GnuPG's plaintext carries the body that was sent.");
      }

      [Test]
      [Description("The copy of a message sent from the webmail that is filed in the sender's Sent folder is sealed to the mailbox key; the recipient's copy left in the clear as any message does, and an ordinary account's Sent copy is the message as sent.")]
      public void TheSentCopyOfAMessageSentFromTheWebmailIsSealed()
      {
         SwitchOn();
         foreach (string address in new[] { _sealed.Address, _plain.Address })
         {
            var imap = new ImapClientSimulator();
            Assert.IsTrue(imap.ConnectAndLogon(address, Password));
            Assert.IsTrue(imap.CreateFolder("Sent"));
            imap.Disconnect();
         }

         const string text = "The copy in Sent is sealed; the recipient's is not.";
         (int status, string body) sent = Send("POST", "/api/v1/me/messages", _me,
            "{\"to\":\"" + _plain.Address + "\",\"subject\":\"From the webmail\",\"text\":\"" + text + "\"}");
         Assert.AreEqual(201, sent.status, sent.body);
         Pop3ClientSimulator.AssertMessageCount(_plain.Address, Password, 1);
         StringAssert.Contains(text, File.ReadAllText(FileOfFirstMessage(_plain)), "The recipient's copy is the message in the clear, as any message is.");

         string sentCopy = File.ReadAllText(FileOfMessage(_sealed, "Sent", 0));
         StringAssert.Contains("Subject: From the webmail", sentCopy, "The kept headers are on the Sent copy.");
         StringAssert.Contains("Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";", sentCopy);
         StringAssert.Contains("X-hMailServer-Zero-Access: sealed on delivery\r\n", sentCopy);
         StringAssert.DoesNotContain(text, sentCopy, "The Sent copy is sealed.");

         // The negative control: the ordinary account's Sent copy is the
         // message as sent.
         (int status, string body) sentPlain = Send("POST", "/api/v1/me/messages", _other,
            "{\"to\":\"" + _sealed.Address + "\",\"subject\":\"From the other\",\"text\":\"" + text + "\"}");
         Assert.AreEqual(201, sentPlain.status, sentPlain.body);
         string plainCopy = File.ReadAllText(FileOfMessage(_plain, "Sent", 0));
         StringAssert.Contains(text, plainCopy);
         StringAssert.DoesNotContain("X-hMailServer-Zero-Access", plainCopy);
      }

      // ---- helpers

      /// <summary>The account's own key into the ring, as the page stores it.</summary>
      private void StoreOwnKey()
      {
         (int status, string body) stored = Send("PUT", "/api/v1/me/openpgp/keys/own", _me, OwnKeyBody(OpenPgpTestSupport.RingOwnPublicBase64, false));
         Assert.AreEqual(201, stored.status, stored.body);
      }

      /// <summary>Alice's key as the page stores it, with the public half given and the revoked flag as asked.</summary>
      private string OwnKeyBody(string publicKeyBase64, bool revoked)
      {
         return "{\"public_key\":\"" + publicKeyBase64 + "\","
                + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\","
                + "\"address\":\"" + _sealed.Address + "\",\"name\":\"Sealed\",\"source\":\"generated\",\"revoked\":" + (revoked ? "true" : "false") + "}";
      }

      /// <summary>
      ///    The public key with a key-expiry subpacket (RFC 9580 section 5.2.3.13)
      ///    put at the front of the hashed area of its subkey's binding signature,
      ///    the hashed length and the packet length stepped by six, nothing
      ///    re-signed: the server reads what the hashed area says without
      ///    verifying the signature (OpenPgpKey.h says why), so this is the key
      ///    that expires which no fixture has. The packet is rewritten with a
      ///    new-format header, which is legal beside the old-format ones GnuPG
      ///    exports.
      /// </summary>
      private static string WithSubkeyExpiry(string publicKeyBase64, uint seconds)
      {
         byte[] key = Convert.FromBase64String(publicKeyBase64);
         int offset = 0;
         bool pastSubkey = false;
         while (offset < key.Length)
         {
            int start = offset;
            byte first = key[offset++];
            int tag, length;
            if ((first & 0x40) != 0)
            {
               tag = first & 0x3F;
               int o1 = key[offset++];
               if (o1 < 192)
                  length = o1;
               else if (o1 < 224)
                  length = ((o1 - 192) << 8) + key[offset++] + 192;
               else
               {
                  length = (key[offset] << 24) | (key[offset + 1] << 16) | (key[offset + 2] << 8) | key[offset + 3];
                  offset += 4;
               }
            }
            else
            {
               tag = (first >> 2) & 0x0F;
               int lengthType = first & 3;
               if (lengthType == 0)
                  length = key[offset++];
               else if (lengthType == 1)
               {
                  length = (key[offset] << 8) | key[offset + 1];
                  offset += 2;
               }
               else
               {
                  length = (key[offset] << 24) | (key[offset + 1] << 16) | (key[offset + 2] << 8) | key[offset + 3];
                  offset += 4;
               }
            }
            int bodyStart = offset;
            offset += length;
            if (tag == 14)
            {
               pastSubkey = true;
               continue;
            }
            if (!pastSubkey || tag != 2)
               continue;

            // The binding signature: version 4, type, two algorithms, the
            // hashed length, then the hashed area.
            var body = new List<byte>(new ArraySegment<byte>(key, bodyStart, length));
            Assert.AreEqual(4, body[0], "The binding signature is not version 4.");
            int hashed = (body[4] << 8) | body[5];
            body.InsertRange(6, new byte[] { 5, 9, (byte) (seconds >> 24), (byte) (seconds >> 16), (byte) (seconds >> 8), (byte) seconds });
            hashed += 6;
            body[4] = (byte) (hashed >> 8);
            body[5] = (byte) hashed;

            var patched = new List<byte>(new ArraySegment<byte>(key, 0, start));
            patched.Add(0xC2);
            if (body.Count < 192)
               patched.Add((byte) body.Count);
            else
            {
               patched.Add((byte) (((body.Count - 192) >> 8) + 192));
               patched.Add((byte) ((body.Count - 192) & 0xFF));
            }
            patched.AddRange(body);
            patched.AddRange(new ArraySegment<byte>(key, offset, key.Length - offset));
            return Convert.ToBase64String(patched.ToArray());
         }
         throw new InvalidOperationException("The key has no subkey binding signature to give an expiry to.");
      }

      /// <summary>The switch thrown as the page throws it, less the derivations.</summary>
      private void SwitchOn(bool storeKey = true)
      {
         if (storeKey)
            StoreOwnKey();
         (int status, string body) on = Send("PUT", "/api/v1/me/zero-access", _me, SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured));
         Assert.AreEqual(201, on.status, on.body);
      }

      private static string SwitchBody(string fingerprint, string passwordWrapped, string recoveryWrapped)
      {
         return "{\"fingerprint\":\"" + fingerprint + "\",\"salt\":\"" + Salt + "\",\"passes\":3,\"memory_kib\":65536,\"lanes\":4,"
                + "\"password_wrapped\":\"" + Escaped(passwordWrapped) + "\",\"recovery_wrapped\":\"" + Escaped(recoveryWrapped) + "\"}";
      }

      private static string RewrapBody(string passwordWrapped)
      {
         return "{\"salt\":\"" + Salt + "\",\"passes\":3,\"memory_kib\":65536,\"lanes\":4,\"password_wrapped\":\"" + Escaped(passwordWrapped) + "\"}";
      }

      private static string FileOfFirstMessage(Account account)
      {
         return FileOfMessage(account, "INBOX", 0);
      }

      private static string FileOfMessage(Account account, string folder, int index)
      {
         return account.IMAPFolders.get_ItemByName(folder).Messages[index].Filename;
      }

      /// <summary>The armoured OpenPGP message inside a stored PGP/MIME file, as gpg takes it.</summary>
      private static string ArmourOf(string stored)
      {
         string armour = stored.Substring(stored.IndexOf("-----BEGIN PGP MESSAGE-----", StringComparison.Ordinal));
         return armour.Substring(0, armour.IndexOf("-----END PGP MESSAGE-----", StringComparison.Ordinal) + "-----END PGP MESSAGE-----".Length) + "\n";
      }

      /// <summary>
      ///    Account.ZeroAccess over COM, read afresh. Late-bound: the property
      ///    arrived with schema 6083 and the test project is compiled against
      ///    the type library registered on the machine, which carries it only
      ///    once the built server has been registered - and a bool through
      ///    dynamic cannot be handed to an NUnit assertion directly.
      /// </summary>
      private bool ZeroAccessOf(string address)
      {
         dynamic account = _application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(address);
         bool value = (bool) account.ZeroAccess;
         return value;
      }

      private static long IdOf(string json)
      {
         int at = json.IndexOf("\"id\":", StringComparison.Ordinal);
         Assert.Greater(at, -1, "The answer carried no id: " + json);
         int from = at + 5;
         int to = from;
         while (to < json.Length && char.IsDigit(json[to]))
            to++;
         return long.Parse(json.Substring(from, to - from));
      }

      private static string FindGpg()
      {
         foreach (string candidate in new[] { @"C:\Program Files\Git\usr\bin\gpg.exe", @"C:\Program Files (x86)\GnuPG\bin\gpg.exe", @"C:\Program Files\GnuPG\bin\gpg.exe" })
         {
            if (File.Exists(candidate))
               return candidate;
         }
         return null;
      }

      /// <summary>The armour handed to gpg with the given fixture secret key in a scratch home; null with why when gpg could not.</summary>
      private static string DecryptWithGpg(string gpg, string armour, string secretArmoured, out string why)
      {
         why = "";
         string home = Path.Combine(Path.GetTempPath(), "za-gpg-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(home);
         try
         {
            string secret = Path.Combine(home, "fixture.sec.asc");
            File.WriteAllText(secret, secretArmoured);
            string message = Path.Combine(home, "message.asc");
            File.WriteAllText(message, armour);
            string common = "--batch --yes --no-tty --pinentry-mode loopback --passphrase " + OpenPgpTestSupport.RingPassphrase + " --homedir \"" + home + "\" ";
            RunGpg(gpg, common + "--import \"" + secret + "\"", out string importOut, out string importErr);
            if (!RunGpg(gpg, common + "--decrypt \"" + message + "\"", out string decrypted, out string decryptErr))
            {
               why = (decryptErr + importErr).Trim().Replace("\r\n", " ");
               return null;
            }
            return decrypted;
         }
         finally
         {
            try { Directory.Delete(home, true); } catch (Exception) { }
         }
      }

      private static bool RunGpg(string gpg, string arguments, out string stdout, out string stderr)
      {
         var start = new ProcessStartInfo(gpg, arguments)
         {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8
         };
         using (var process = Process.Start(start))
         {
            stdout = process.StandardOutput.ReadToEnd();
            stderr = process.StandardError.ReadToEnd();
            if (!process.WaitForExit(30000))
            {
               try { process.Kill(); } catch (Exception) { }
               return false;
            }
            return process.ExitCode == 0;
         }
      }

      /// <summary>An armoured block as one JSON string: its newlines escaped.</summary>
      private static string Escaped(string armoured)
      {
         return armoured.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
