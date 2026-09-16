// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Reflection;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Sieve
{
   /// <summary>
   ///    The "body" test (RFC 5173) against a message that is signed or encrypted
   ///    end to end, and why this fixture asserts that NOTHING about the evaluation
   ///    changed.
   ///
   ///    The rule engine's body criterion now declines on a sealed message, because
   ///    that engine is ours and its semantics are ours to choose. Sieve is not
   ///    ours. RFC 5173 defines ":text" and ":content" over the decoded MIME parts,
   ///    a sealed message genuinely has no text part, and an empty set of values
   ///    really is the correct evaluation - a third truth value would make this
   ///    server's Sieve disagree with every other implementation, and a script that
   ///    behaves differently here than on Dovecot is worse than one that behaves
   ///    surprisingly on both.
   ///
   ///    So the tests below pin the RFC behaviour: "if body :contains" does not
   ///    match, "if not body :contains" does, and "body :raw" - which sees the
   ///    armour exactly as it arrived - is the one honest way a script can
   ///    recognise a sealed message. What is new is a single line in the log saying
   ///    that this is what happened, so that a script whose author cannot work out
   ///    why every encrypted message is filed the wrong way has somewhere to look.
   ///
   ///    See hmailserver/docs/SealedMail.md.
   /// </summary>
   [TestFixture]
   public class SieveSealedBody : TestFixtureBase
   {
      private const string Password = "secret";
      private const string TargetFolder = "Matched";

      private const string CiphertextMarker = "c2VhbGVkLXNpZXZlLWNpcGhlcnRleHQ=";

      private static void SetScript(Account account, string script)
      {
         // Late-bound, for the same reason the sibling Sieve delivery fixtures are:
         // so the fixture does not depend on the registered type library having been
         // regenerated after an IDL change.
         account.GetType().InvokeMember(
            "SieveScript", BindingFlags.SetProperty, null, account, new object[] { script });
      }

      /// <summary>
      ///    Delivers one raw message under the given script and answers where it
      ///    landed: true for the "Matched" folder the script files into, false for
      ///    INBOX.
      /// </summary>
      private bool FilesIntoMatchedFolder(string script, string rawMessage)
      {
         Account recipient = SingletonProvider<TestSetup>.Instance.AddAccount(
            _domain, "sieve-sealed@example.test", Password);

         recipient.IMAPFolders.Add(TargetFolder);

         SetScript(recipient, script);

         var smtp = new SmtpClientSimulator();
         smtp.SendRaw("correspondent@example.com", recipient.Address, rawMessage);

         IMAPFolder inbox = recipient.IMAPFolders.get_ItemByName("INBOX");
         IMAPFolder matched = recipient.IMAPFolders.get_ItemByName(TargetFolder);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         int inInbox = inbox.Messages.Count;
         int inMatched = matched.Messages.Count;

         Assert.AreEqual(1, inInbox + inMatched,
            "Exactly one copy of the message should exist. INBOX has " + inInbox + " and " +
            TargetFolder + " has " + inMatched + ".");

         return inMatched == 1;
      }

      private static string PgpMimeMessage(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Sealed sieve\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";\r\n" +
                " boundary=\"sealed-boundary\"\r\n" +
                "\r\n" +
                "--sealed-boundary\r\n" +
                "Content-Type: application/pgp-encrypted\r\n" +
                "\r\n" +
                "Version: 1\r\n" +
                "\r\n" +
                "--sealed-boundary\r\n" +
                "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n" +
                "Content-Disposition: inline; filename=\"encrypted.asc\"\r\n" +
                "\r\n" +
                "-----BEGIN PGP MESSAGE-----\r\n" +
                "\r\n" +
                CiphertextMarker + "\r\n" +
                "=abcd\r\n" +
                "-----END PGP MESSAGE-----\r\n" +
                "\r\n" +
                "--sealed-boundary--\r\n";
      }

      private static string PlainMessage(string recipient, string body)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Sealed sieve\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                body + "\r\n";
      }

      private const string FileIntoIfBodyContains =
         "require [\"body\", \"fileinto\"];\r\n" +
         "if body :contains \"invoice\" {\r\n" +
         "  fileinto \"" + TargetFolder + "\";\r\n" +
         "}\r\n";

      private const string FileIntoIfBodyDoesNotContain =
         "require [\"body\", \"fileinto\"];\r\n" +
         "if not body :contains \"invoice\" {\r\n" +
         "  fileinto \"" + TargetFolder + "\";\r\n" +
         "}\r\n";

      private const string FileIntoIfRawBodyContains =
         "require [\"body\", \"fileinto\"];\r\n" +
         "if body :raw :contains \"BEGIN PGP MESSAGE\" {\r\n" +
         "  fileinto \"" + TargetFolder + "\";\r\n" +
         "}\r\n";

      [Test]
      [Description("RFC 5173 unchanged: body :contains does not match a sealed message, because it has no decodable text part.")]
      public void ABodyContainsTestDoesNotMatchASealedMessage()
      {
         Assert.IsFalse(
            FilesIntoMatchedFolder(FileIntoIfBodyContains, PgpMimeMessage("sieve-sealed@example.test")),
            "A sealed message has no text part, so body :contains must be false for every key.");
      }

      [Test]
      [Description("RFC 5173 unchanged: the negation therefore matches, which is correct and is what the log line exists to explain.")]
      public void TheNegationOfABodyTestMatchesASealedMessage()
      {
         Assert.IsTrue(
            FilesIntoMatchedFolder(FileIntoIfBodyDoesNotContain, PgpMimeMessage("sieve-sealed@example.test")),
            "\"if not body :contains\" is true when the body test is false, and that is the RFC's arithmetic, not a defect.");

         BounceTestSupport.AssertLogContains("tested the body of a message that is encrypted end to end",
            "A script author whose filter behaves differently on encrypted mail needs one line saying why.");
      }

      [Test]
      [Description("body :raw sees the armour, which is how a script can recognise a sealed message on purpose.")]
      public void ARawBodyTestSeesTheArmourOfASealedMessage()
      {
         Assert.IsTrue(
            FilesIntoMatchedFolder(FileIntoIfRawBodyContains, PgpMimeMessage("sieve-sealed@example.test")),
            "body :raw is the undecoded body and must still see the PGP armour.");
      }

      [Test]
      [Description("The negative control: an ordinary message is not reported as sealed, and :raw is not logged either.")]
      public void AnOrdinaryMessageIsNotReportedAsSealed()
      {
         Assert.IsTrue(
            FilesIntoMatchedFolder(FileIntoIfBodyContains, PlainMessage("sieve-sealed@example.test", "Your invoice is attached.")),
            "An ordinary body test stopped working.");

         StringAssert.DoesNotContain("tested the body of a message that is encrypted end to end",
            LogHandler.ReadCurrentDefaultLog(),
            "An ordinary message was reported as sealed, which would make the line meaningless.");
      }

      private const string FileIntoIfCiphertextPartContains =
         "require [\"body\", \"fileinto\"];\r\n" +
         "if body :content \"application/octet-stream\" :contains \"" + CiphertextMarker + "\" {\r\n" +
         "  fileinto \"" + TargetFolder + "\";\r\n" +
         "}\r\n";

      [Test]
      [Description("A :content test that names the ciphertext part compares against it for real, so it matches and is not reported as having compared against nothing.")]
      public void AContentTestOnTheCiphertextPartIsARealComparison()
      {
         Assert.IsTrue(
            FilesIntoMatchedFolder(FileIntoIfCiphertextPartContains, PgpMimeMessage("sieve-sealed@example.test")),
            "body :content \"application/octet-stream\" is handed the ciphertext part, which contains the marker.");

         StringAssert.DoesNotContain("tested the body of a message that is encrypted end to end",
            LogHandler.ReadCurrentDefaultLog(),
            "The line says the test compared against nothing; this one compared against the ciphertext part, so it must not be written.");
      }
   }
}
