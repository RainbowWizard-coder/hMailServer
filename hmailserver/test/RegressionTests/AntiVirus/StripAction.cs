// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiVirus
{
   /// <summary>
   ///    What the anti-virus action "Remove all attachments and deliver the text"
   ///    (eAntivirusAction.hDeleteAttachments) does with a message that is not
   ///    text with files attached: a message that is one attachment and nothing
   ///    else, a message that is one text part, and a message the stripper cannot
   ///    read.
   ///
   ///    - One attachment and nothing else is an attachment like any other, and the
   ///      notice takes its place: what is delivered is the header, its subject
   ///      marked, and the server's VIRUS_ATTACHMENT_REMOVED text, without the
   ///      part's own Content- fields, so that the notice reads as text.
   ///    - An S/MIME message signed as one part (signed-data, the opaque form) is
   ///      such a part too, and is replaced by the notice; one encrypted as one
   ///      part (enveloped-data) is delivered as it arrived, as every encrypted
   ///      message is.
   ///    - One text part is the message's text, and is delivered as it is, as the
   ///      text part of a multipart message is kept.
   ///    - A message that cannot be read cannot be stripped, so nothing of it can
   ///      be kept: it is deleted, as the delete action deletes it, and each of
   ///      its recipients is sent the "message deleted" notice in place of the
   ///      note, whatever the two notice settings say; its sender is not told.
   ///
   ///    And the "message deleted" notice reaches the recipients of a message
   ///    whose envelope sender is empty, as it reaches any other recipient.
   ///
   ///    The scanners are command lines with a fixed exit code, as in
   ///    SealedMailSafety: these tests are about what the server does with a
   ///    verdict, not about reaching one. What is scanned carries the EICAR test
   ///    file, put together at run time as the ClamAV fixture puts it together, so
   ///    that what reaches the mailbox can be looked for in it. See
   ///    hmailserver/docs/SealedMail.md, "The "delete attachments" anti-virus
   ///    action".
   /// </summary>
   [TestFixture]
   public class StripAction : TestFixtureBase
   {
      private const string Password = "secret";

      // As in SealedMailSafety: "%FILE%" is where CustomVirusScanner puts the
      // path, and "&rem" swallows it, so that cmd's own exit code is the verdict.
      private const string AlwaysInfectedScanner = @"C:\Windows\System32\cmd.exe /c exit 3&rem %FILE%";
      private const int InfectedReturnValue = 3;

      private const string PlainTextMarker = "This text is the whole of the message.";

      // How long the holder below keeps the message file to itself, and how long
      // the OnDeliverMessage script then waits. The stripper reads the file a
      // fraction of a second into the hold, and the wait is over seconds after
      // the hold is: a slow machine has room on both sides.
      private const int HoldMilliseconds = 2500;
      private const int DeliverMessageWaitMilliseconds = 7000;

      private Account _account;

      private bool _originalCustomEnabled;
      private string _originalCustomExecutable;
      private int _originalCustomReturnValue;
      private eAntivirusAction _originalAction;
      private bool _originalNotifySender;
      private bool _originalNotifyReceiver;

      private string _scriptFile;
      private string _originalScript;
      private string _holdScript;
      private string _holdSignal;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "strip@example.test", Password);

         var antiVirus = _settings.AntiVirus;

         _originalCustomEnabled = antiVirus.CustomScannerEnabled;
         _originalCustomExecutable = antiVirus.CustomScannerExecutable;
         _originalCustomReturnValue = antiVirus.CustomScannerReturnValue;
         _originalAction = antiVirus.Action;
         _originalNotifySender = antiVirus.NotifySender;
         _originalNotifyReceiver = antiVirus.NotifyReceiver;

         // Off, as the shipped defaults have them: what a test sees in the
         // mailbox is then what the action itself delivers.
         antiVirus.NotifySender = false;
         antiVirus.NotifyReceiver = false;

         _scriptFile = null;
         _originalScript = null;
         _holdScript = null;
         _holdSignal = null;
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

         // PerformBasicSetup switches scripting off and the language back to
         // VBScript; the script file itself is put back here.
         if (_scriptFile != null)
         {
            _settings.Scripting.Enabled = false;

            if (_originalScript != null)
               File.WriteAllText(_scriptFile, _originalScript);
            else if (File.Exists(_scriptFile))
               File.Delete(_scriptFile);
         }

         if (_holdScript != null && File.Exists(_holdScript))
            File.Delete(_holdScript);

         if (_holdSignal != null && File.Exists(_holdSignal))
            File.Delete(_holdSignal);
      }

      private void EnableScanner(string executable, eAntivirusAction action)
      {
         var antiVirus = _settings.AntiVirus;

         antiVirus.CustomScannerExecutable = executable;
         antiVirus.CustomScannerReturnValue = InfectedReturnValue;
         antiVirus.CustomScannerEnabled = true;
         antiVirus.Action = action;
      }

      /// <summary>
      ///    The EICAR test file, base64-encoded as a mail client attaches it. Put
      ///    together from two fragments at run time, as in the ClamAV fixture, so
      ///    that neither it nor its base64 form is a literal in this source file:
      ///    on-access virus scanners quarantine files that hold either.
      /// </summary>
      private static string EicarBase64()
      {
         var firstPart = @"X5O!P%@AP[4\PZX54(P^)7CC)7}";
         var secondPart = @"$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";

         return Convert.ToBase64String(Encoding.ASCII.GetBytes(firstPart + secondPart));
      }

      private string ServerMessage(string name)
      {
         return _settings.ServerMessages.get_ItemByName(name).Text;
      }

      private static string HeaderOf(string message)
      {
         var end = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);

         return end < 0 ? message : message.Substring(0, end + 2);
      }

      /// <summary>
      ///    An ordinary multipart message: a line of text and EICAR attached.
      /// </summary>
      private static string TextAndEicarMessage(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Text and a file\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/mixed; boundary=\"strip-boundary\"\r\n" +
                "\r\n" +
                "--strip-boundary\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                PlainTextMarker + "\r\n" +
                "\r\n" +
                "--strip-boundary\r\n" +
                "Content-Type: application/octet-stream; name=\"eicar.com\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"eicar.com\"\r\n" +
                "\r\n" +
                EicarBase64() + "\r\n" +
                "\r\n" +
                "--strip-boundary--\r\n";
      }

      /// <summary>
      ///    A message of one part, its header the part's: the Content- fields the
      ///    case names, then EICAR - base64-encoded, or inside a message that is
      ///    itself the one part.
      /// </summary>
      private static string OneAttachmentMessage(string recipient, string shape)
      {
         string contentFields;
         string body;

         switch (shape)
         {
            case "attachment":
               contentFields =
                  "Content-Type: application/octet-stream; name=\"eicar.com\"\r\n" +
                  "Content-Transfer-Encoding: base64\r\n" +
                  "Content-Disposition: attachment; filename=\"eicar.com\"\r\n" +
                  "Content-Description: EICAR\r\n";
               body = EicarBase64() + "\r\n";
               break;

            case "program without a disposition":
               contentFields =
                  "Content-Type: application/x-msdownload; name=\"setup.exe\"\r\n" +
                  "Content-Transfer-Encoding: base64\r\n";
               body = EicarBase64() + "\r\n";
               break;

            case "text declared an attachment":
               contentFields =
                  "Content-Type: text/html; charset=us-ascii; name=\"invoice.html\"\r\n" +
                  "Content-Transfer-Encoding: base64\r\n" +
                  "Content-Disposition: attachment; filename=\"invoice.html\"\r\n";
               body = EicarBase64() + "\r\n";
               break;

            case "message":
               contentFields =
                  "Content-Type: message/rfc822\r\n" +
                  "Content-Disposition: inline\r\n";
               body =
                  "From: someone@example.com\r\n" +
                  "To: " + recipient + "\r\n" +
                  "Subject: Inside\r\n" +
                  "MIME-Version: 1.0\r\n" +
                  "Content-Type: application/octet-stream; name=\"eicar.com\"\r\n" +
                  "Content-Transfer-Encoding: base64\r\n" +
                  "Content-Disposition: attachment; filename=\"eicar.com\"\r\n" +
                  "\r\n" +
                  EicarBase64() + "\r\n";
               break;

            default:
               throw new ArgumentException("No such shape: " + shape);
         }

         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: One attachment\r\n" +
                "MIME-Version: 1.0\r\n" +
                contentFields +
                "\r\n" +
                body;
      }

      [TestCase("attachment")]
      [TestCase("program without a disposition")]
      [TestCase("text declared an attachment")]
      [TestCase("message")]
      [Description("A message that is one attachment and nothing else is replaced by the notice, whose header holds none of the part's Content- fields.")]
      public void AMessageThatIsOneAttachmentIsReplacedByTheNotice(string shape)
      {
         EnableScanner(AlwaysInfectedScanner, eAntivirusAction.hDeleteAttachments);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            OneAttachmentMessage(_account.Address, shape));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.DoesNotContain(EicarBase64(), delivered,
            "The attachment the scanner objected to was delivered: the message was its one part.");
         StringAssert.Contains(ServerMessage("VIRUS_ATTACHMENT_REMOVED").TrimEnd(), delivered,
            "The notice is not where the attachment was.");
         StringAssert.Contains("Subject: " + ServerMessage("VIRUS_FOUND") + ": One attachment", delivered,
            "The subject does not say that a virus was found.");

         var header = HeaderOf(delivered);
         StringAssert.DoesNotContain("Content-Transfer-Encoding", header,
            "The removed part's encoding is still in the header, so a client decodes the notice by it.");
         StringAssert.DoesNotContain("Content-Disposition", header,
            "The removed part's disposition is still in the header, so a client offers the notice as the file.");
         StringAssert.DoesNotContain("Content-Description", header,
            "The removed part's description is still in the header.");
         StringAssert.DoesNotContain("Content-Type", header,
            "The removed part's type is still in the header.");

         BounceTestSupport.AssertLogContains("the message was one attachment",
            "The log has to say that the notice is all that was delivered.");

         // The one message, read (and so taken) above, was all that arrived.
         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 0);
      }

      /// <summary>
      ///    The CMS object an S/MIME message signed as one part begins with:
      ///    ContentInfo, signed-data (1.2.840.113549.1.7.2), indefinite lengths -
      ///    what SealedMessageTester takes for opaque signed-data.
      /// </summary>
      private const string SignedDataPrefix = "MIAGCSqGSIb3DQEHAqCAMIACAQE=";

      /// <summary>
      ///    And an enveloped-data one's (1.2.840.113549.1.7.3): an encrypted
      ///    message, which the action leaves as it arrived.
      /// </summary>
      private const string EnvelopedDataPrefix = "MIAGCSqGSIb3DQEHA6CAMIACAQAAAAAAAAA=";

      private static string SMimeOnePartMessage(string recipient, string smimeType, string base64)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: One S/MIME part\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: application/pkcs7-mime; smime-type=" + smimeType + "; name=\"smime.p7m\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"smime.p7m\"\r\n" +
                "\r\n" +
                base64 + "\r\n";
      }

      [Test]
      [Description("An S/MIME message signed as one part - signed-data, not encrypted - is replaced by the notice, as any message that is one attachment is.")]
      public void AnSMimeMessageSignedAsOnePartIsReplacedByTheNotice()
      {
         EnableScanner(AlwaysInfectedScanner, eAntivirusAction.hDeleteAttachments);

         // The signed-data object's start, and the EICAR file after it, where
         // the signed content would be.
         var signedData = Convert.FromBase64String(SignedDataPrefix);
         var eicar = Convert.FromBase64String(EicarBase64());
         var content = new byte[signedData.Length + eicar.Length];
         Buffer.BlockCopy(signedData, 0, content, 0, signedData.Length);
         Buffer.BlockCopy(eicar, 0, content, signedData.Length, eicar.Length);
         var body = Convert.ToBase64String(content);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            SMimeOnePartMessage(_account.Address, "signed-data", body));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.DoesNotContain(body, delivered,
            "The signed message the scanner objected to was delivered as it arrived.");
         StringAssert.Contains(ServerMessage("VIRUS_ATTACHMENT_REMOVED").TrimEnd(), delivered,
            "The notice is not where the signed message was.");
         StringAssert.DoesNotContain("Content-Type", HeaderOf(delivered),
            "The signed message's type is still in the header.");

         BounceTestSupport.AssertLogContains("is signed in S/MIME's opaque form",
            "The log has to say that a signed message was replaced, and why.");

         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 0);
      }

      [Test]
      [Description("The negative control: an S/MIME message encrypted as one part - enveloped-data - is delivered as it arrived, as before.")]
      public void AnSMimeMessageEncryptedAsOnePartIsDeliveredAsItArrived()
      {
         EnableScanner(AlwaysInfectedScanner, eAntivirusAction.hDeleteAttachments);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            SMimeOnePartMessage(_account.Address, "enveloped-data", EnvelopedDataPrefix));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.Contains(EnvelopedDataPrefix, delivered,
            "The ciphertext of an encrypted message was not delivered.");
         StringAssert.Contains("application/pkcs7-mime", HeaderOf(delivered),
            "An encrypted message was rewritten.");
         StringAssert.DoesNotContain(ServerMessage("VIRUS_ATTACHMENT_REMOVED").TrimEnd(), delivered,
            "An encrypted message was given the notice.");

         BounceTestSupport.AssertLogContains("attachments have NOT been stripped",
            "A message that still carries what the scanner objected to must be reported, not quietly delivered.");
      }

      [Test]
      [Description("The negative control: a message that is one text part is delivered as it is, with no notice - it is the message's text, which the action keeps.")]
      public void AMessageThatIsOneTextPartIsDeliveredAsItIs()
      {
         EnableScanner(AlwaysInfectedScanner, eAntivirusAction.hDeleteAttachments);

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            "From: correspondent@example.com\r\n" +
            "To: " + _account.Address + "\r\n" +
            "Subject: Only text\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            PlainTextMarker + "\r\n");

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.Contains(PlainTextMarker, delivered,
            "A message that is its text lost the text.");
         StringAssert.Contains("Subject: Only text", delivered,
            "A message with nothing to strip was rewritten.");
         StringAssert.DoesNotContain(ServerMessage("VIRUS_ATTACHMENT_REMOVED").TrimEnd(), delivered,
            "A message with no attachment was given the notice that its attachments were removed.");
      }

      /// <summary>
      ///    The custom scanner for the test below, run as the service runs it. It
      ///    starts a second process that opens the message file without letting
      ///    anyone else read it - as a host antivirus or a backup agent can hold a
      ///    file it is looking at - waits until that process has the file, and
      ///    then answers "infected". The hold ends by itself a few seconds later.
      ///    Deleting the file is shared, so the server can still remove it.
      /// </summary>
      private const string HoldScript =
         "param([string]$File, [string]$Signal, [int]$HoldMilliseconds)\r\n" +
         "$holder = \"`$h = [System.IO.File]::Open('\" + $File + \"', 'Open', 'Read', 'Delete'); \" +\r\n" +
         "   \"[System.IO.File]::WriteAllText('\" + $Signal + \"', 'held'); \" +\r\n" +
         "   \"Start-Sleep -Milliseconds \" + $HoldMilliseconds + \"; `$h.Dispose()\"\r\n" +
         "$encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($holder))\r\n" +
         "Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -WindowStyle Hidden -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded | Out-Null\r\n" +
         "$deadline = [DateTime]::UtcNow.AddSeconds(60)\r\n" +
         "while (-not (Test-Path -LiteralPath $Signal)) {\r\n" +
         "   if ([DateTime]::UtcNow -gt $deadline) { exit 2 }\r\n" +
         "   Start-Sleep -Milliseconds 50\r\n" +
         "}\r\n" +
         "Remove-Item -LiteralPath $Signal -ErrorAction SilentlyContinue\r\n" +
         "exit 3\r\n";

      [Test]
      [Description("A message the stripper cannot read is not delivered: it is deleted, and its recipient is sent the message-deleted notice in place of the note.")]
      public void AMessageTheStripperCannotReadIsDeletedAndItsRecipientIsTold()
      {
         string powershell = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
         if (!File.Exists(powershell))
            Assert.Ignore("No Windows PowerShell at " + powershell + ".");

         // The helper and its signal live in the server's own temporary folder,
         // which this test may write and the service may read.
         var tempDirectory = _settings.Directories.TempDirectory;
         var token = Guid.NewGuid().ToString("N");
         _holdScript = Path.Combine(tempDirectory, "strip-action-hold-" + token + ".ps1");
         _holdSignal = Path.Combine(tempDirectory, "strip-action-held-" + token + ".txt");
         File.WriteAllText(_holdScript, HoldScript);

         EnableScanner("\"" + powershell + "\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + _holdScript +
            "\" \"%FILE%\" \"" + _holdSignal + "\" " + HoldMilliseconds, eAntivirusAction.hDeleteAttachments);

         // Whatever goes on to be delivered waits here, well past the end of the
         // hold: a message the server went on with after the stripper could not
         // read it would be readable again by the time it reached the mailbox.
         var scripting = _settings.Scripting;
         scripting.Language = "JScript";
         _scriptFile = scripting.CurrentScriptFile;
         _originalScript = File.Exists(_scriptFile) ? File.ReadAllText(_scriptFile) : null;
         File.WriteAllText(_scriptFile,
            "function OnDeliverMessage(message)\r\n" +
            "{\r\n" +
            "   var until = new Date().getTime() + " + DeliverMessageWaitMilliseconds + ";\r\n" +
            "   while (new Date().getTime() < until) { }\r\n" +
            "}\r\n");
         scripting.Enabled = true;
         scripting.Reload();

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            TextAndEicarMessage(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         // What the test stands on: the file was held while the stripper read it.
         // The stripper's own line says so, in the words it has had since it
         // first refused to rewrite a message it could not read.
         BounceTestSupport.AssertLogContains("could not be read, so its attachments",
            "The helper did not hold the message file while the stripper read it, so this test has tested nothing.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         StringAssert.DoesNotContain(EicarBase64(), delivered,
            "A message the stripper could not read was delivered as it arrived, the attachment the scanner objected to in it.");
         StringAssert.DoesNotContain(PlainTextMarker, delivered,
            "Something of a message the stripper could not read was delivered.");
         StringAssert.Contains("Subject: " + ServerMessage("VIRUS_FOUND") + ":", delivered,
            "The recipient was not sent the message-deleted notice in place of the note.");

         // The notice, read (and so taken) above, was all that arrived: the
         // message itself never does, once the hold is over or later.
         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 0);

         BounceTestSupport.AssertLogContains("the message could not be read, so its attachments could not be stripped",
            "The deletion has to be in the log, with its reason.");

         // The notice is made from the message's header, which could not be read
         // either while the file was held: said, and the notice goes all the same.
         CustomAsserts.AssertReportedError("Could not read the message header");
      }

      [Test]
      [Description("With the delete action and the recipients' notice on, the recipient of a message whose envelope sender is empty is sent the notice.")]
      public void TheRecipientOfAMessageWithNoSenderIsToldOfItsDeletion()
      {
         EnableScanner(AlwaysInfectedScanner, eAntivirusAction.hDeleteEmail);
         _settings.AntiVirus.NotifyReceiver = true;

         SmtpClientSimulator.StaticSendRaw("", _account.Address, TextAndEicarMessage(_account.Address));

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 1);

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         StringAssert.DoesNotContain(EicarBase64(), delivered,
            "The deleted message was delivered.");
         StringAssert.Contains("Subject: " + ServerMessage("VIRUS_FOUND") + ": Text and a file", delivered,
            "What arrived is not the message-deleted notice.");
      }
   }
}
