// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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
   ///    read or cannot rewrite.
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
   ///      So is a message that can be read but whose file cannot be written,
   ///      since what would be delivered is the file as it arrived.
   ///
   ///    And the "message deleted" notice reaches the recipients of a message
   ///    whose envelope sender is empty, as it reaches any other recipient; and
   ///    when the message's file cannot be read as the delete action's notices
   ///    are made, the sender's notice names the addresses the sender sent the
   ///    message to, as the sender gave them, and each recipient's notice that
   ///    recipient alone.
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

      private static string FirstLineOf(string text)
      {
         var end = text.IndexOfAny(new[] { '\r', '\n' });

         return end < 0 ? text : text.Substring(0, end);
      }

      /// <summary>
      ///    The line of the server's "message deleted" notice (VIRUS_NOTIFICATION)
      ///    that holds the macro, as a notice has it with the macro replaced by the
      ///    value - "   From: someone@example.com" in the shipped text.
      /// </summary>
      private string NoticeLine(string macro, string value)
      {
         foreach (var line in ServerMessage("VIRUS_NOTIFICATION").Split('\n'))
         {
            if (line.Contains(macro))
               return line.TrimEnd('\r').Replace(macro, value);
         }

         Assert.Fail("The server's VIRUS_NOTIFICATION text has no " + macro + ".");
         return null;
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
      ///    The custom scanner for the tests below, run as the service runs it. It
      ///    starts a second process that opens the message file sharing it only as
      ///    <paramref name="sharing"/> says - as a host antivirus or a backup agent
      ///    can hold a file it is looking at - waits until that process has the
      ///    file, and then answers "infected". The hold ends by itself a few
      ///    seconds later. Deleting the file is shared, so the server can still
      ///    remove it.
      /// </summary>
      private static string HoldScript(string sharing)
      {
         return
            "param([string]$File, [string]$Signal, [int]$HoldMilliseconds)\r\n" +
            "$holder = \"`$h = [System.IO.File]::Open('\" + $File + \"', 'Open', 'Read', '" + sharing + "'); \" +\r\n" +
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
      }

      /// <summary>
      ///    HoldScript as the scanner, and <paramref name="action"/> as the
      ///    action: while the server acts on the verdict, another process holds
      ///    the message's file, sharing it as <paramref name="sharing"/> says
      ///    ("Delete": nobody may read it or write it; "Read, Delete": it may be
      ///    read, and not written).
      /// </summary>
      private void ScanWhileAnotherProcessHoldsTheFile(string sharing, eAntivirusAction action)
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
         File.WriteAllText(_holdScript, HoldScript(sharing));

         EnableScanner("\"" + powershell + "\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + _holdScript +
            "\" \"%FILE%\" \"" + _holdSignal + "\" " + HoldMilliseconds, action);
      }

      /// <summary>
      ///    The strip action, with HoldScript as the scanner: while the stripper
      ///    works on the message, another process holds its file, sharing it as
      ///    <paramref name="sharing"/> says ("Delete": nobody may read it or write
      ///    it; "Read, Delete": it may be read, and not written).
      /// </summary>
      private void StripWhileAnotherProcessHoldsTheFile(string sharing)
      {
         ScanWhileAnotherProcessHoldsTheFile(sharing, eAntivirusAction.hDeleteAttachments);

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
      }

      [Test]
      [Description("A message the stripper cannot read is not delivered: it is deleted, and its recipient is sent the message-deleted notice in place of the note.")]
      public void AMessageTheStripperCannotReadIsDeletedAndItsRecipientIsTold()
      {
         StripWhileAnotherProcessHoldsTheFile("Delete");

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
         // either while the file was held: the envelope names the message in its
         // place, and nothing is written to the error log, since what happened is
         // what the two lines above say, and handled.
         StringAssert.Contains(NoticeLine("%MACRO_FROM%", "correspondent@example.com"), delivered,
            "The notice does not name the sender of the message it says was deleted.");
         StringAssert.Contains(NoticeLine("%MACRO_TO%", _account.Address), delivered,
            "The notice does not name the recipient of the message it says was deleted.");

         CustomAsserts.AssertNoReportedError();
      }

      [TestCase("one attachment")]
      [TestCase("text and an attachment")]
      [Description("A message the stripper can read but cannot rewrite is not delivered: it is deleted, and its recipient is sent the message-deleted notice in place of the note.")]
      public void AMessageTheStripperCannotRewriteIsDeletedAndItsRecipientIsTold(string shape)
      {
         // Read, and not written: the stripper reads the message and works out
         // what to keep, and the rewrite is what fails. A message that is one
         // attachment is rewritten as the notice alone, the other as its text
         // and the notice: each way of rewriting it.
         StripWhileAnotherProcessHoldsTheFile("Read, Delete");

         var oneAttachment = shape == "one attachment";

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            oneAttachment ? OneAttachmentMessage(_account.Address, "attachment") : TextAndEicarMessage(_account.Address));

         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         StringAssert.DoesNotContain(EicarBase64(), delivered,
            "A message whose file the stripper could not rewrite was delivered as it arrived, the attachment the scanner objected to in it.");
         StringAssert.DoesNotContain(PlainTextMarker, delivered,
            "Something of a message the stripper could not rewrite was delivered.");
         StringAssert.Contains("Subject: " + ServerMessage("VIRUS_FOUND") + ": " + (oneAttachment ? "One attachment" : "Text and a file"), delivered,
            "The recipient was not sent the message-deleted notice in place of the note.");
         StringAssert.Contains(FirstLineOf(ServerMessage("VIRUS_NOTIFICATION")), delivered,
            "What arrived is not the message-deleted notice.");
         StringAssert.DoesNotContain(ServerMessage("VIRUS_ATTACHMENT_REMOVED").TrimEnd(), delivered,
            "The message was delivered with its attachments stripped, so its file was written after all.");

         // What the test stands on: the file could be read and not written while
         // the stripper rewrote it, and the stripper says so.
         BounceTestSupport.AssertLogContains("could not be rewritten, so its attachments could not be stripped and no part of it can be kept",
            "The helper did not hold the message file against writing while the stripper rewrote it, so this test has tested nothing.");

         BounceTestSupport.AssertLogContains("the message could not be rewritten, so its attachments could not be stripped",
            "The deletion has to be in the log, with its reason.");

         // The notice is made from the message's header, which could be read.
         StringAssert.Contains(NoticeLine("%MACRO_FROM%", "correspondent@example.com"), delivered,
            "The notice does not name the sender of the message it says was deleted.");

         // The notice, read (and so taken) above, was all that arrived.
         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 0);

         CustomAsserts.AssertNoReportedError();
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

      [Test]
      [Description("With the delete action and both notices on, while another process holds the message's file as the notices are made, the sender's notice names the addresses the sender sent the message to, as the sender gave them, and each recipient's notice names that recipient alone.")]
      public void TheDeleteActionsNoticesNameWhomTheMessageWasSentToWhenItsFileCannotBeRead()
      {
         var setup = SingletonProvider<TestSetup>.Instance;

         // A sender of this server, so that its notice can be read here, sending
         // to the fixture's account and to a distribution list of two more.
         var sender = setup.AddAccount(_domain, "strip-sender@example.test", Password);
         var firstMember = setup.AddAccount(_domain, "strip-member1@example.test", Password);
         var secondMember = setup.AddAccount(_domain, "strip-member2@example.test", Password);
         var list = setup.AddDistributionList(_domain, "strip-list@example.test",
            new List<string> { firstMember.Address, secondMember.Address });

         ScanWhileAnotherProcessHoldsTheFile("Delete", eAntivirusAction.hDeleteEmail);
         _settings.AntiVirus.NotifySender = true;
         _settings.AntiVirus.NotifyReceiver = true;

         // The header names another sender and one recipient alone, so that a
         // notice made from it could not pass for one made from the envelope.
         new SmtpClientSimulator().SendRaw(sender.Address, new List<string> { _account.Address, list.Address },
            TextAndEicarMessage(_account.Address));

         var sendersNotice = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         var accountsNotice = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);
         var firstMembersNotice = Pop3ClientSimulator.AssertGetFirstMessageText(firstMember.Address, Password);
         var secondMembersNotice = Pop3ClientSimulator.AssertGetFirstMessageText(secondMember.Address, Password);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // What the test stands on: the file was held while the notices were
         // made. The delete action's notices read it each for itself, and say
         // in the error log that they could not, as they always have.
         CustomAsserts.AssertReportedError("Could not read the message header, since the file was not available");
         Assert.IsTrue(LogHandler.ClearErrorLogUntilSettled(),
            "Errors were still being written after the notices had arrived.");

         BounceTestSupport.AssertLogContains("Message will be deleted (contained virus",
            "The message was not deleted by the delete action.");

         // The sender's: whom the message was sent to, as the sender gave it -
         // the list by its own address, once, and none of its members.
         StringAssert.Contains("Subject: " + ServerMessage("VIRUS_FOUND") + ":", sendersNotice,
            "What reached the sender is not the message-deleted notice.");
         StringAssert.Contains(NoticeLine("%MACRO_FROM%", sender.Address) + "\r\n", sendersNotice,
            "The sender's notice does not name the sender of the message it says was deleted.");
         StringAssert.Contains(NoticeLine("%MACRO_TO%", _account.Address + ", " + list.Address) + "\r\n", sendersNotice,
            "The sender's notice does not name the addresses the sender sent the deleted message to.");
         StringAssert.DoesNotContain(firstMember.Address, sendersNotice,
            "The sender's notice names a member of the distribution list the sender wrote to.");
         StringAssert.DoesNotContain(secondMember.Address, sendersNotice,
            "The sender's notice names a member of the distribution list the sender wrote to.");
         StringAssert.DoesNotContain(EicarBase64(), sendersNotice, "The deleted message reached its sender.");

         // Each recipient's: that recipient, and no other.
         AssertTheRecipientsNoticeNamesThemAlone(accountsNotice, _account.Address, sender.Address,
            firstMember.Address, secondMember.Address);
         AssertTheRecipientsNoticeNamesThemAlone(firstMembersNotice, firstMember.Address, sender.Address,
            _account.Address, secondMember.Address);
         AssertTheRecipientsNoticeNamesThemAlone(secondMembersNotice, secondMember.Address, sender.Address,
            _account.Address, firstMember.Address);

         // The notices, read (and so taken) above, were all that arrived.
         Pop3ClientSimulator.AssertMessageCount(sender.Address, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(firstMember.Address, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(secondMember.Address, Password, 0);
      }

      private void AssertTheRecipientsNoticeNamesThemAlone(string notice, string recipient, string sender, params string[] otherRecipients)
      {
         StringAssert.Contains("Subject: " + ServerMessage("VIRUS_FOUND") + ":", notice,
            "What reached " + recipient + " is not the message-deleted notice.");
         StringAssert.Contains(NoticeLine("%MACRO_FROM%", sender) + "\r\n", notice,
            "The notice to " + recipient + " does not name the sender of the message it says was deleted.");
         StringAssert.Contains(NoticeLine("%MACRO_TO%", recipient) + "\r\n", notice,
            "The notice to " + recipient + " does not name its recipient alone.");

         foreach (var other in otherRecipients)
            StringAssert.DoesNotContain(other, notice, "The notice to " + recipient + " names another recipient, " + other + ".");

         StringAssert.DoesNotContain(EicarBase64(), notice, "The deleted message reached " + recipient + ".");
      }
   }
}
