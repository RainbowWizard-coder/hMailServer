// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text;
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
   ///    A password-protected message to anyone (ProtectedMessages.h,
   ///    RestApiProtectedMessages.cpp): the page encrypts the text and the
   ///    attachments to a password as one OpenPGP message and hands this server the
   ///    ciphertext in chunks; the server files it, sends the recipient a short
   ///    notice with a link and the hint, and never has the plaintext.
   ///
   ///    The CRYPTOGRAPHY is not under test here - it is the page's, and
   ///    build/check-portal-openpgp.js holds it to GnuPG in both directions. What
   ///    is under test is what this server does with what the page hands it: that
   ///    the bytes it stores are the ciphertext and not the text, that anything
   ///    that is not ciphertext to a passphrase is discarded rather than filed,
   ///    that the link is 128 bits of base64url that anyone may fetch and a wrong
   ///    id is a 404 that says nothing, that the notice carries the link and the
   ///    hint and nothing of the text, that a withdrawal and an expiry end it, that
   ///    the ceilings refuse with a sentence, that a reply is filed under the
   ///    original's sender and the sender told, and that an app password cannot
   ///    make one.
   ///
   ///    The vector below was made by the page's own module
   ///    (PortalOpenPgp.protectedMessage) under the password "correct-horse", over
   ///    a MIME message whose text is "The figures are 42."; the server checks
   ///    only its shape, so any symmetric OpenPGP message would do.
   /// </summary>
   [TestFixture]
   public class RestApiProtectedMessages : TestFixtureBase
   {
      private static int RestPort = 9776;
      private const string AdminPassword = "testar";
      private const string Password = "Protected-Passw0rd!";

      // What the page makes: an ASCII-armoured PGP MESSAGE whose first packet is
      // a symmetric-key encrypted session key (tag 3, "wy4E" once decoded) and
      // whose body is an integrity-protected data packet (tag 18).
      private const string Vector =
         "-----BEGIN PGP MESSAGE-----\r\n" +
         "\r\n" +
         "wy4ECQMIDhJlWZpqtOn//QKmQPPvj1yaROry6YGBUB8MzkD0MHUdgdp+pnBWPeY0\r\n" +
         "0sBgAYVwPMhCdjpNBsnsq5HpCBOwwtnQOyOtMs8Uy30X4BirtxKdzuGSLDIf6vYd\r\n" +
         "D11hoBg1x9yzbY1TMs5uYgVBwr1g7aEwvF69+Da2fLm+RgKilQRD1pKRrIQ/0Tud\r\n" +
         "L8JDlqOySn1rpaZRHxoj3CaueWA23l3AOgKE8GgnRiEQP4k6xaveUVsfrNAHn1vh\r\n" +
         "YI+E9cqDSR2S+HRl+Up3Fl9OiaJhhwVEfqWthGduKCy0/A7G08kK5b14gjXXRN4u\r\n" +
         "32MUpgsD1B6krUyOIvvuoAmBE3pUfxAkboQO1UqKZnsP1INxNQwkyNWBXmczqr0j\r\n" +
         "iaU94FyU6HQmh+UWyJFf+b+FsHdHlPd8RjrRoxYRKeHwVpO/D790yPWnbj9Wy5QD\r\n" +
         "9soF\r\n" +
         "=hiVe\r\n" +
         "-----END PGP MESSAGE-----\r\n";

      private Account _sender;
      private Account _reader;
      private string _me;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"http://127.0.0.1:" + RestPort + "\"}").status);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}").status);

         _sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "protected-sender@" + _domain.Name, Password);
         _reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "protected-reader@" + _domain.Name, Password);
         _me = Basic(_sender.Address, Password);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_sender.Address, Password));
         Assert.IsTrue(imap.CreateFolder("Sent"));
         imap.Disconnect();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\"}");
            Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The ciphertext is stored as it arrived, in chunks, and the file holds the ciphertext and not the text; the link is 128 bits of base64url; the notice carries the link and the hint and nothing of the text; the sender keeps a copy of the notice in Sent; the fetch needs no credential and marks it opened; a wrong id is a 404; a withdrawal ends it and deletes the file.")]
      public void TheCiphertextIsStoredAndTheNoticeCarriesTheLink()
      {
         (int status, string body) made = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"hint\":\"the usual word\",\"size\":" + Vector.Length + "}");
         Assert.AreEqual(201, made.status, made.body);
         long id = long.Parse(Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value);
         string token = Regex.Match(made.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;
         Assert.IsTrue(Regex.IsMatch(token, "^[A-Za-z0-9_-]{22}$"), "The token is 22 characters of base64url - 128 bits: " + token);
         StringAssert.Contains("\"link\":\"http://127.0.0.1:" + RestPort + "/portal/protected/" + token + "\"", made.body);
         StringAssert.Contains("\"complete\":false", made.body);
         StringAssert.Contains("\"hint\":\"the usual word\"", made.body);

         // Nothing has been sent, and the page fetch does not find a record that
         // is not finished - a half-uploaded blob is nobody's to read.
         Pop3ClientSimulator.AssertMessageCount(_reader.Address, Password, 0);
         Assert.AreEqual(404, Send("GET", "/api/v1/protected/" + token, null, null).status);

         // Two chunks, the second refused until the first has landed.
         int half = Vector.Length / 2;
         (int status, string body) wrongOffset = Put("/api/v1/me/protected/" + id + "/content?offset=" + half, _me, Vector.Substring(half));
         Assert.AreEqual(409, wrongOffset.status, wrongOffset.body);
         StringAssert.Contains("\"stored\":0", wrongOffset.body);
         (int status, string body) first = Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, Vector.Substring(0, half));
         Assert.AreEqual(200, first.status, first.body);
         StringAssert.Contains("\"stored\":" + half + ",", first.body);
         StringAssert.Contains("\"complete\":false", first.body);
         (int status, string body) second = Put("/api/v1/me/protected/" + id + "/content?offset=" + half, _me, Vector.Substring(half));
         Assert.AreEqual(200, second.status, second.body);
         StringAssert.Contains("\"complete\":true", second.body);
         StringAssert.Contains("\"sent_copy\":true", second.body);
         Assert.AreEqual(409, Put("/api/v1/me/protected/" + id + "/content?offset=" + Vector.Length, _me, "x").status, "A complete message takes no more bytes.");

         // The file on disk is the ciphertext, byte for byte, and not the text.
         string stored = Path.Combine(_settings.Directories.DataDirectory, "Protected", _sender.ID.ToString(), token + ".asc");
         Assert.IsTrue(File.Exists(stored), "The ciphertext is kept at " + stored);
         Assert.AreEqual(Vector, File.ReadAllText(stored), "What is stored is what the page sent.");
         StringAssert.DoesNotContain("The figures are 42.", File.ReadAllText(stored));

         // The notice: the link and the hint, from the sender, and nothing of the text.
         string notice = Pop3ClientSimulator.AssertGetFirstMessageText(_reader.Address, Password).Replace("=\r\n", "");
         StringAssert.Contains("Subject: A password-protected message from", notice);
         StringAssert.Contains("http://127.0.0.1:" + RestPort + "/portal/protected/" + token, notice);
         StringAssert.Contains("the usual word", notice);
         StringAssert.Contains("X-hMailServer-Protected: " + token, notice);
         StringAssert.DoesNotContain("The figures are 42.", notice);
         StringAssert.DoesNotContain("BEGIN PGP MESSAGE", notice, "The ciphertext does not go by mail either; only the link does.");
         Assert.IsTrue(Regex.IsMatch(notice, "(?m)^From: .*" + Regex.Escape(_sender.Address)), "The notice is from the sender: " + notice);

         // The sender's Sent folder holds the notice.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_sender.Address, Password));
         Assert.AreEqual(1, imap.GetMessageCount("Sent"));
         imap.Disconnect();

         // The fetch, with no credential at all: the ciphertext, the hint, the
         // dates - and nothing about who the recipient is.
         (int status, string body) fetched = Send("GET", "/api/v1/protected/" + token, null, null);
         Assert.AreEqual(200, fetched.status, fetched.body);
         StringAssert.Contains("\"hint\":\"the usual word\"", fetched.body);
         StringAssert.Contains("\"reply\":false", fetched.body);
         StringAssert.Contains("\"can_reply\":true", fetched.body);
         StringAssert.Contains("wy4ECQMI", fetched.body, "The message is the armour the page sent.");
         StringAssert.DoesNotContain(_reader.Address, fetched.body, "The fetch does not say who the message is for.");

         // The sender sees it opened, which is all this server can know.
         (int status, string body) listed = Send("GET", "/api/v1/me/protected", _me, null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"available\":true", listed.body);
         StringAssert.Contains("\"recipient\":\"" + _reader.Address.ToLowerInvariant() + "\"", listed.body);
         Assert.IsTrue(Regex.IsMatch(listed.body, "\"opened\":[1-9]\\d*"), "Opened is when the ciphertext was first fetched: " + listed.body);
         StringAssert.Contains("\"days\":30", listed.body);

         // A wrong id: the same 404 whether it never existed or is somebody's -
         // and a plain 404, counted against nobody. A page fetches its link on
         // load, and a link older than its row is exactly such a token: four
         // fetches of one, more than the auto-ban's three failures, leave the
         // address able to sign in.
         for (int i = 0; i < 4; i++)
         {
            (int status, string body) unknown = Send("GET", "/api/v1/protected/" + new string('A', 22), null, null);
            Assert.AreEqual(404, unknown.status, unknown.body);
            StringAssert.DoesNotContain("expire", unknown.body);
         }
         Assert.AreEqual(200, Send("GET", "/api/v1/me/protected", _me, null).status, "Four stale links fetched, and the address signs in as before.");
         Assert.AreEqual(404, Send("GET", "/api/v1/protected/not-a-token", null, null).status);
         Assert.AreEqual(404, Send("GET", "/api/v1/protected/" + token.ToUpperInvariant().Substring(0, 21) + "!", null, null).status);

         // Withdrawn: the fetch says so, the file is gone, the row stays as revoked.
         Assert.AreEqual(404, Send("DELETE", "/api/v1/me/protected/" + id, Basic(_reader.Address, Password), null).status, "Another account's message is not found.");
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/protected/" + id, _me, null).status);
         (int status, string body) gone = Send("GET", "/api/v1/protected/" + token, null, null);
         Assert.AreEqual(410, gone.status, gone.body);
         StringAssert.Contains("withdrawn", gone.body);
         Assert.IsFalse(File.Exists(stored), "The ciphertext is deleted on withdrawal.");
         StringAssert.Contains("\"revoked\":true", Send("GET", "/api/v1/me/protected", _me, null).body);
      }

      [Test]
      [Description("What does not have the shape of ciphertext to a passphrase is discarded, not filed: the text in the clear, bytes that are not an OpenPGP message at all, a message encrypted to a key, a literal packet slipped in between the session key and the body, and a packet after the body - each with a sentence, and the record gone. Nothing is sent for any of them.")]
      public void WhatIsNotCiphertextIsDiscarded()
      {
         // The vector's own two packets, to build the impostors that carry
         // real ciphertext beside what does not belong: 48 bytes of session
         // key packet, then the integrity-protected body.
         byte[] packets = PacketsOf(Vector);
         byte[] sessionKey = new byte[48];
         byte[] sealedBody = new byte[packets.Length - 48];
         Array.Copy(packets, 0, sessionKey, 0, 48);
         Array.Copy(packets, 48, sealedBody, 0, sealedBody.Length);
         Assert.AreEqual(0xc3, packets[0], "The vector begins with a symmetric-key encrypted session key packet.");
         Assert.AreEqual(0xd2, packets[48], "And its second packet is the integrity-protected body.");
         // A literal data packet (tag 11): binary, no name, no date, and the text.
         byte[] literal = Concat(new byte[] { 0xcb, 11, 0x62, 0, 0, 0, 0, 0 }, Encoding.ASCII.GetBytes("hello"));

         (string bytes, string sentence)[] impostors =
         {
            ("The figures are 42.\r\n", "not an OpenPGP message"),
            ("-----BEGIN PGP MESSAGE-----\r\n\r\nVGhlIGZpZ3VyZXMgYXJlIDQyLg==\r\n-----END PGP MESSAGE-----\r\n", "not an OpenPGP message"),
            // A public-key encrypted session key packet: tag 1 in the old format, one byte of body.
            ("-----BEGIN PGP MESSAGE-----\r\n\r\nhQABAA==\r\n-----END PGP MESSAGE-----\r\n", "encrypted to a key"),
            // A plaintext filed under the name of ciphertext: the session key, a literal packet, the body.
            (Armoured(Concat(sessionKey, literal, sealedBody)), "packet 11 has no place"),
            // Bytes nobody would decrypt: the body, then a literal packet after it.
            (Armoured(Concat(sessionKey, sealedBody, literal)), "something follows the encrypted body"),
            // A session key packet that is far too large to be one.
            (Armoured(Concat(new byte[] { 0xc3, 0xc0, 0x00 }, new byte[192], sealedBody)), "larger than one can be")
         };
         foreach ((string impostor, string sentence) in impostors)
         {
            (int status, string body) made = Send("POST", "/api/v1/me/protected", _me,
               "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + impostor.Length + "}");
            Assert.AreEqual(201, made.status, made.body);
            long id = long.Parse(Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value);
            string token = Regex.Match(made.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;
            (int status, string body) refused = Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, impostor);
            Assert.AreEqual(400, refused.status, refused.body);
            StringAssert.Contains("\"discarded\":true", refused.body);
            StringAssert.Contains(sentence, refused.body);
            Assert.AreEqual(404, Send("GET", "/api/v1/protected/" + token, null, null).status, "The record is gone.");
            Assert.IsFalse(File.Exists(Path.Combine(_settings.Directories.DataDirectory, "Protected", _sender.ID.ToString(), token + ".asc")), "And so is the file.");
         }
         Pop3ClientSimulator.AssertMessageCount(_reader.Address, Password, 0);
         StringAssert.DoesNotContain("\"complete\":true", Send("GET", "/api/v1/me/protected", _me, null).body);

         // The negative control: the vector's own packets, armoured by this
         // fixture without a checksum line, are the shape and are filed.
         string genuine = Armoured(Concat(sessionKey, sealedBody));
         (int status, string body) accepted = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + genuine.Length + "}");
         Assert.AreEqual(201, accepted.status, accepted.body);
         (int status, string body) filed = Put("/api/v1/me/protected/" + Regex.Match(accepted.body, "\"id\":(\\d+)").Groups[1].Value + "/content?offset=0", _me, genuine);
         Assert.AreEqual(200, filed.status, filed.body);
         StringAssert.Contains("\"complete\":true", filed.body);
      }

      [Test]
      [Description("More bytes than were declared are refused, and a second chunk for an offset the file has already passed is answered with where the file really is - judged under the lock the append takes.")]
      public void TheChunksAreHeldToTheDeclaredSizeAndTheFile()
      {
         (int status, string body) made = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + (Vector.Length - 10) + "}");
         Assert.AreEqual(201, made.status, made.body);
         long id = long.Parse(Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) over = Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, Vector);
         Assert.AreEqual(413, over.status, over.body);
         StringAssert.Contains("more bytes than the size that was declared", over.body);

         int half = Vector.Length / 2;
         (int status, string body) first = Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, Vector.Substring(0, half));
         Assert.AreEqual(200, first.status, first.body);
         // The file has the first half; a chunk that says it starts at 0 again
         // is refused with where the file has reached, whatever the row said.
         _application.Database.ExecuteSQL("update hm_protectedmessages set pmstored = 0 where pmid = " + id);
         (int status, string body) again = Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, Vector.Substring(0, half));
         Assert.AreEqual(409, again.status, again.body);
         StringAssert.Contains("\"stored\":" + half, again.body);
         string stored = Path.Combine(_settings.Directories.DataDirectory, "Protected", _sender.ID.ToString(), Regex.Match(made.body, "\"token\":\"([^\"]+)\"").Groups[1].Value + ".asc");
         Assert.AreEqual(half, new FileInfo(stored).Length, "The first half, once, and nothing appended twice.");
      }

      [Test]
      [Description("The ceilings refuse with a sentence: a message over 25 MB, more than the account's 250 MB outstanding (counted from the declared sizes), a hint too long, an expiry past the server's days, a recipient that is not an address - and a request authenticated with an app password cannot make one at all.")]
      public void TheCeilingsRefuseWithASentence()
      {
         (int status, string body) tooBig = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + (25L * 1024 * 1024 + 1) + "}");
         Assert.AreEqual(413, tooBig.status, tooBig.body);
         StringAssert.Contains("25 MB", tooBig.body);

         // Held to the 25 MB as the number it is, before it is made a whole
         // number, so that a size past it is refused whatever its magnitude and
         // counts against nothing: the twelve below still fit.
         foreach (string size in new[] { "9.3e18", "1e20", "1e308" })
         {
            (int status, string body) large = Send("POST", "/api/v1/me/protected", _me,
               "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + size + "}");
            Assert.AreEqual(413, large.status, "A size of " + size + ": " + large.body);
            StringAssert.Contains("25 MB", large.body);
         }

         // Twelve declared at 20 MB fit under 250; the thirteenth does not.
         for (int i = 0; i < 12; i++)
         {
            (int status, string body) declared = Send("POST", "/api/v1/me/protected", _me,
               "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + (20L * 1024 * 1024) + "}");
            Assert.AreEqual(201, declared.status, "Declared " + i + ": " + declared.body);
         }
         (int status, string body) over = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + (20L * 1024 * 1024) + "}");
         Assert.AreEqual(413, over.status, over.body);
         StringAssert.Contains("250 MB", over.body);
         StringAssert.Contains("protected_ceiling", over.body);

         Assert.AreEqual(400, Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"not-an-address\",\"size\":10}").status, "A recipient that is not an address.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":10,\"hint\":\"" + new string('h', 256) + "\"}").status, "A hint past 255 characters.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":10,\"expires_in\":" + (31L * 24 * 60 * 60) + "}").status, "An expiry past the server's thirty days.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":0}").status, "No bytes at all.");

         // An app password is a credential handed to a mail program: it reads
         // the mailbox and sends mail, and it does not make this server write a
         // link to itself into a message in the person's name.
         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", _me,
            "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(_sender.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);
         (int status, string body) refused = Send("POST", "/api/v1/me/protected", asProgram,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":10}");
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("app password", refused.body);
         // Nor the bytes, nor the withdrawal, nor the list - which carries
         // every live link and hint, more than a program that reads mail needs.
         (int status, string body) made = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + Vector.Length + "}");
         Assert.AreEqual(201, made.status, made.body);
         long id = long.Parse(Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value);
         Assert.AreEqual(403, Put("/api/v1/me/protected/" + id + "/content?offset=0", asProgram, Vector).status, "The bytes are refused to an app password.");
         Assert.AreEqual(403, Send("DELETE", "/api/v1/me/protected/" + id, asProgram, null).status, "So is the withdrawal.");
         (int status, string body) listed = Send("GET", "/api/v1/me/protected", asProgram, null);
         Assert.AreEqual(403, listed.status, listed.body);
         StringAssert.Contains("app password", listed.body);
         Assert.AreEqual(200, Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, Vector).status, "The account's own password does all of it (negative control).");
         Assert.AreEqual(200, Send("GET", "/api/v1/me/protected", _me, null).status);
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/protected/" + id, _me, null).status);
      }

      [Test]
      [Description("The routes that take no credential are held to their own ceilings: a reply over 1 MB is refused, an original collects twenty replies and refuses the twenty-first, and the per-address budget exempts loopback - a hundred and twenty-five fetches from this machine are all answered.")]
      public void TheUnauthenticatedRoutesHaveTheirOwnCeilings()
      {
         string token = SendOne("");
         Pop3ClientSimulator.AssertGetFirstMessageText(_reader.Address, Password);

         (int status, string body) tooBig = Send("POST", "/api/v1/protected/" + token + "/reply", null, "{\"message\":\"" + new string('A', 1024 * 1024 + 1) + "\"}");
         Assert.AreEqual(413, tooBig.status, tooBig.body);
         StringAssert.Contains("1 MB", tooBig.body);

         for (int i = 0; i < 20; i++)
            Assert.AreEqual(201, Send("POST", "/api/v1/protected/" + token + "/reply", null, "{\"message\":\"" + Escaped(Vector) + "\"}").status, "Reply " + (i + 1));
         (int status, string body) twentyFirst = Send("POST", "/api/v1/protected/" + token + "/reply", null, "{\"message\":\"" + Escaped(Vector) + "\"}");
         Assert.AreEqual(400, twentyFirst.status, twentyFirst.body);
         StringAssert.Contains("reply_ceiling", twentyFirst.body);

         // The budget is a hundred and twenty an hour per address, and loopback
         // is exempt from it as it is from the failed-logon count: this
         // fixture is a process on this machine, and so is every other.
         for (int i = 0; i < 125; i++)
         {
            int status = Send("GET", "/api/v1/protected/" + new string('B', 22), null, null).status;
            Assert.AreEqual(404, status, "Fetch " + (i + 1) + " from loopback is answered, not budgeted.");
         }
         Assert.AreEqual(200, Send("GET", "/api/v1/protected/" + token, null, null).status);
      }

      [Test]
      [Description("A record never finished goes with its file a day after it was made, and the directory of an account that no longer exists goes with the retention task - each decided by a query that answered.")]
      public void TheSweepRemovesWhatWasNeverFinishedAndWhatNobodyOwns()
      {
         (int status, string body) made = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + Vector.Length + "}");
         Assert.AreEqual(201, made.status, made.body);
         long id = long.Parse(Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value);
         string token = Regex.Match(made.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;
         Assert.AreEqual(200, Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, Vector.Substring(0, 100)).status);
         string stored = Path.Combine(_settings.Directories.DataDirectory, "Protected", _sender.ID.ToString(), token + ".asc");
         Assert.IsTrue(File.Exists(stored), "Half an upload is on disk: " + stored);

         // Younger than a day it stays (negative control); a day old it goes.
         _application.Utilities.RunMessageRetention();
         Assert.IsTrue(File.Exists(stored), "An upload still in progress is left alone.");
         StringAssert.Contains("\"token\":\"" + token + "\"", Send("GET", "/api/v1/me/protected", _me, null).body);
         _application.Database.ExecuteSQL("update hm_protectedmessages set pmcreated = pmcreated - 90000 where pmid = " + id);
         _application.Utilities.RunMessageRetention();
         Assert.IsFalse(File.Exists(stored), "A record never finished goes with its file after a day.");
         StringAssert.DoesNotContain("\"token\":\"" + token + "\"", Send("GET", "/api/v1/me/protected", _me, null).body, "And its row.");

         // An account deleted takes its rows with it; its directory goes with
         // the retention task, which is the one that asks the database.
         Account gone = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "protected-gone@" + _domain.Name, Password);
         string asGone = Basic(gone.Address, Password);
         (int status, string body) theirs = Send("POST", "/api/v1/me/protected", asGone,
            "{\"recipient\":\"" + _reader.Address + "\",\"size\":" + Vector.Length + "}");
         Assert.AreEqual(201, theirs.status, theirs.body);
         Assert.AreEqual(200, Put("/api/v1/me/protected/" + Regex.Match(theirs.body, "\"id\":(\\d+)").Groups[1].Value + "/content?offset=0", asGone, Vector).status);
         string directory = Path.Combine(_settings.Directories.DataDirectory, "Protected", gone.ID.ToString());
         Assert.IsTrue(Directory.Exists(directory), "The account's directory: " + directory);
         _application.Utilities.RunMessageRetention();
         Assert.IsTrue(Directory.Exists(directory), "An account that exists keeps its directory (negative control).");
         _domain.Accounts.DeleteByDBID(gone.ID);
         _application.Utilities.RunMessageRetention();
         Assert.IsFalse(Directory.Exists(directory), "The directory of an account that no longer exists is removed.");
      }

      [Test]
      [Description("A reply written on the reading page is filed under the original's sender with the original's token on it, the sender is told by a short internal message with a link the same password opens, and what is refused: a reply that is not ciphertext, a reply to a reply, a reply to a withdrawn message.")]
      public void AReplyIsFiledUnderTheSenderAndTheSenderIsTold()
      {
         string token = SendOne("the usual word");
         Pop3ClientSimulator.AssertGetFirstMessageText(_reader.Address, Password);

         Assert.AreEqual(400, Send("POST", "/api/v1/protected/" + token + "/reply", null, "{\"message\":\"Thank you, received.\"}").status, "A reply in the clear is refused.");
         Assert.AreEqual(404, Send("POST", "/api/v1/protected/" + new string('B', 22) + "/reply", null, "{\"message\":\"" + Escaped(Vector) + "\"}").status);

         (int status, string body) replied = Send("POST", "/api/v1/protected/" + token + "/reply", null, "{\"message\":\"" + Escaped(Vector) + "\"}");
         Assert.AreEqual(201, replied.status, replied.body);
         string replyToken = Regex.Match(replied.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;
         Assert.IsTrue(Regex.IsMatch(replyToken, "^[A-Za-z0-9_-]{22}$"), replied.body);
         StringAssert.Contains("\"notified\":true", replied.body);

         // Under the sender, marked as a reply to the original.
         (int status, string body) listed = Send("GET", "/api/v1/me/protected", _me, null);
         StringAssert.Contains("\"token\":\"" + replyToken + "\"", listed.body);
         StringAssert.Contains("\"reply_to\":\"" + token + "\"", listed.body);
         Assert.IsTrue(File.Exists(Path.Combine(_settings.Directories.DataDirectory, "Protected", _sender.ID.ToString(), replyToken + ".asc")), "The reply's ciphertext is the sender's.");

         // The sender is told, by an internal message with the link.
         string note = Pop3ClientSimulator.AssertGetFirstMessageText(_sender.Address, Password).Replace("=\r\n", "");
         StringAssert.Contains("Subject: A reply to your password-protected message", note);
         StringAssert.Contains("/portal/protected/" + replyToken, note);
         StringAssert.Contains("X-hMailServer-Protected: " + replyToken, note);
         StringAssert.DoesNotContain("BEGIN PGP MESSAGE", note);

         // The reply is fetched like any other, says it is a reply, and takes no reply itself.
         (int status, string body) fetched = Send("GET", "/api/v1/protected/" + replyToken, null, null);
         Assert.AreEqual(200, fetched.status, fetched.body);
         StringAssert.Contains("\"reply\":true", fetched.body);
         StringAssert.Contains("\"can_reply\":false", fetched.body);
         Assert.AreEqual(400, Send("POST", "/api/v1/protected/" + replyToken + "/reply", null, "{\"message\":\"" + Escaped(Vector) + "\"}").status, "A reply to a reply.");

         // Withdrawn: no more replies.
         long id = long.Parse(Regex.Match(listed.body, "\"id\":(\\d+),\"token\":\"" + token + "\"").Groups[1].Value);
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/protected/" + id, _me, null).status);
         Assert.AreEqual(410, Send("POST", "/api/v1/protected/" + token + "/reply", null, "{\"message\":\"" + Escaped(Vector) + "\"}").status);
      }

      [Test]
      [Description("A message whose expiry has passed is refused on the page, listed as expired, and the sweep deletes its ciphertext; and with OpenPGP switched off in the webmail, or no public address for the links, nothing is offered.")]
      public void AnExpiryEndsItAndTheSwitchesRefuseIt()
      {
         DateTime sentAt = DateTime.UtcNow;
         string token = SendOne("", 3);
         string stored = Path.Combine(_settings.Directories.DataDirectory, "Protected", _sender.ID.ToString(), token + ".asc");
         Assert.IsTrue(File.Exists(stored), "Kept until it expires: " + stored);
         int remaining = 3500 - (int) (DateTime.UtcNow - sentAt).TotalMilliseconds;
         if (remaining > 0)
            Thread.Sleep(remaining);
         (int status, string body) expired = Send("GET", "/api/v1/protected/" + token, null, null);
         Assert.AreEqual(410, expired.status, expired.body);
         StringAssert.Contains("expired", expired.body);
         Assert.IsTrue(Regex.IsMatch(Send("GET", "/api/v1/me/protected", _me, null).body, "\"expires\":\\d+,\"expired\":true"), "Listed as expired.");
         _application.Utilities.RunMessageRetention();
         Assert.IsFalse(File.Exists(stored), "The sweep deletes the ciphertext.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":false}").status);
         (int status, string body) off = Send("POST", "/api/v1/me/protected", _me, "{\"recipient\":\"" + _reader.Address + "\",\"size\":10}");
         Assert.AreEqual(403, off.status, off.body);
         StringAssert.Contains("openpgp_unavailable", off.body);
         StringAssert.Contains("\"available\":false", Send("GET", "/api/v1/me/protected", _me, null).body);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true}").status);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\",\"quarantine_digest_base_url\":\"\",\"import_oauth_redirect_base_url\":\"\"}").status);
         (int status, string body) noAddress = Send("POST", "/api/v1/me/protected", _me, "{\"recipient\":\"" + _reader.Address + "\",\"size\":10}");
         Assert.AreEqual(403, noAddress.status, noAddress.body);
         StringAssert.Contains("protected_unavailable", noAddress.body);
      }

      [Test]
      [Description("The reading page and its script are served to anyone, with the webmail's own headers; the page loads the two modules and its own script and nothing else.")]
      public void TheReadingPageIsServedToAnyone()
      {
         (int status, string body) page = Send("GET", "/portal/protected/" + new string('C', 22), null, null);
         Assert.AreEqual(200, page.status, page.body.Substring(0, Math.Min(200, page.body.Length)));
         StringAssert.Contains("<script src=\"/portal-smime.js\"></script>", page.body);
         StringAssert.Contains("<script src=\"/portal-openpgp.js\"></script>", page.body);
         StringAssert.Contains("<script src=\"/portal-protected.js\"></script>", page.body);
         StringAssert.Contains("does not pretend it could stop you copying", page.body);
         Assert.IsFalse(Regex.IsMatch(page.body, "<script[^>]*>\\s*[^<\\s]"), "No inline script: the policy forbids it.");

         (int status, string body) script = Send("GET", "/portal-protected.js", null, null);
         Assert.AreEqual(200, script.status);
         StringAssert.Contains("openProtected", script.body);
         StringAssert.Contains("That password is not right.", script.body);
      }

      // ---- helpers ---------------------------------------------------------

      /// <summary>Records a message, uploads the vector in one chunk and answers its token.</summary>
      private string SendOne(string hint, int expiresIn = 0)
      {
         string extra = expiresIn > 0 ? ",\"expires_in\":" + expiresIn : "";
         (int status, string body) made = Send("POST", "/api/v1/me/protected", _me,
            "{\"recipient\":\"" + _reader.Address + "\",\"hint\":\"" + hint + "\",\"size\":" + Vector.Length + extra + "}");
         Assert.AreEqual(201, made.status, made.body);
         long id = long.Parse(Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) done = Put("/api/v1/me/protected/" + id + "/content?offset=0", _me, Vector);
         Assert.AreEqual(200, done.status, done.body);
         StringAssert.Contains("\"complete\":true", done.body);
         return Regex.Match(made.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;
      }

      /// <summary>The packets under the vector's armour, its checksum line left out.</summary>
      private static byte[] PacketsOf(string armoured)
      {
         var data = new StringBuilder();
         foreach (string line in armoured.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.StartsWith("-----") || line.StartsWith("=") || line.Length == 0)
               continue;
            data.Append(line);
         }
         return Convert.FromBase64String(data.ToString());
      }

      /// <summary>Packets as a PGP MESSAGE, with no checksum line, which the server accepts.</summary>
      private static string Armoured(byte[] packets)
      {
         string base64 = Convert.ToBase64String(packets);
         var lines = new StringBuilder();
         for (int at = 0; at < base64.Length; at += 64)
            lines.Append(base64.Substring(at, Math.Min(64, base64.Length - at))).Append("\r\n");
         return "-----BEGIN PGP MESSAGE-----\r\n\r\n" + lines + "-----END PGP MESSAGE-----\r\n";
      }

      private static byte[] Concat(params byte[][] parts)
      {
         int length = 0;
         foreach (byte[] part in parts)
            length += part.Length;
         byte[] joined = new byte[length];
         int at = 0;
         foreach (byte[] part in parts)
         {
            Array.Copy(part, 0, joined, at, part.Length);
            at += part.Length;
         }
         return joined;
      }

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

      /// <summary>
      ///    A chunk of the ciphertext, as the page PUTs one: the bytes as the body,
      ///    which the content route takes raw whatever the content type says.
      /// </summary>
      private static (int status, string body) Put(string path, string authorization, string bytes)
      {
         return RestRequest.Send("127.0.0.1", RestPort, "PUT", path, authorization, bytes);
      }
   }
}
