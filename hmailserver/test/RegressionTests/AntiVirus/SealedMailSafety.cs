// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiVirus
{
   /// <summary>
   ///    What the anti-virus half of the server does to a message it cannot read.
   ///
   ///    A "sealed" message is one signed or encrypted end to end by the two people
   ///    writing to each other - S/MIME or PGP/MIME - so that this server is a
   ///    courier for bytes it holds no key to. Three things used to happen to such a
   ///    message, all of them silent:
   ///
   ///    - The blocked-attachment list was applied to it. The ciphertext of a
   ///      PGP/MIME message is a part named encrypted.asc and the signature of an
   ///      S/MIME one is smime.p7s, and the MIME layer calls both attachments, so an
   ///      administrator blocking *.asc or *.p7s had the content of every sealed
   ///      message replaced with a notice, the file rewritten, and no log line
   ///      anywhere. The plaintext existed nowhere else.
   ///    - The virus scanner scanned the ciphertext, found nothing, and the message
   ///      was recorded as clean - which is not the same statement as "nobody could
   ///      look", and the two were written down identically.
   ///    - The "delete attachments" action kept one part of it and threw the rest
   ///      away, which on a sealed message means keeping a fragment of base64 under
   ///      a header rebuilt without its Content-Type.
   ///
   ///    Every test here has its negative control in the same fixture, because a fix
   ///    that simply switched attachment blocking or stripping off would otherwise
   ///    pass all of them. See hmailserver/docs/SealedMail.md.
   ///
   ///    The scanners are two cmd.exe invocations with a fixed exit code rather than
   ///    clamd: the tests are about what the server does with a verdict, not about
   ///    reaching a verdict, and a fixture that needs a daemon running is a fixture
   ///    that does not run.
   /// </summary>
   [TestFixture]
   public class SealedMailSafety : TestFixtureBase
   {
      private const string Password = "secret";

      // "%FILE%" is what CustomVirusScanner substitutes the path into; without it
      // the whole string is quoted as an executable path and nothing launches. The
      // "&rem" swallows the file name so that cmd's own exit code is the verdict.
      private const string AlwaysCleanScanner = @"C:\Windows\System32\cmd.exe /c exit 0&rem %FILE%";
      private const string AlwaysInfectedScanner = @"C:\Windows\System32\cmd.exe /c exit 3&rem %FILE%";
      private const int InfectedReturnValue = 3;

      private const string CiphertextMarker = "c2VhbGVkLWNpcGhlcnRleHQtbWFya2Vy";
      private const string SignaturePartMarker = "c2lnbmF0dXJlLWJ5dGVzLW1hcmtlcg";
      private const string BinaryPartMarker = "YmluYXJ5LXBheWxvYWQtbWFya2Vy";
      private const string PlainTextMarker = "This note must survive the stripper.";

      private Account _account;

      private bool _originalCustomEnabled;
      private string _originalCustomExecutable;
      private int _originalCustomReturnValue;
      private eAntivirusAction _originalAction;
      private bool _originalNotifySender;
      private bool _originalNotifyReceiver;
      private bool _originalBlocking;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sealed@example.test", Password);

         var antiVirus = _settings.AntiVirus;

         _originalCustomEnabled = antiVirus.CustomScannerEnabled;
         _originalCustomExecutable = antiVirus.CustomScannerExecutable;
         _originalCustomReturnValue = antiVirus.CustomScannerReturnValue;
         _originalAction = antiVirus.Action;
         _originalNotifySender = antiVirus.NotifySender;
         _originalNotifyReceiver = antiVirus.NotifyReceiver;
         _originalBlocking = antiVirus.EnableAttachmentBlocking;

         // A notification is another message in another mailbox, and every count in
         // this fixture is of one mailbox.
         antiVirus.NotifySender = false;
         antiVirus.NotifyReceiver = false;
      }

      [TearDown]
      public new void TearDown()
      {
         var antiVirus = _settings.AntiVirus;

         antiVirus.CustomScannerEnabled = _originalCustomEnabled;
         antiVirus.CustomScannerExecutable = _originalCustomExecutable;
         antiVirus.CustomScannerReturnValue = _originalCustomReturnValue;
         antiVirus.Action = _originalAction;
         antiVirus.NotifySender = _originalNotifySender;
         antiVirus.NotifyReceiver = _originalNotifyReceiver;
         antiVirus.EnableAttachmentBlocking = _originalBlocking;

         RemoveBlockedWildcards();
      }

      /// <summary>
      ///    Blocks one file-name pattern, and remembers to take it away again: the
      ///    blocked-attachment list is server-wide and PerformBasicSetup does not
      ///    clear it, so a wildcard left behind here is a wildcard every later
      ///    fixture runs with.
      /// </summary>
      private void BlockWildcard(string wildcard)
      {
         var blocked = _settings.AntiVirus.BlockedAttachments.Add();
         blocked.Wildcard = wildcard;
         blocked.Description = "SealedMailSafety";
         blocked.Save();

         _settings.AntiVirus.EnableAttachmentBlocking = true;
      }

      private void RemoveBlockedWildcards()
      {
         var blockedAttachments = _settings.AntiVirus.BlockedAttachments;

         for (var i = blockedAttachments.Count - 1; i >= 0; i--)
         {
            var item = blockedAttachments[i];

            if (item.Description == "SealedMailSafety")
               blockedAttachments.DeleteByDBID(item.ID);
         }
      }

      private void EnableScanner(string executable, int virusReturnValue, eAntivirusAction action)
      {
         var antiVirus = _settings.AntiVirus;

         antiVirus.CustomScannerExecutable = executable;
         antiVirus.CustomScannerReturnValue = virusReturnValue;
         antiVirus.CustomScannerEnabled = true;
         antiVirus.Action = action;
      }

      /// <summary>
      ///    A PGP/MIME message (RFC 3156): a version marker and the ciphertext,
      ///    under a multipart/encrypted top level. The ciphertext part is named
      ///    encrypted.asc, which is what real clients call it and what an
      ///    administrator blocking "*.asc" would hit.
      /// </summary>
      private static string PgpMimeMessage(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Sealed\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";\r\n" +
                " boundary=\"sealed-boundary\"\r\n" +
                "\r\n" +
                "--sealed-boundary\r\n" +
                "Content-Type: application/pgp-encrypted\r\n" +
                "Content-Description: PGP/MIME version identification\r\n" +
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

      /// <summary>
      ///    An S/MIME detached signature (RFC 8551): the text as it was written,
      ///    plus smime.p7s. Blocking "*.p7s" used to replace the signature with a
      ///    notice, which makes every message the organisation signs arrive as
      ///    tampered with.
      /// </summary>
      private static string SMimeSignedMessage(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Signed\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\";\r\n" +
                " micalg=sha-256; boundary=\"signed-boundary\"\r\n" +
                "\r\n" +
                "--signed-boundary\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                PlainTextMarker + "\r\n" +
                "\r\n" +
                "--signed-boundary\r\n" +
                "Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"smime.p7s\"\r\n" +
                "\r\n" +
                SignaturePartMarker + "\r\n" +
                "\r\n" +
                "--signed-boundary--\r\n";
      }

      /// <summary>
      ///    The negative control's shape: an ordinary multipart/mixed carrying a
      ///    file with exactly the same name as the sealed message's ciphertext part.
      ///    Nothing about this one is sealed, so the block must still happen.
      /// </summary>
      private static string OrdinaryMessageWithAscAttachment(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Ordinary\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/mixed; boundary=\"mixed-boundary\"\r\n" +
                "\r\n" +
                "--mixed-boundary\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                PlainTextMarker + "\r\n" +
                "\r\n" +
                "--mixed-boundary\r\n" +
                "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n" +
                "Content-Disposition: inline; filename=\"encrypted.asc\"\r\n" +
                "\r\n" +
                CiphertextMarker + "\r\n" +
                "\r\n" +
                "--mixed-boundary--\r\n";
      }

      /// <summary>
      ///    A multipart/mixed whose FIRST part is the attachment and whose second is
      ///    the text. The order is the whole point: the stripper walked the parts
      ///    and kept the first one IsGoodTextPart_ accepted, and that function
      ///    accepted every part, so this message was "stripped" to its binary
      ///    attachment and its text was thrown away.
      /// </summary>
      private static string AttachmentFirstMessage(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Attachment first\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/mixed; boundary=\"order-boundary\"\r\n" +
                "\r\n" +
                "--order-boundary\r\n" +
                "Content-Type: application/octet-stream; name=\"payload.bin\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"payload.bin\"\r\n" +
                "\r\n" +
                BinaryPartMarker + "\r\n" +
                "\r\n" +
                "--order-boundary\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                PlainTextMarker + "\r\n" +
                "\r\n" +
                "--order-boundary--\r\n";
      }

      /// <summary>
      ///    Two attachments and no text part at all. Falling out of the stripper's
      ///    walk with nothing to keep became reachable the moment IsGoodTextPart_
      ///    started telling the truth, and returning there would deliver the
      ///    infected message whole.
      /// </summary>
      private static string NoTextPartMessage(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: No text\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/mixed; boundary=\"notext-boundary\"\r\n" +
                "\r\n" +
                "--notext-boundary\r\n" +
                "Content-Type: application/octet-stream; name=\"payload.bin\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"payload.bin\"\r\n" +
                "\r\n" +
                BinaryPartMarker + "\r\n" +
                "\r\n" +
                "--notext-boundary\r\n" +
                "Content-Type: application/octet-stream; name=\"second.bin\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"second.bin\"\r\n" +
                "\r\n" +
                BinaryPartMarker + "\r\n" +
                "\r\n" +
                "--notext-boundary--\r\n";
      }

      [Test]
      [Description("A PGP/MIME message is not rewritten by the blocked-attachment list, and the log says why.")]
      public void ThePgpCiphertextSurvivesTheBlockedAttachmentList()
      {
         BlockWildcard("*.asc");

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            PgpMimeMessage(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.Contains(CiphertextMarker, delivered,
            "The ciphertext of the sealed message was replaced, which destroys it beyond recovery.");
         StringAssert.Contains("filename=\"encrypted.asc\"", delivered,
            "The ciphertext part was renamed, so a client will no longer treat it as the encrypted body.");
         StringAssert.DoesNotContain("encrypted.asc.txt", delivered,
            "The ciphertext part was renamed, so a client will no longer treat it as the encrypted body.");
         StringAssert.DoesNotContain("was blocked for delivery", delivered,
            "The blocked-attachment notice was written into a sealed message.");

         BounceTestSupport.AssertLogContains("was NOT checked against the blocked-attachment list",
            "Leaving a sealed message alone has to be visible to the administrator whose list is not being applied.");
      }

      [Test]
      [Description("An S/MIME signature part is not rewritten by the blocked-attachment list.")]
      public void TheSMimeSignatureSurvivesTheBlockedAttachmentList()
      {
         BlockWildcard("*.p7s");

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            SMimeSignedMessage(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.Contains(SignaturePartMarker, delivered,
            "The detached signature was replaced, so the message now arrives as tampered with.");
         StringAssert.DoesNotContain("smime.p7s.txt", delivered,
            "The detached signature was renamed, so no client will find it.");
      }

      [Test]
      [Description("The negative control: an ordinary message carrying the same file name is still blocked, and the rewrite is now in the log.")]
      public void AnOrdinaryMessageWithTheSameFileNameIsStillBlocked()
      {
         BlockWildcard("*.asc");

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            OrdinaryMessageWithAscAttachment(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.Contains("was blocked for delivery", delivered,
            "Attachment blocking stopped working on ordinary messages, which is not a fix.");
         StringAssert.Contains("encrypted.asc.txt", delivered,
            "The blocked attachment was not renamed.");
         StringAssert.DoesNotContain(CiphertextMarker, delivered,
            "The blocked attachment's content was not replaced.");

         BounceTestSupport.AssertLogContains("matched the blocked-attachment list",
            "Rewriting somebody's mail has to leave a record naming the message and the file.");
      }

      [Test]
      [Description("A clean scan of a sealed message is recorded as a scan that could not see the content.")]
      public void ACleanVerdictOnASealedMessageIsRecordedAsUnexaminable()
      {
         EnableScanner(AlwaysCleanScanner, InfectedReturnValue, eAntivirusAction.hDeleteEmail);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            PgpMimeMessage(_account.Address));

         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 1);

         BounceTestSupport.AssertLogContains("COULD NOT BE EXAMINED",
            "A scanner that could only see ciphertext must not leave the same trace as one that examined the message.");
      }

      [Test]
      [Description("The negative control: an ordinary message that scans clean is not recorded as unexaminable.")]
      public void ACleanVerdictOnAnOrdinaryMessageSaysNothingAboutSealing()
      {
         EnableScanner(AlwaysCleanScanner, InfectedReturnValue, eAntivirusAction.hDeleteEmail);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            OrdinaryMessageWithAscAttachment(_account.Address));

         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 1);

         StringAssert.DoesNotContain("COULD NOT BE EXAMINED", LogHandler.ReadCurrentDefaultLog(),
            "An ordinary message was reported as unexaminable, which would make the line meaningless.");
      }

      [Test]
      [Description("The attachment stripper leaves a sealed message exactly as it arrived, and says so.")]
      public void TheStripperDeclinesASealedMessage()
      {
         EnableScanner(AlwaysInfectedScanner, InfectedReturnValue, eAntivirusAction.hDeleteAttachments);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            PgpMimeMessage(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.Contains(CiphertextMarker, delivered,
            "The stripper kept one part of a sealed message, which leaves the reader a fragment of ciphertext.");
         StringAssert.Contains("multipart/encrypted", delivered,
            "The stripper rebuilt the message without its top-level Content-Type.");

         BounceTestSupport.AssertLogContains("attachments have NOT been stripped",
            "A message that still carries what the scanner objected to must be reported, not quietly delivered.");
      }

      [Test]
      [Description("The stripper keeps the text part of an ordinary message even when the attachment comes first.")]
      public void TheStripperKeepsTheTextPartAndNotTheFirstPart()
      {
         EnableScanner(AlwaysInfectedScanner, InfectedReturnValue, eAntivirusAction.hDeleteAttachments);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            AttachmentFirstMessage(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.Contains(PlainTextMarker, delivered,
            "The text of the message was thrown away and an attachment was kept in its place.");
         StringAssert.DoesNotContain(BinaryPartMarker, delivered,
            "The attachment the scanner objected to survived the strip.");
      }

      [Test]
      [Description("A message with no text part at all is stripped to nothing rather than delivered whole.")]
      public void TheStripperLeavesNothingWhenThereIsNoTextPart()
      {
         EnableScanner(AlwaysInfectedScanner, InfectedReturnValue, eAntivirusAction.hDeleteAttachments);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            NoTextPartMessage(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.DoesNotContain(BinaryPartMarker, delivered,
            "A message with no text part was delivered with its attachments intact.");
      }
   }
}
