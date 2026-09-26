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
      [Description("An app password learns that the mailbox is zero access and which key it stands on, and is not given the key's wrapped copies, the Argon2 parameters or the own keys' secret halves - each an offline test of a guess at the password; the account's own password is given all of them.")]
      public void AnAppPasswordIsNotGivenTheWrappedKeys()
      {
         // Against the code before the fix this fails: GET /api/v1/me/zero-access
         // and GET /api/v1/me/openpgp/keys answered an app password exactly as
         // they answer the password, password_wrapped, salt and secret_key in it.
         SwitchOn();

         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", _me, "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(_sealed.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);

         (int status, string body) program = Send("GET", "/api/v1/me/zero-access", asProgram, null);
         Assert.AreEqual(200, program.status, program.body);
         StringAssert.Contains("\"enabled\":true", program.body, "The app password still learns the mailbox is zero access.");
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", program.body);
         StringAssert.Contains("\"keys_withheld\":true", program.body);
         StringAssert.DoesNotContain("password_wrapped", program.body, "The copy sealed under the account password went to an app password.");
         StringAssert.DoesNotContain("recovery_wrapped", program.body);
         StringAssert.DoesNotContain("\"salt\"", program.body);
         StringAssert.DoesNotContain("argon2", program.body);
         StringAssert.DoesNotContain("PGP PRIVATE KEY", program.body);

         (int status, string body) programKeys = Send("GET", "/api/v1/me/openpgp/keys", asProgram, null);
         Assert.AreEqual(200, programKeys.status, programKeys.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", programKeys.body, "The ring is still listed.");
         StringAssert.Contains("\"secret_key_withheld\":true", programKeys.body);
         StringAssert.DoesNotContain("\"secret_key\"", programKeys.body, "An own key's secret half went to an app password.");

         // The negative control: the account's own password is given everything,
         // which is what the page it signs in to needs.
         (int status, string body) owner = Send("GET", "/api/v1/me/zero-access", _me, null);
         StringAssert.Contains("\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"", owner.body);
         StringAssert.Contains("\"argon2\":{\"passes\":3,\"memory_kib\":65536,\"lanes\":4}", owner.body);
         StringAssert.DoesNotContain("keys_withheld", owner.body);
         StringAssert.Contains("\"secret_key\":\"", Send("GET", "/api/v1/me/openpgp/keys", _me, null).body);

         // And what the row may name is held to OWASP's Argon2id floor, two
         // passes over 19 MiB: the 8 MiB and one pass the server took before
         // would have made each guess cheap for whoever holds the copy.
         (int status, string body) cheap = Send("PUT", "/api/v1/me/zero-access/rewrap", _me,
            "{\"salt\":\"" + Salt + "\",\"passes\":1,\"memory_kib\":8192,\"lanes\":1,\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"}");
         Assert.AreEqual(400, cheap.status, "A re-wrap at 8 MiB and one pass was taken: " + cheap.body);
         (int status, string body) floor = Send("PUT", "/api/v1/me/zero-access/rewrap", _me,
            "{\"salt\":\"" + Salt + "\",\"passes\":2,\"memory_kib\":19456,\"lanes\":1,\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"}");
         Assert.AreEqual(200, floor.status, "A re-wrap at the floor was refused: " + floor.body);
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
         string opened = OpenWithGpg(ArmourOf(stored), OpenPgpTestSupport.RingOwnSecretArmoured);
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
         // Alice's key with her subkey's binding signed again by her primary to
         // give it an expiry (OpenPgpTestSupport.RingOwnPublicExpiringBase64):
         // a binding merely edited would no longer verify, and the server
         // verifies it before it encrypts to the subkey.
         string expiring = OpenPgpTestSupport.RingOwnPublicExpiringBase64;
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
         // Real OpenPGP packets: a public-key session key packet and then
         // encrypted data. What is under a key is decided from the packets, not
         // the label, so a fixture of any other base64 is sealed (see
         // ALabelIsNotEnoughToBeStoredUnsealed).
         const string armour = "-----BEGIN PGP MESSAGE-----\r\n\r\nwQwDAAAAAAAAAAABAAHSGQFzZWFsZWQtY2lwaGVydGV4dC1tYXJrZXI=\r\n=za00\r\n-----END PGP MESSAGE-----\r\n";
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
      [Description("A message that only SAYS it is encrypted is sealed: a multipart/encrypted label over ordinary text, and an S/MIME v2 opaque-signed message labelled application/pkcs7-mime with no smime-type, whose content anyone can read. Neither is under a key, so neither is stored as it arrived.")]
      public void ALabelIsNotEnoughToBeStoredUnsealed()
      {
         // Against the code before the fix this fails: the label alone decided
         // "already encrypted", and both messages were stored in the clear in a
         // mailbox that promises the administrator cannot read it.
         SwitchOn();

         const string secret = "Readable text under an encrypted label.";
         string fakeLabel = "From: sender@example.org\r\nTo: " + _sealed.Address + "\r\nSubject: Only the label\r\nMIME-Version: 1.0\r\n"
                            + "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"za-label\"\r\n\r\n"
                            + "--za-label\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n"
                            + "--za-label\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\n" + secret + "\r\n"
                            + "--za-label--\r\n";
         SmtpClientSimulator.StaticSendRaw("sender@example.org", _sealed.Address, fakeLabel);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         string first = File.ReadAllText(FileOfFirstMessage(_sealed));
         StringAssert.Contains("X-hMailServer-Zero-Access: sealed on delivery\r\n", first, "A multipart/encrypted label over plain text was stored as it arrived.");
         StringAssert.DoesNotContain(secret, first, "The text under the label is on the disk in the clear.");

         // The start of a CMS signed-data object (1.2.840.113549.1.7.2), which
         // an S/MIME v2 client sends as application/pkcs7-mime with no
         // smime-type: signed, not encrypted.
         string opaqueSigned = "From: sender@example.org\r\nTo: " + _sealed.Address + "\r\nSubject: Signed, not encrypted\r\nMIME-Version: 1.0\r\n"
                               + "Content-Type: application/pkcs7-mime; name=\"smime.p7m\"\r\nContent-Transfer-Encoding: base64\r\n"
                               + "Content-Disposition: attachment; filename=\"smime.p7m\"\r\n\r\n"
                               + "MIAGCSqGSIb3DQEHAqCAMIACAQE=\r\n";
         SmtpClientSimulator.StaticSendRaw("sender@example.org", _sealed.Address, opaqueSigned);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 2);
         string second = File.ReadAllText(FileOfMessage(_sealed, "INBOX", 1));
         StringAssert.Contains("X-hMailServer-Zero-Access: sealed on delivery\r\n", second, "Opaque signed data, which anyone can read, was stored as it arrived.");
         StringAssert.DoesNotContain("MIAGCSqGSIb3DQEHAqCAMIACAQE=", second, "The signed-data body is on the disk as it arrived.");
      }

      [Test]
      [Description("A PGP/MIME message that holds real ciphertext AND a readable part beside it is sealed, not stored as it arrived: only a message wholly under a key - the version part and the ciphertext, and nothing else - is kept as it came.")]
      public void AnEncryptedPartDoesNotCarryAReadablePartPastTheSeal()
      {
         // Against the code before the fix this fails: GatewayEncryption's
         // question - is there an OpenPGP message under a key in here? - is
         // answered yes by the ciphertext part, and the message was stored as it
         // arrived, the third part's text on the disk in the clear.
         SwitchOn();

         const string armour = "-----BEGIN PGP MESSAGE-----\r\n\r\nwQwDAAAAAAAAAAABAAHSGQFzZWFsZWQtY2lwaGVydGV4dC1tYXJrZXI=\r\n=za00\r\n-----END PGP MESSAGE-----\r\n";
         const string readable = "A readable part riding beside real ciphertext.";
         string raw = "From: sender@example.org\r\nTo: " + _sealed.Address + "\r\nSubject: Ciphertext and a passenger\r\nMIME-Version: 1.0\r\n"
                      + "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"za-extra\"\r\n\r\n"
                      + "--za-extra\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n"
                      + "--za-extra\r\nContent-Type: application/octet-stream\r\n\r\n" + armour
                      + "--za-extra\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\n" + readable + "\r\n"
                      + "--za-extra--\r\n";
         SmtpClientSimulator.StaticSendRaw("sender@example.org", _sealed.Address, raw);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 1);
         string stored = File.ReadAllText(FileOfFirstMessage(_sealed));
         StringAssert.Contains("X-hMailServer-Zero-Access: sealed on delivery\r\n", stored, "Real ciphertext with a readable part beside it was stored as it arrived.");
         StringAssert.DoesNotContain(readable, stored, "The readable part is on the disk in the clear.");

         // The negative control, AMessageThatArrivesEncryptedIsStoredAsItArrived's
         // message: the same two parts without the passenger stay as they came.
         string whole = "From: sender@example.org\r\nTo: " + _sealed.Address + "\r\nSubject: Only ciphertext\r\nMIME-Version: 1.0\r\n"
                        + "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"za-whole\"\r\n\r\n"
                        + "--za-whole\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n"
                        + "--za-whole\r\nContent-Type: application/octet-stream\r\n\r\n" + armour + "--za-whole--\r\n";
         SmtpClientSimulator.StaticSendRaw("sender@example.org", _sealed.Address, whole);
         Pop3ClientSimulator.AssertMessageCount(_sealed.Address, Password, 2);
         string second = File.ReadAllText(FileOfMessage(_sealed, "INBOX", 1));
         StringAssert.Contains(armour, second);
         StringAssert.DoesNotContain("X-hMailServer-Zero-Access", second, "A message wholly under a key was given a second layer.");
      }

      [Test]
      [Description("An administrator's support session reads the zero-access state and the rings, and changes none of it: the switch on or off, the re-wrap, the account's own OpenPGP keys, a correspondent's key and what the account said about it are the account holder's alone. The account's own password does each of them.")]
      public void ASupportSessionCannotChangeTheKeysOrTheSwitch()
      {
         // Against the code before the fix this fails: only an app password was
         // refused on these writes, so an administrator who opened a support
         // session could switch zero access off - the next delivery then stored
         // in the clear for the administrator the switch keeps out - replace the
         // password copy, plant a key of their own in the ring or a
         // correspondent's, or mark one verified.
         StoreOwnKey();
         (int status, string body) peer = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me, PeerKeyBody());
         Assert.AreEqual(201, peer.status, peer.body);
         long peerId = IdOf(peer.body);

         Assert.AreEqual(200, Send("PUT", "/api/v1/me/preferences", _me, "{\"support_allowed\":\"1\"}").status, "The account could not allow support access.");
         (int status, string headers, string body) opened = RestRequest.Exchange("127.0.0.1", RestPort, "POST", "/api/v1/accounts/" + _sealed.Address + "/support-session",
            Basic("Administrator", AdminPassword), null, null);
         Assert.AreEqual(201, opened.status, opened.body);
         string cookie = Regex.Match(opened.headers, "hmailsession=([^;\r\n]+)").Groups[1].Value;
         Assert.IsFalse(string.IsNullOrEmpty(cookie), opened.headers);
         string session = "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer";

         // It is a working session: it reads as the account.
         (int status, string body) reads = AsSupport("GET", "/api/v1/me/zero-access", session, null);
         Assert.AreEqual(200, reads.status, reads.body);
         StringAssert.Contains("\"enabled\":false", reads.body);

         // The switch, thrown on.
         (int status, string body) on = AsSupport("PUT", "/api/v1/me/zero-access", session,
            SwitchBody(OpenPgpTestSupport.RingOwnFingerprint, OpenPgpTestSupport.RingOwnSecretArmoured, OpenPgpTestSupport.RingOwnSecretArmoured));
         AssertSupportRefused(on, "switched the mailbox to zero access");
         Assert.IsFalse(ZeroAccessOf(_sealed.Address), "The support session's switch-on went through.");

         SwitchOn(false);

         AssertSupportRefused(AsSupport("PUT", "/api/v1/me/zero-access/rewrap", session, RewrapBody(OpenPgpTestSupport.RingOwnSecretArmoured)), "re-wrapped the mailbox key");
         AssertSupportRefused(AsSupport("DELETE", "/api/v1/me/zero-access", session, null), "switched zero access off");
         Assert.IsTrue(ZeroAccessOf(_sealed.Address), "The support session switched zero access off.");

         AssertSupportRefused(AsSupport("PUT", "/api/v1/me/openpgp/keys/own", session, OwnKeyBody(OpenPgpTestSupport.RingOwnPublicBase64, false)), "stored an own key");
         AssertSupportRefused(AsSupport("DELETE", "/api/v1/me/openpgp/keys/own/" + OpenPgpTestSupport.RingOwnFingerprint, session, null), "removed an own key");
         AssertSupportRefused(AsSupport("PUT", "/api/v1/me/openpgp/keys/correspondents", session, PeerKeyBody()), "stored a correspondent's key");
         AssertSupportRefused(AsSupport("PUT", "/api/v1/me/openpgp/keys/" + peerId + "/verified", session, "{\"verified\":true}"), "marked a key verified");
         AssertSupportRefused(AsSupport("PUT", "/api/v1/me/openpgp/keys/" + peerId + "/accepted", session, "{\"accepted\":true}"), "accepted a change of key");
         AssertSupportRefused(AsSupport("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingPeerFingerprint, session, null), "forgot a correspondent's key");

         // Nothing changed, and the session still reads - without the copies.
         (int status, string body) stillReads = AsSupport("GET", "/api/v1/me/zero-access", session, null);
         Assert.AreEqual(200, stillReads.status, stillReads.body);
         StringAssert.Contains("\"keys_withheld\":true", stillReads.body);
         (int status, string body) ring = Send("GET", "/api/v1/me/openpgp/keys", _me, null);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", ring.body, "The support session removed the correspondent's key.");
         StringAssert.DoesNotContain("\"verified\":true", ring.body, "The support session's mark is on the key.");

         // The negative control: the account's own password does each of them.
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/openpgp/keys/" + peerId + "/verified", _me, "{\"verified\":true}").status, "The account holder could not mark the key.");
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingPeerFingerprint, _me, null).status, "The account holder could not forget the key.");
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/zero-access/rewrap", _me, RewrapBody(OpenPgpTestSupport.RingOwnSecretArmoured)).status, "The account holder could not re-wrap.");
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/zero-access", _me, null).status, "The account holder could not switch zero access off.");
         Assert.IsFalse(ZeroAccessOf(_sealed.Address));
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

         string opened = OpenWithGpg(ArmourOf(sealedFile), OpenPgpTestSupport.RingPeerSecretArmoured);
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

      /// <summary>
      ///    The plaintext GnuPG makes of the armour with the given fixture secret
      ///    key, in a scratch home. A proof that needs gpg is INCONCLUSIVE when
      ///    there is no gpg, or it cannot import the key - the environment, not
      ///    the server - and never passed; it FAILS when gpg imported the key and
      ///    cannot open what the server wrote. (The gateway encryption fixture,
      ///    SMTP/GatewayEncryption.cs, was reviewed for passing on both; this is
      ///    the same shape, swept here with it.)
      /// </summary>
      private static string OpenWithGpg(string armour, string secretArmoured)
      {
         string gpg = FindGpg();
         if (gpg == null)
            Assert.Inconclusive("No gpg on this machine, so the sealed message was not opened: everything else in this test held. The Linux CI step and build/check-portal-openpgp.js are the GnuPG proof of the writer.");

         string home = Path.Combine(Path.GetTempPath(), "za-gpg-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(home);
         try
         {
            string secret = Path.Combine(home, "fixture.sec.asc");
            File.WriteAllText(secret, secretArmoured);
            string message = Path.Combine(home, "message.asc");
            File.WriteAllText(message, armour);
            string common = "--batch --yes --no-tty --pinentry-mode loopback --passphrase " + OpenPgpTestSupport.RingPassphrase + " --homedir \"" + OpenPgpTestSupport.GpgHome(gpg, home) + "\" ";

            if (!RunGpg(gpg, common + "--import \"" + secret + "\"", out string _, out string importErr))
               Assert.Inconclusive("gpg is here and could not import the fixture's secret key (" + importErr.Trim().Replace("\r\n", " ") + "), so the sealed message was not opened: the environment, not the server.");

            if (!RunGpg(gpg, common + "--decrypt \"" + message + "\"", out string decrypted, out string decryptErr))
               Assert.Fail("gpg imported the fixture's secret key and could not open the sealed message: " + decryptErr.Trim().Replace("\r\n", " "));

            return decrypted;
         }
         finally
         {
            // The agent gpg started for this home is stopped before the home goes.
            string gpgconf = Path.Combine(Path.GetDirectoryName(gpg), "gpgconf.exe");
            if (File.Exists(gpgconf))
               RunGpg(gpgconf, "--homedir \"" + OpenPgpTestSupport.GpgHome(gpg, home) + "\" --kill gpg-agent", out string _, out string _);

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

      /// <summary>Bob's public key as the page stores a correspondent's.</summary>
      private static string PeerKeyBody()
      {
         return "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingPeerAddress
                + "\",\"name\":\"Bob\",\"source\":\"imported\"}";
      }

      /// <summary>A request in the support session whose cookie and page header are in session.</summary>
      private static (int status, string body) AsSupport(string method, string path, string session, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, null, requestBody, session);
      }

      private static void AssertSupportRefused((int status, string body) answer, string what)
      {
         Assert.AreEqual(403, answer.status, "A support session " + what + ": " + answer.body);
         StringAssert.Contains("support_session", answer.body, "Refused, but not as a support session: " + answer.body);
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
