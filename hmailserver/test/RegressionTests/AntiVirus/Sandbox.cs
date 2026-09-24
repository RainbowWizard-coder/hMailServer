// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiVirus
{
   /// <summary>
   ///    Attachments opened somewhere safe (docs/Sandbox.md): a message that no
   ///    signed-in user of this server submitted, with an attachment of a submitted
   ///    type, is held - in no mailbox, not in the quarantine - while the poller
   ///    submits each such file and asks for its verdict, and the answer decides:
   ///    clean is delivered with a header, malicious is quarantined, and an error or
   ///    the timeout does what SandboxTimeoutAction says.
   ///
   ///    The sandbox is a FakeHttpEndpoint on this fixture's own port: it answers
   ///    the submit with an id and the verdict route with a scripted sequence -
   ///    pending, pending, done - so that the poll is exercised and not skipped.
   ///    The poll runs every five seconds here (the shipped thirty would make every
   ///    test a minute long), and the verdict cache is off unless a test is about
   ///    it, since a verdict remembered from one test would answer for the next.
   ///    Every attachment's bytes carry a fresh GUID for the same reason, and bytes
   ///    no text encoding survives - NUL and 0x80 to 0xFF - so that a file mangled
   ///    between the message and the sandbox is seen.
   ///
   ///    QuarantineEnabled is on for every test but the one about it being off: a
   ///    malicious verdict with the quarantine off is returned to its sender.
   ///
   ///    Ports: the REST listener on 9986 (this row's range is 9986-9995), the
   ///    sandbox on 9987, and 9988 is the address of a sandbox that is down -
   ///    nothing listens there until a test says so.
   ///
   ///    Not tested here, and said so: the bounded retry of a hold that cannot be
   ///    recorded (a database refusing the insert every time), a release racing a
   ///    verdict, the transient read failure of a message file another process
   ///    holds, the Control Panel's Remove the token button, and the sandbox's
   ///    token left out of the backup archive.
   /// </summary>
   [TestFixture]
   public class Sandbox : TestFixtureBase
   {
      private static int RestPort = 9986;
      private const int SandboxPort = 9987;
      private const int DownPort = 9988;

      private const string AdminPassword = "testar";
      private const string Password = "test";
      private const string Outsider = "outsider@sandbox-far.test";

      private const string DefaultTypes =
         "exe,dll,scr,js,jse,vbs,wsf,hta,msi,bat,cmd,ps1,jar,lnk,iso,img,docm,xlsm,pptm,doc,xls,ppt,rtf,pdf,zip,7z,rar";

      private const string Pending = "{\"state\":\"pending\"}";

      private const string SurblBody =
         "This is a test message with a SURBL url: -> http://surbl-org-permanent-test-point.com/ <-";

      private Account _recipient;
      private Account _sender;

      [SetUp]
      public void StartRestApiAndResetTheSandbox()
      {
         _recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sandbox-recipient@" + _domain.Name, Password);
         _sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sandbox-sender@" + _domain.Name, Password);

         IniFileSetting.Write("QuarantineEnabled", "1");
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         ResetSandbox_();
      }

      [TearDown]
      public void StopRestApi()
      {
         // Each step on its own: the one that must run - the sandbox switched off,
         // over COM - is not skipped because the REST listener stopped answering.
         // A held message is not in the delivery queue, so the suite's queue
         // clearing does not reach it, and a sandbox left on would hold the next
         // fixture's attachments for fifteen minutes.
         Try_(DiscardEverythingHeld_);
         Try_(ResetSandbox_);
         Try_(() => DeleteQuarantinedFrom_(Outsider));
         Try_(() => DeleteQuarantinedFrom_(_sender.Address));
         Try_(TestSetup.DeleteMessagesInQueue);
         Try_(() => IniFileSetting.Delete("MessageTraceEnabled"));
         Try_(() => IniFileSetting.Delete("QuarantineEnabled"));

         RestListener.Stop();
         _application.Reinitialize();
      }

      // ---- the hold, and its verdicts ----------------------------------------------

      [Test]
      [Description("An executable from outside is held - in no mailbox, listed as held with its submission, not in the quarantine - its bytes reach the sandbox unchanged, NUL and high bytes included, with their SHA-256 and the token, and once the verdict is clean it is delivered with the header; the trace has both events")]
      public void AnExecutableFromOutsideIsHeldAndDeliveredWithTheHeaderOnceClean()
      {
         IniFileSetting.Write("MessageTraceEnabled", "1");
         _application.Reinitialize();

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-1"))
         {
            sandbox.SetSequence("/verdict", (200, Pending), (200, Pending), (200, Done_("clean", 3, "opened, nothing happened")));
            Enable_(sandbox);

            dynamic settings = _settings;
            settings.SetSandboxToken("sandbox-t0ken");

            byte[] file = FileBytes_("clean");
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address, MessageWithAttachment_(Outsider, _recipient.Address, "sandbox clean", "invoice.exe", file));

            JsonElement held = WaitForSubmitted_(1);
            JsonElement message = held.GetProperty("messages")[0];
            long queueId = message.GetProperty("queue_id").GetInt64();
            JsonElement heldFile = message.GetProperty("files")[0];

            Assert.AreEqual("invoice.exe", heldFile.GetProperty("filename").GetString());
            Assert.AreEqual(Sha256Hex_(file), heldFile.GetProperty("sha256").GetString(), "The hold records the file's SHA-256.");
            Assert.AreEqual("job-1", heldFile.GetProperty("submission").GetString(), "The hold records the id the sandbox answered.");
            Assert.AreEqual("pending", message.GetProperty("decision").GetString());

            // Held, not delivered and not quarantined.
            ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "A held message is in no mailbox.");
            ClassicAssert.IsFalse(QuarantineHolds_(Outsider), "A held message is not in the quarantine.");

            // Listed by the queue, marked held, with its submission.
            (int status, string body) queue = Admin("GET", "/api/v1/queue", null);
            Assert.AreEqual(200, queue.status, queue.body);
            JsonElement listed = JsonDocument.Parse(queue.body).RootElement.GetProperty("messages").EnumerateArray()
               .First(m => m.GetProperty("id").GetInt64() == queueId);
            Assert.AreEqual("sandbox", listed.GetProperty("held").GetString());
            Assert.AreEqual("job-1", listed.GetProperty("submissions")[0].GetString());

            // What the sandbox received: the file, its name, its hash, the queue id and the token.
            Assert.AreEqual(1, sandbox.CountRequests("POST", "/submit"), "One file, one submission.");
            int submitIndex = sandbox.Requests.FindIndex(r => r.StartsWith("POST /submit"));
            string submit = sandbox.Requests[submitIndex];
            StringAssert.Contains("multipart/form-data; boundary=", submit);
            StringAssert.Contains("name=\"sha256\"\r\n\r\n" + Sha256Hex_(file), submit);
            StringAssert.Contains("name=\"filename\"\r\n\r\ninvoice.exe", submit);
            StringAssert.Contains("name=\"message_id\"\r\n\r\n" + queueId, submit);
            StringAssert.Contains("Authorization: Bearer sandbox-t0ken", submit);
            CollectionAssert.AreEqual(file, FilePart_(sandbox.RequestBodies[submitIndex]),
               "The file's own bytes are sent, decoded from the message, byte for byte.");

            // pending, pending, done: delivered with the header, and the hold gone.
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(60));
            string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password);
            StringAssert.Contains("X-hMailServer-Sandbox: clean (1 file(s), ", delivered);
            StringAssert.Contains("sandbox clean", delivered);
            ClassicAssert.GreaterOrEqual(sandbox.CountRequests("GET", "/verdict"), 3, "The verdict was asked for until it was done.");
            StringAssert.Contains("?id=job-1", sandbox.Requests.First(r => r.StartsWith("GET /verdict")));
            Assert.AreEqual(0, Held_().GetProperty("count").GetInt32());

            // The trace tells the story.
            var trace = _application.GlobalObjects.MessageTrace;
            trace.SearchByQueueID((int) queueId);
            var events = new List<string>();
            for (int i = 0; i < trace.Count; i++)
               events.Add(trace[i].EventName);
            CollectionAssert.Contains(events, "sandbox-submitted");
            CollectionAssert.Contains(events, "sandbox-verdict");
         }
      }

      [Test]
      [Description("A message whose attachment is not of a submitted type is delivered at once, with no submission, and without the X-hMailServer-Sandbox header its sender wrote")]
      public void AnAttachmentOutsideTheTypesIsDeliveredAtOnceWithNoSubmissionAndNoForgedHeader()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-2"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               "X-hMailServer-Sandbox: clean (1 file(s), 1s)\r\n" +
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox text", "notes.txt", FileBytes_("text"), "text/plain"));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1);
            string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password);
            StringAssert.DoesNotContain("X-hMailServer-Sandbox", delivered, "The sender's own field is removed as the message arrives.");
            Assert.AreEqual(0, sandbox.CountRequests("POST", "/submit"), "Nothing outside the types is submitted.");
            Assert.AreEqual(0, Held_().GetProperty("count").GetInt32());
         }
      }

      [Test]
      [Description("A malicious verdict quarantines the message with the verdict's detail as the reason, the recipient's digest lists it, and the administrator's release delivers it without a second submission")]
      public void AMaliciousVerdictQuarantinesTheDigestListsItAndTheReleaseDoesNotSubmitAgain()
      {
         dynamic settings = _settings;
         dynamic domain = _domain;

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-3"))
         {
            try
            {
               sandbox.SetSequence("/verdict", (200, Pending), (200, Done_("malicious", 95, "Drops a ransomware payload")));
               Enable_(sandbox);

               SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
                  MessageWithAttachment_(Outsider, _recipient.Address, "sandbox malicious", "invoice.exe", FileBytes_("malicious")));

               Assert.IsTrue(WaitFor_(() => QuarantineHolds_(Outsider), 60), "The malicious message never reached the quarantine.");
               JsonElement entry = QuarantineEntry_(Outsider).Value;
               StringAssert.Contains("Drops a ransomware payload", entry.GetProperty("reason").GetString(), "The verdict's detail is the reason.");
               ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "A malicious message is not delivered.");
               Assert.AreEqual(0, Held_().GetProperty("count").GetInt32(), "The hold ends with the verdict.");

               // The recipient's digest lists it, as it lists anything else held for them.
               settings.QuarantineDigestBaseUrl = "http://127.0.0.1:" + RestPort;
               domain.QuarantineDigest = true;
               domain.QuarantineDigestHour = 7;
               _domain.Save();

               dynamic antiSpam = _settings.AntiSpam;
               ClassicAssert.GreaterOrEqual((int) antiSpam.SendQuarantineDigests(), 1);
               Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1);
               StringAssert.Contains("sandbox malicious", Messages_(_recipient.Address)[0],
                  "The digest names the held message.");

               // Released by the administrator: delivered, and not submitted again.
               (int status, string body) release = Admin("POST", "/api/v1/quarantine/" + entry.GetProperty("id").GetInt64() + "/release", "{}");
               Assert.AreEqual(200, release.status, release.body);
               Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 2);
               List<string> messages = Messages_(_recipient.Address);
               string released = messages.First(m => !m.Contains("X-hMailServer-Quarantine-Digest"));
               StringAssert.Contains("Subject: sandbox malicious", released);
               StringAssert.Contains("X-hMailServer-Sandbox: malicious", released, "The released copy says what the sandbox said.");
               Assert.AreEqual(1, CountOccurrences_(released, "X-hMailServer-Sandbox:"), "The header is written once.");
               Assert.AreEqual(1, sandbox.CountRequests("POST", "/submit"), "The quarantine's release does not submit the file again.");
            }
            finally
            {
               settings.QuarantineDigestBaseUrl = "";
               domain.QuarantineDigest = false;
               _domain.Save();
            }
         }
      }

      [Test]
      [Description("With QuarantineEnabled off, as shipped, a malicious verdict returns the message to its sender with 5.7.1 and puts nothing in a quarantine no digest reads")]
      public void WithTheQuarantineOffAMaliciousMessageIsReturnedToItsSender()
      {
         IniFileSetting.Delete("QuarantineEnabled");
         _application.Reinitialize();

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-q"))
         {
            sandbox.SetSequence("/verdict", (200, Done_("malicious", 99, "Encrypts the disk")));
            Enable_(sandbox);

            // A local sender without signing in: held like any unauthenticated
            // submission, and a mailbox the report can be read from.
            SmtpClientSimulator.StaticSendRaw(_sender.Address, _recipient.Address,
               MessageWithAttachment_(_sender.Address, _recipient.Address, "sandbox malicious no quarantine", "bad.exe", FileBytes_("no-quarantine")));

            Pop3ClientSimulator.AssertMessageCount(_sender.Address, Password, 1, TimeSpan.FromSeconds(60));
            string report = Pop3ClientSimulator.AssertGetFirstMessageText(_sender.Address, Password).Replace("=\r\n", "");
            StringAssert.Contains("Status: 5.7.1", report);
            StringAssert.Contains("quarantine on this server is off", report);
            ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "A malicious message is not delivered.");
            ClassicAssert.AreEqual(0, QuarantineCount_(_sender.Address), "Nothing is put in a quarantine that is off.");
         }
      }

      [Test]
      [Description("A suspicious verdict at or above the score threshold is quarantined; below it the message is delivered as clean")]
      public void SuspiciousAtTheThresholdIsQuarantinedAndBelowItIsDelivered()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-4"))
         {
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxScoreThreshold = 70;

            sandbox.SetSequence("/verdict", (200, Done_("suspicious", 80, "Writes to the Run key")));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox suspicious 80", "macro.docm", FileBytes_("suspicious-80")));

            Assert.IsTrue(WaitFor_(() => QuarantineHolds_(Outsider), 60), "Suspicious at 80 against a threshold of 70 is malicious.");
            StringAssert.Contains("Writes to the Run key", QuarantineEntry_(Outsider).Value.GetProperty("reason").GetString());
            ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password));

            sandbox.SetSequence("/verdict", (200, Done_("suspicious", 50, "Reads the clipboard")));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox suspicious 50", "macro.docm", FileBytes_("suspicious-50")));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(60));
            string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password);
            StringAssert.Contains("sandbox suspicious 50", delivered);
            StringAssert.Contains("X-hMailServer-Sandbox: clean (1 file(s), ", delivered, "Suspicious at 50 is below the threshold, and so clean.");
         }
      }

      [Test]
      [Description("A suspicious answer without a numeric score - missing, or the digits in a string - is an error, never clean, and is not remembered: the same file is submitted again")]
      public void ASuspiciousAnswerWithoutANumericScoreIsAnErrorAndIsNotRemembered()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-s"))
         {
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxCacheHours = 24;
            settings.SandboxScoreThreshold = 1;

            byte[] same = FileBytes_("no-score");
            sandbox.SetSequence("/verdict", (200, "{\"state\":\"done\",\"verdict\":\"suspicious\",\"detail\":\"No score given\"}"));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address, MessageWithAttachment_(Outsider, _recipient.Address, "sandbox no score", "tool.exe", same));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(60));
            string first = Messages_(_recipient.Address)[0];
            StringAssert.Contains("X-hMailServer-Sandbox: error on tool.exe: answered suspicious without a numeric score", first);

            sandbox.SetSequence("/verdict", (200, "{\"state\":\"done\",\"verdict\":\"suspicious\",\"score\":\"85\",\"detail\":\"A string score\"}"));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address, MessageWithAttachment_(Outsider, _recipient.Address, "sandbox string score", "tool.exe", same));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 2, TimeSpan.FromSeconds(60));
            string second = Messages_(_recipient.Address).First(m => m.Contains("Subject: sandbox string score"));
            StringAssert.Contains("answered suspicious without a numeric score", second, "A score in a string is not a number.");
            StringAssert.DoesNotContain("X-hMailServer-Sandbox: clean", second);
            Assert.AreEqual(2, sandbox.CountRequests("POST", "/submit"), "An error is not remembered: the same file is submitted again.");
         }
      }

      [Test]
      [Description("The same file twice is submitted once while its clean verdict is remembered, and a remembered malicious file quarantines the next message without a submission")]
      public void TheSameFileTwiceIsSubmittedOnceAndARememberedMaliciousOneIsQuarantinedWithoutASubmission()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-5"))
         {
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxCacheHours = 24;

            byte[] same = FileBytes_("cached-clean");
            sandbox.SetSequence("/verdict", (200, Done_("clean", 0, "nothing")));

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address, MessageWithAttachment_(Outsider, _recipient.Address, "sandbox first", "tool.exe", same));
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(60));

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address, MessageWithAttachment_(Outsider, _recipient.Address, "sandbox second", "tool.exe", same));
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 2, TimeSpan.FromSeconds(20));
            string second = Messages_(_recipient.Address).First(m => m.Contains("Subject: sandbox second"));
            StringAssert.Contains("X-hMailServer-Sandbox: clean (1 file(s), 0s)", second, "The remembered verdict delivers at once.");
            Assert.AreEqual(1, sandbox.CountRequests("POST", "/submit"), "The same file is submitted once.");

            // A malicious verdict is remembered too.
            byte[] bad = FileBytes_("cached-malicious");
            sandbox.SetSequence("/verdict", (200, Done_("malicious", 99, "Encrypts the disk")));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address, MessageWithAttachment_(Outsider, _recipient.Address, "sandbox bad one", "bad.exe", bad));
            Assert.IsTrue(WaitFor_(() => QuarantineCount_(Outsider) == 1, 60), "The first malicious message is quarantined.");
            Assert.AreEqual(2, sandbox.CountRequests("POST", "/submit"));

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address, MessageWithAttachment_(Outsider, _recipient.Address, "sandbox bad two", "bad.exe", bad));
            Assert.IsTrue(WaitFor_(() => QuarantineCount_(Outsider) == 2, 20), "The second is quarantined on the remembered verdict.");
            Assert.AreEqual(2, sandbox.CountRequests("POST", "/submit"), "A remembered malicious file is not submitted again.");
            ClassicAssert.AreEqual(2, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "Neither malicious message is delivered.");
         }
      }

      // ---- an error, and the waits that end ---------------------------------------

      [Test]
      [Description("An error on one file does not end the hold while another file is still waiting: the message stays held, and the other file's malicious answer quarantines it. The ids come from the address's {id}.")]
      public void AnErrorOnOneFileWaitsForTheOthersAndAMaliciousAnswerStillQuarantines()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("unused"))
         {
            sandbox.SetSequence("/submit", (200, "{\"id\":\"job-a\"}"), (200, "{\"id\":\"job-b\"}"));
            sandbox.SetSequence("/verdict/job-a", (200, Done_("error", 0, "Password-protected archive")));
            sandbox.SetSequence("/verdict/job-b", (200, Pending));

            dynamic settings = _settings;
            settings.SandboxSubmitUrl = sandbox.UrlFor("/submit");
            settings.SandboxVerdictUrl = sandbox.UrlFor("/verdict/{id}");
            settings.SandboxPollSeconds = 5;
            settings.SandboxCacheHours = 0;
            settings.SandboxEnabled = true;

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithParts_(Outsider, _recipient.Address, "sandbox error and pending",
                  AttachmentPart_("application/octet-stream", "a.zip", FileBytes_("error-a")),
                  AttachmentPart_("application/octet-stream", "b.exe", FileBytes_("pending-b"))));

            WaitForHeld_(1);
            Assert.IsTrue(WaitFor_(() =>
            {
               JsonElement files = Held_().GetProperty("messages")[0].GetProperty("files");
               return files.EnumerateArray().Any(f => f.GetProperty("filename").GetString() == "a.zip" && f.GetProperty("verdict").GetString() == "error");
            }, 40), "The first file was answered error. " + Held_());

            // Several more polls: still held, still pending.
            Thread.Sleep(TimeSpan.FromSeconds(15));
            JsonElement held = Held_();
            Assert.AreEqual(1, held.GetProperty("count").GetInt32(), "An error on one file does not end the hold while another waits.");
            Assert.AreEqual("pending", held.GetProperty("messages")[0].GetProperty("decision").GetString());
            ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "Nothing is delivered beside the waiting file.");
            ClassicAssert.GreaterOrEqual(sandbox.CountRequests("GET", "/verdict/job-b"), 2, "The other file is still asked about, by its own id in the address.");

            sandbox.SetSequence("/verdict/job-b", (200, Done_("malicious", 97, "Injects into explorer.exe")));
            Assert.IsTrue(WaitFor_(() => QuarantineHolds_(Outsider), 60), "The waiting file's malicious answer quarantines the message.");
            StringAssert.Contains("Injects into explorer.exe", QuarantineEntry_(Outsider).Value.GetProperty("reason").GetString());
            ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password));
         }
      }

      [Test]
      [Description("Once every file has answered and one of them is an error, SandboxTimeoutAction decides: 0 delivers with a header naming the file, 1 returns the message to its sender with 5.7.1")]
      public void AnErrorOnceEveryFileHasAnsweredIsDecidedByTheAction()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-e"))
         {
            sandbox.SetSequence("/verdict", (200, Done_("error", 0, "Unsupported format")));
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxTimeoutAction = 0;

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox error deliver", "odd.exe", FileBytes_("error-deliver")));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(60));
            StringAssert.Contains("X-hMailServer-Sandbox: error on odd.exe: Unsupported format",
               Messages_(_recipient.Address)[0]);

            settings.SandboxTimeoutAction = 1;
            SmtpClientSimulator.StaticSendRaw(_sender.Address, _recipient.Address,
               MessageWithAttachment_(_sender.Address, _recipient.Address, "sandbox error bounce", "odd.exe", FileBytes_("error-bounce")));

            Pop3ClientSimulator.AssertMessageCount(_sender.Address, Password, 1, TimeSpan.FromSeconds(60));
            string report = Pop3ClientSimulator.AssertGetFirstMessageText(_sender.Address, Password).Replace("=\r\n", "");
            StringAssert.Contains("Status: 5.7.1", report);
            StringAssert.Contains("error on odd.exe", report);
            ClassicAssert.AreEqual(1, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "The returned message is not delivered.");
         }
      }

      [Test]
      [Description("A hold the sandbox never answers ends at SandboxTimeoutMinutes, and with the action deliver the message is delivered with a header saying it timed out")]
      public void AHoldPastItsTimeoutIsDeliveredWithAHeaderSayingSo()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-6"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxTimeoutMinutes = 1;
            settings.SandboxTimeoutAction = 0;

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox never answers", "setup.msi", FileBytes_("timeout-deliver")));

            WaitForHeld_(1);
            ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(120));
            StringAssert.Contains("X-hMailServer-Sandbox: timed out after 1 minute(s)", Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password));
         }
      }

      [Test]
      [Description("With the action bounce, a hold past its timeout is returned to its sender with 5.7.1 and not delivered")]
      public void AHoldPastItsTimeoutIsReturnedToTheSenderWhenTheActionIsBounce()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-7"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxTimeoutMinutes = 1;
            settings.SandboxTimeoutAction = 1;

            // A local sender without signing in: not a signed-in submission, so it
            // is held, and a sender whose mailbox the report can be read from.
            SmtpClientSimulator.StaticSendRaw(_sender.Address, _recipient.Address,
               MessageWithAttachment_(_sender.Address, _recipient.Address, "sandbox bounce", "setup.msi", FileBytes_("timeout-bounce")));

            WaitForHeld_(1);

            Pop3ClientSimulator.AssertMessageCount(_sender.Address, Password, 1, TimeSpan.FromSeconds(120));
            string report = Pop3ClientSimulator.AssertGetFirstMessageText(_sender.Address, Password);
            StringAssert.Contains("Status: 5.7.1", report);
            StringAssert.Contains("sandbox bounce", report);
            // The sentence a person reads is quoted-printable, whose soft line
            // breaks can fall inside it; read with them taken out.
            StringAssert.Contains("did not give a verdict", report.Replace("=\r\n", ""));
            ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "A returned message is not delivered.");
         }
      }

      [Test]
      [Description("A sandbox that accepts the connection and never answers does not stretch the hold: the timeout ends it within a pass or two")]
      public void ASandboxThatHangsDoesNotStretchTheTimeout()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-h"))
         {
            // The submit is answered; every verdict request hangs past its own
            // timeout, and the endpoint answers nothing else meanwhile.
            sandbox.SetStall("/verdict", 40);
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxTimeoutMinutes = 1;
            settings.SandboxTimeoutAction = 0;

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox hangs", "setup.exe", FileBytes_("hang")));

            WaitForSubmitted_(1);
            DateTime held = DateTime.UtcNow;

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(240));
            StringAssert.Contains("X-hMailServer-Sandbox: timed out after 1 minute(s)", Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password));
            ClassicAssert.Less((DateTime.UtcNow - held).TotalSeconds, 200, "A hanging sandbox held the message far past its minute.");
         }
      }

      [Test]
      [Description("A sandbox that is down - the connection refused - is counted and retried from the queue file at every poll, and once it comes up the same bytes are submitted and the clean answer delivers the message before the timeout")]
      public void ASandboxThatIsDownIsRetriedAndTheFileIsSubmittedWhenItComesUp()
      {
         dynamic settings = _settings;
         settings.SandboxSubmitUrl = "http://127.0.0.1:" + DownPort + "/submit";
         settings.SandboxVerdictUrl = "http://127.0.0.1:" + DownPort + "/verdict";
         settings.SandboxPollSeconds = 5;
         settings.SandboxCacheHours = 0;
         settings.SandboxTimeoutMinutes = 10;
         settings.SandboxTimeoutAction = 0;
         settings.SandboxEnabled = true;

         byte[] file = FileBytes_("down");
         SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
            MessageWithAttachment_(Outsider, _recipient.Address, "sandbox is down", "update.exe", file));

         WaitForHeld_(1);
         Assert.IsTrue(WaitFor_(() =>
         {
            JsonElement held = Held_();
            if (held.GetProperty("count").GetInt32() != 1)
               return false;
            JsonElement heldFile = held.GetProperty("messages")[0].GetProperty("files")[0];
            return heldFile.GetProperty("state").GetString() == "not_submitted" && heldFile.GetProperty("errors").GetInt32() >= 2;
         }, 40), "The failed submit is counted and tried again at every poll. " + Held_());

         ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "Nothing is delivered while the sandbox is down.");

         // Up: the next poll reads the file from the queued message again and submits it.
         using (var up = new FakeHttpEndpoint(DownPort, 200, Pending, "application/json"))
         {
            up.SetResponse("/submit", 200, Encoding.UTF8.GetBytes("{\"id\":\"job-up\"}"), "application/json");
            up.SetSequence("/verdict", (200, Done_("clean", 1, "came up")));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(90));
            StringAssert.Contains("X-hMailServer-Sandbox: clean (1 file(s), ", Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password),
               "Delivered on the verdict, not on the timeout.");

            Assert.AreEqual(1, up.CountRequests("POST", "/submit"));
            int submitIndex = up.Requests.FindIndex(r => r.StartsWith("POST /submit"));
            StringAssert.Contains("name=\"sha256\"\r\n\r\n" + Sha256Hex_(file), up.Requests[submitIndex]);
            CollectionAssert.AreEqual(file, FilePart_(up.RequestBodies[submitIndex]), "The retried submit carries the file's own bytes.");
         }
      }

      [Test]
      [Description("The submit's id is used as the sandbox answered it - a number, and {id} and {sha256} in the address - an answer that is not the contract is a failed request, counted, and a submit that answered no id is polled by the hash, {id} included")]
      public void TheSubmissionIdIsUsedAsAnsweredAndAMalformedVerdictIsCounted()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("unused"))
         {
            sandbox.SetResponse("/submit", 200, Encoding.UTF8.GetBytes("{\"id\":12345}"), "application/json");
            sandbox.SetSequence("/verdict", (200, "this is not the contract"));

            dynamic settings = _settings;
            settings.SandboxSubmitUrl = sandbox.UrlFor("/submit");
            settings.SandboxVerdictUrl = sandbox.UrlFor("/verdict") + "?job={id}&hash={sha256}";
            settings.SandboxPollSeconds = 5;
            settings.SandboxCacheHours = 0;
            settings.SandboxEnabled = true;

            byte[] numbered = FileBytes_("numbered");
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox numbered", "numbered.exe", numbered));

            Assert.IsTrue(WaitFor_(() =>
            {
               JsonElement held = Held_();
               if (held.GetProperty("count").GetInt32() != 1)
                  return false;
               JsonElement heldFile = held.GetProperty("messages")[0].GetProperty("files")[0];
               return heldFile.GetProperty("state").GetString() == "submitted" && heldFile.GetProperty("errors").GetInt32() >= 2;
            }, 40), "An answer that is not the contract is a failed request, counted and asked again. " + Held_());
            Assert.AreEqual("12345", Held_().GetProperty("messages")[0].GetProperty("files")[0].GetProperty("submission").GetString(), "A numeric id is kept as its digits.");
            StringAssert.Contains("GET /verdict?job=12345&hash=" + Sha256Hex_(numbered) + " ", sandbox.Requests.First(r => r.StartsWith("GET /verdict")));

            sandbox.SetSequence("/verdict", (200, Done_("clean", 0, "nothing")));
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(60));

            // No id at all: {id} in the address becomes the hash.
            sandbox.SetResponse("/submit", 200, Encoding.UTF8.GetBytes("{}"), "application/json");
            settings.SandboxVerdictUrl = sandbox.UrlFor("/verdict/{id}");

            byte[] unnamed = FileBytes_("no-id");
            string hash = Sha256Hex_(unnamed);
            sandbox.SetSequence("/verdict/" + hash, (200, Done_("clean", 0, "nothing")));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox no id", "noid.exe", unnamed));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 2, TimeSpan.FromSeconds(60));
            ClassicAssert.GreaterOrEqual(sandbox.CountRequests("GET", "/verdict/" + hash), 1, "Polled by the hash when the submit named no id.");
            StringAssert.Contains("X-hMailServer-Sandbox: clean", Messages_(_recipient.Address).First(m => m.Contains("Subject: sandbox no id")));
         }
      }

      // ---- what is and is not examined ---------------------------------------------

      [Test]
      [Description("What this server's signed-in users submit - over authenticated SMTP to a colleague or to the outside, or through the webmail - is not held or uploaded while SandboxInternalMail is off, and is when it is on")]
      public void WhatSignedInUsersSubmitIsNotHeldOrUploadedUnlessAskedFor()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-9"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);

            new SmtpClientSimulator().SendRaw(_sender.Address, Password, _sender.Address, _recipient.Address,
               MessageWithAttachment_(_sender.Address, _recipient.Address, "sandbox internal", "build.exe", FileBytes_("internal-off")));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1);
            StringAssert.DoesNotContain("X-hMailServer-Sandbox", Messages_(_recipient.Address)[0]);

            // The webmail's send, through the same route the send API uses.
            (int status, string body) sent = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/me/messages", Basic(_sender.Address, Password),
               "{\"to\":\"" + _recipient.Address + "\",\"subject\":\"sandbox webmail\",\"text\":\"See the report.\"," +
               "\"attachments\":[{\"name\":\"Quarterly-Report.pdf\",\"type\":\"application/pdf\",\"data\":\"" + Convert.ToBase64String(FileBytes_("webmail")) + "\"}]}");
            Assert.AreEqual(201, sent.status, sent.body);
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 2);
            StringAssert.DoesNotContain("X-hMailServer-Sandbox", Messages_(_recipient.Address).First(m => m.Contains("sandbox webmail")));

            // Outbound: a signed-in user's document to the outside does not leave
            // for the sandbox either.
            new SmtpClientSimulator().SendRaw(_sender.Address, Password, _sender.Address, "partner@sandbox-far.test",
               MessageWithAttachment_(_sender.Address, "partner@sandbox-far.test", "sandbox outbound", "contract.doc", FileBytes_("outbound")));
            Thread.Sleep(TimeSpan.FromSeconds(10));

            Assert.AreEqual(0, Held_().GetProperty("count").GetInt32(), "Nothing a signed-in user sent is held.");
            Assert.AreEqual(0, sandbox.CountRequests("POST", "/submit"), "Nothing a signed-in user sent is uploaded.");

            dynamic settings = _settings;
            settings.SandboxInternalMail = true;

            new SmtpClientSimulator().SendRaw(_sender.Address, Password, _sender.Address, _recipient.Address,
               MessageWithAttachment_(_sender.Address, _recipient.Address, "sandbox internal held", "build.exe", FileBytes_("internal-on")));

            WaitForSubmitted_(1);
            Assert.AreEqual(1, sandbox.CountRequests("POST", "/submit"), "With SandboxInternalMail on, a signed-in submission is submitted like any other.");
         }
      }

      [Test]
      [Description("A trailing dot, the Content-Type name beside a harmless filename, a forwarded message carrying an executable, and a file name with an emoji are all held; the emoji is stored as a question mark")]
      public void ANameVariantOrAForwardedMessageDoesNotTakeAListedTypePastTheSandbox()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-n"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);

            // "invoice.exe." is saved by Windows as invoice.exe.
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox trailing dot", "invoice.exe.", FileBytes_("trailing-dot")));
            WaitForHeld_(1);

            // The filename a client may ignore for the name it prefers.
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithParts_(Outsider, _recipient.Address, "sandbox two names",
                  "Content-Type: application/octet-stream; name=\"payload.exe\"\r\n" +
                  "Content-Disposition: attachment; filename=\"notes.txt\"\r\n" +
                  "Content-Transfer-Encoding: base64\r\n\r\n" +
                  Convert.ToBase64String(FileBytes_("two-names"), Base64FormattingOptions.InsertLineBreaks) + "\r\n"));
            WaitForHeld_(2);

            // A forwarded message whose own attachment is the executable.
            // Its own boundary, which the outer message's parser must not meet.
            string inner = MessageWithAttachment_("someone@elsewhere.test", "outsider@sandbox-far.test", "the original", "dropper.exe", FileBytes_("forwarded"))
               .Replace("sandbox-boundary", "inner-boundary");
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithParts_(Outsider, _recipient.Address, "sandbox forwarded",
                  "Content-Type: message/rfc822\r\n" +
                  "Content-Disposition: attachment\r\n\r\n" +
                  inner + "\r\n"));
            WaitForHeld_(3);

            // An emoji in the name, RFC 2231 encoded.
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithParts_(Outsider, _recipient.Address, "sandbox emoji",
                  "Content-Type: application/octet-stream\r\n" +
                  "Content-Disposition: attachment; filename*=utf-8''%F0%9F%98%80invoice.exe\r\n" +
                  "Content-Transfer-Encoding: base64\r\n\r\n" +
                  Convert.ToBase64String(FileBytes_("emoji"), Base64FormattingOptions.InsertLineBreaks) + "\r\n"));
            JsonElement held = WaitForHeld_(4);

            List<string> names = held.GetProperty("messages").EnumerateArray()
               .SelectMany(m => m.GetProperty("files").EnumerateArray().Select(f => f.GetProperty("filename").GetString())).ToList();
            CollectionAssert.Contains(names, "invoice.exe.", "The trailing dot is judged as Windows saves the file. " + string.Join(" | ", names));
            CollectionAssert.Contains(names, "notes.txt", "The Content-Type name is judged beside the filename. " + string.Join(" | ", names));
            CollectionAssert.Contains(names, "dropper.exe", "A forwarded message is opened. " + string.Join(" | ", names));
            CollectionAssert.Contains(names, "?invoice.exe", "A character outside the Basic Multilingual Plane is stored as a question mark. " + string.Join(" | ", names));
         }
      }

      [Test]
      [Description("A message with more files to submit than SandboxMaxFiles is not held and nothing is uploaded: SandboxTimeoutAction decides it at once, with a header saying why")]
      public void AMessageOverTheFileCapIsDecidedByTheActionAtOnce()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-c"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxMaxFiles = 2;
            settings.SandboxTimeoutAction = 0;

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithParts_(Outsider, _recipient.Address, "sandbox three files",
                  AttachmentPart_("application/octet-stream", "one.exe", FileBytes_("cap-1")),
                  AttachmentPart_("application/octet-stream", "two.exe", FileBytes_("cap-2")),
                  AttachmentPart_("application/octet-stream", "three.exe", FileBytes_("cap-3"))));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(30));
            StringAssert.Contains("X-hMailServer-Sandbox: not examined: 3 files to submit, more than SandboxMaxFiles (2)",
               Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password));
            Assert.AreEqual(0, sandbox.CountRequests("POST", "/submit"), "Nothing over the cap is uploaded.");
         }
      }

      [Test]
      [Description("A message larger than the 80 MB the server parses is not delivered unexamined: SandboxTimeoutAction decides it, here by returning it to its sender")]
      public void AMessageTooLargeToReadIsDecidedByTheAction()
      {
         dynamic settings = _settings;
         int maxMessageSize = _settings.MaxMessageSize;

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-l"))
         {
            try
            {
               sandbox.SetSequence("/verdict", (200, Pending));
               Enable_(sandbox);
               settings.SandboxTimeoutAction = 1;
               _settings.MaxMessageSize = 0;

               // About 82 MB once base64: past what MessageData parses.
               var large = new byte[62 * 1024 * 1024];
               for (int i = 0; i < large.Length; i++)
                  large[i] = (byte) (i % 251);

               SmtpClientSimulator.StaticSendRaw(_sender.Address, _recipient.Address,
                  MessageWithAttachment_(_sender.Address, _recipient.Address, "sandbox too large to read", "huge.zip", large));

               Pop3ClientSimulator.AssertMessageCount(_sender.Address, Password, 1, TimeSpan.FromSeconds(180));
               string report = Pop3ClientSimulator.AssertGetFirstMessageText(_sender.Address, Password).Replace("=\r\n", "");
               StringAssert.Contains("Status: 5.7.1", report);
               StringAssert.Contains("larger than 80 MB", report);
               ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "It is not delivered unexamined.");
               Assert.AreEqual(0, sandbox.CountRequests("POST", "/submit"));
            }
            finally
            {
               _settings.MaxMessageSize = maxMessageSize;
            }
         }
      }

      [Test]
      [Description("A message the spam quarantine held at SMTP time never met the sandbox, and its release is examined like new mail: held and submitted")]
      public void AReleaseFromTheSpamQuarantineIsExaminedLikeNewMail()
      {
         var antiSpam = _application.Settings.AntiSpam;
         int markThreshold = antiSpam.SpamMarkThreshold;
         int deleteThreshold = antiSpam.SpamDeleteThreshold;
         var surbl = antiSpam.SURBLServers[0];
         bool surblActive = surbl.Active;
         int surblScore = surbl.Score;

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-r"))
         {
            try
            {
               sandbox.SetSequence("/verdict", (200, Pending));
               Enable_(sandbox);

               antiSpam.SpamMarkThreshold = 1;
               antiSpam.SpamDeleteThreshold = 5;
               surbl.Active = true;
               surbl.Score = 10;
               surbl.Save();

               SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
                  MessageWithBodyAndParts_(Outsider, _recipient.Address, "sandbox spam quarantined", SurblBody,
                     AttachmentPart_("application/octet-stream", "invoice.exe", FileBytes_("spam"))));

               Assert.IsTrue(WaitFor_(() => QuarantineHolds_(Outsider), 30), "The spam quarantine held the message at SMTP time.");
               Assert.AreEqual(0, sandbox.CountRequests("POST", "/submit"), "It never reached the sandbox.");

               surbl.Active = surblActive;
               surbl.Save();

               (int status, string body) release = Admin("POST", "/api/v1/quarantine/" + QuarantineEntry_(Outsider).Value.GetProperty("id").GetInt64() + "/release", "{}");
               Assert.AreEqual(200, release.status, release.body);

               WaitForSubmitted_(1);
               Assert.AreEqual(1, sandbox.CountRequests("POST", "/submit"), "The release is examined like new mail.");
               ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, Password), "It waits for the verdict.");
            }
            finally
            {
               antiSpam.SpamMarkThreshold = markThreshold;
               antiSpam.SpamDeleteThreshold = deleteThreshold;
               surbl.Active = surblActive;
               surbl.Score = surblScore;
               surbl.Save();
            }
         }
      }

      [Test]
      [Description("A message the anti-virus deleted is never submitted; the same message past a scanner that finds nothing is")]
      public void AMessageTheAntiVirusDeletedIsNeverSubmitted()
      {
         const string infected = @"C:\Windows\System32\cmd.exe /c exit 3&rem %FILE%";
         const string clean = @"C:\Windows\System32\cmd.exe /c exit 0&rem %FILE%";

         var antiVirus = _settings.AntiVirus;
         bool enabled = antiVirus.CustomScannerEnabled;
         string executable = antiVirus.CustomScannerExecutable;
         int returnValue = antiVirus.CustomScannerReturnValue;
         eAntivirusAction action = antiVirus.Action;
         bool notifySender = antiVirus.NotifySender;
         bool notifyReceiver = antiVirus.NotifyReceiver;

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-10"))
         {
            try
            {
               sandbox.SetSequence("/verdict", (200, Pending));
               Enable_(sandbox);

               antiVirus.NotifySender = false;
               antiVirus.NotifyReceiver = false;
               antiVirus.Action = eAntivirusAction.hDeleteEmail;
               antiVirus.CustomScannerExecutable = infected;
               antiVirus.CustomScannerReturnValue = 3;
               antiVirus.CustomScannerEnabled = true;

               LogHandler.DeleteCurrentDefaultLog();
               SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
                  MessageWithAttachment_(Outsider, _recipient.Address, "sandbox infected", "virus.exe", FileBytes_("infected")));

               Assert.IsTrue(WaitFor_(() => LogHandler.ReadCurrentDefaultLog().Contains("Message will be deleted (contained virus"), 30),
                  "The scanner deleted the message.");
               Assert.AreEqual(0, sandbox.CountRequests("POST", "/submit"), "A message the anti-virus deleted is not submitted.");
               Assert.AreEqual(0, Held_().GetProperty("count").GetInt32());

               antiVirus.CustomScannerExecutable = clean;
               SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
                  MessageWithAttachment_(Outsider, _recipient.Address, "sandbox scanned clean", "tool.exe", FileBytes_("scanned")));

               WaitForSubmitted_(1);
               Assert.AreEqual(1, sandbox.CountRequests("POST", "/submit"), "What the scanner passes goes on to the sandbox.");
            }
            finally
            {
               antiVirus.CustomScannerEnabled = enabled;
               antiVirus.CustomScannerExecutable = executable;
               antiVirus.CustomScannerReturnValue = returnValue;
               antiVirus.Action = action;
               antiVirus.NotifySender = notifySender;
               antiVirus.NotifyReceiver = notifyReceiver;
            }
         }
      }

      [Test]
      [Description("An attachment larger than SandboxMaxSizeKB is not submitted, and the message is delivered at once with a header saying so")]
      public void AnAttachmentOverTheSizeLimitIsDeliveredWithAHeaderSayingSo()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-11"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);
            dynamic settings = _settings;
            settings.SandboxMaxSizeKB = 1;

            byte[] large = Encoding.ASCII.GetBytes(new string('A', 4096) + Guid.NewGuid().ToString("N"));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox too large", "large.zip", large));

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1);
            StringAssert.Contains("X-hMailServer-Sandbox: not submitted: large.zip (larger than 1 KB",
               Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password));
            Assert.AreEqual(0, sandbox.CountRequests("POST", "/submit"), "A file over the limit is not submitted.");
         }
      }

      [Test]
      [Description("A locked account's message waiting for the sandbox is moved to the compromised-account review, and no sandbox verdict sends it")]
      public void TheCompromiseReviewHoldTakesPrecedenceOverTheSandbox()
      {
         Account takeover = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sandbox-takeover@" + _domain.Name, Password);
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sandbox-stranger1@" + _domain.Name, Password);
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sandbox-stranger2@" + _domain.Name, Password);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sandbox-stranger3@" + _domain.Name, Password);

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-k"))
         {
            try
            {
               Assert.AreEqual(200, Admin("PUT", "/api/v1/settings",
                  "{\"compromise_detection_mode\":\"enforce\",\"compromise_volume_floor\":3,\"compromise_recipient_floor\":3}").status);

               sandbox.SetSequence("/verdict", (200, Pending));
               Enable_(sandbox);
               dynamic settings = _settings;
               settings.SandboxInternalMail = true;

               // The first, with a file, is held for the sandbox; the third completes
               // the shape of a takeover and locks the account.
               new SmtpClientSimulator().SendRaw(takeover.Address, Password, takeover.Address, first.Address,
                  MessageWithAttachment_(takeover.Address, first.Address, "Message 1", "payroll.exe", FileBytes_("takeover")));
               WaitForSubmitted_(1);

               string error;
               new SmtpClientSimulator().Send(false, takeover.Address, Password, takeover.Address, second.Address, "Message 2", "The text.", out error);
               new SmtpClientSimulator().Send(false, takeover.Address, Password, takeover.Address, third.Address, "Message 3", "The text.", out error);

               Assert.IsTrue(WaitFor_(() => Held_().GetProperty("count").GetInt32() == 0, 30), "The locked account's held message left the sandbox hold for review.");
               (int status, string body) review = Admin("GET", "/api/v1/review", null);
               Assert.AreEqual(200, review.status, review.body);
               StringAssert.Contains("Message 1", review.body, "It waits on the review page.");

               // The sandbox answers clean; the review hold stands.
               sandbox.SetSequence("/verdict", (200, Done_("clean", 0, "nothing")));
               Thread.Sleep(TimeSpan.FromSeconds(20));
               ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(first.Address, Password), "No sandbox decision sends a locked account's mail.");
            }
            finally
            {
               (int status, string body) review = Admin("GET", "/api/v1/review", null);
               if (review.status == 200)
               {
                  foreach (JsonElement held in JsonDocument.Parse(review.body).RootElement.GetProperty("messages").EnumerateArray())
                     Admin("DELETE", "/api/v1/review/" + held.GetProperty("id").GetInt64(), null);
               }

               Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(takeover.Address) + "/unlock", null);
               Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"watch\",\"compromise_volume_floor\":30,\"compromise_recipient_floor\":20}");
            }
         }
      }

      // ---- the administrator -------------------------------------------------------

      [Test]
      [Description("An administrator releases one held message, which is delivered saying so, and discards another; both are on the audit trail as their own rows; the list pages; a read-only key and a key confined to a domain are refused")]
      public void AnAdministratorReleasesOrDiscardsAHeldMessageAndBothAreAudited()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-12"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox release me", "one.exe", FileBytes_("release")));
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox discard me", "two.exe", FileBytes_("discard")));

            JsonElement held = WaitForSubmitted_(2);
            var ids = held.GetProperty("messages").EnumerateArray()
               .ToDictionary(m => m.GetProperty("files")[0].GetProperty("filename").GetString(), m => m.GetProperty("queue_id").GetInt64());
            long releaseId = ids["one.exe"];
            long discardId = ids["two.exe"];

            // A page at a time, with the total beside it.
            JsonElement page = Held_("?limit=1");
            Assert.AreEqual(1, page.GetProperty("count").GetInt32());
            Assert.AreEqual(2, page.GetProperty("total").GetInt32());
            long firstId = page.GetProperty("messages")[0].GetProperty("queue_id").GetInt64();
            JsonElement next = Held_("?offset=1&limit=1");
            Assert.AreNotEqual(firstId, next.GetProperty("messages")[0].GetProperty("queue_id").GetInt64(), "The second page is the other message.");
            Assert.AreEqual(400, Admin("GET", "/api/v1/sandbox/held?limit=0", null).status);

            // The queue lists them, and does not retry or delete them: 409, naming the sandbox's routes.
            (int status, string body) retry = Admin("POST", "/api/v1/queue/" + releaseId + "/retry", "{}");
            Assert.AreEqual(409, retry.status, retry.body);
            StringAssert.Contains("/api/v1/sandbox/held/", retry.body);
            Assert.AreEqual(409, Admin("DELETE", "/api/v1/queue/" + discardId, null).status);

            // A read-only key reads the list and changes nothing.
            (string id, string token) reader = CreateKey_("{\"label\":\"sandbox - read-only\",\"scope\":\"readonly\"}");
            (string id, string token) confined = CreateKey_("{\"label\":\"sandbox - one domain\",\"scope\":\"full\",\"domains\":\"" + _domain.Name + "\"}");
            try
            {
               Assert.AreEqual(200, Bearer_("GET", "/api/v1/sandbox/held", reader.token).status);
               Assert.AreEqual(403, Bearer_("POST", "/api/v1/sandbox/held/" + releaseId + "/release", reader.token, "{}").status);
               Assert.AreEqual(403, Bearer_("DELETE", "/api/v1/sandbox/held/" + discardId, reader.token).status);

               // The list is server-wide, like the queue: a key confined to named domains is refused all three.
               Assert.AreEqual(403, Bearer_("GET", "/api/v1/sandbox/held", confined.token).status);
               Assert.AreEqual(403, Bearer_("POST", "/api/v1/sandbox/held/" + releaseId + "/release", confined.token, "{}").status);
               Assert.AreEqual(403, Bearer_("DELETE", "/api/v1/sandbox/held/" + discardId, confined.token).status);

               Assert.AreEqual(2, Held_().GetProperty("count").GetInt32(), "The refusals changed nothing.");
            }
            finally
            {
               Admin("DELETE", "/api/v1/apikeys/" + reader.id, null);
               Admin("DELETE", "/api/v1/apikeys/" + confined.id, null);
            }

            (int status, string body) released = Admin("POST", "/api/v1/sandbox/held/" + releaseId + "/release", "{}");
            Assert.AreEqual(200, released.status, released.body);
            (int status, string body) discarded = Admin("DELETE", "/api/v1/sandbox/held/" + discardId, null);
            Assert.AreEqual(200, discarded.status, discarded.body);

            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1);
            string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, Password);
            StringAssert.Contains("sandbox release me", delivered);
            StringAssert.Contains("X-hMailServer-Sandbox: released by administrator", delivered);

            Assert.AreEqual(0, Held_().GetProperty("count").GetInt32(), "Both decisions end the hold.");
            Assert.AreEqual(2, sandbox.CountRequests("POST", "/submit"), "A release submits nothing again.");

            // Twice is not found.
            Assert.AreEqual(404, Admin("POST", "/api/v1/sandbox/held/" + releaseId + "/release", "{}").status);
            Assert.AreEqual(404, Admin("DELETE", "/api/v1/sandbox/held/" + discardId, null).status);

            List<JsonElement> trail = Audit_("sandbox hold");
            Assert.IsTrue(trail.Any(e => e.GetProperty("object").GetString() == releaseId.ToString() && e.GetProperty("action").GetString() == "released"),
               "The release is its own audit row.");
            Assert.IsTrue(trail.Any(e => e.GetProperty("object").GetString() == discardId.ToString() && e.GetProperty("action").GetString() == "discarded"),
               "The discard is its own audit row.");
         }
      }

      [Test]
      [Description("COM DeliveryQueue.Remove of a held message is the sandbox's discard, and is on the audit trail as one")]
      public void AComRemovalOfAHeldMessageIsAuditedAsADiscard()
      {
         using (FakeHttpEndpoint sandbox = NewSandbox_("job-com"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));
            Enable_(sandbox);

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox com remove", "three.exe", FileBytes_("com-remove")));

            long queueId = WaitForSubmitted_(1).GetProperty("messages")[0].GetProperty("queue_id").GetInt64();

            _application.GlobalObjects.DeliveryQueue.Remove(queueId);

            Assert.AreEqual(0, Held_().GetProperty("count").GetInt32(), "The held message is gone.");
            JsonElement row = Audit_("sandbox hold").FirstOrDefault(e => e.GetProperty("object").GetString() == queueId.ToString());
            Assert.AreEqual(JsonValueKind.Object, row.ValueKind, "The removal is on the audit trail.");
            Assert.AreEqual("discarded", row.GetProperty("action").GetString());
            StringAssert.Contains("DeliveryQueue.Remove", row.GetProperty("detail").GetString());
         }
      }

      // ---- the settings ------------------------------------------------------------

      [Test]
      [Description("The switch is refused on while an address is empty, over REST and COM, and taken with both addresses in one request; an address emptied while it is on ends the hold that was waiting with a header, holds nothing more, and the log says why at the next start")]
      public void TheSwitchNeedsBothAddressesAndAnEmptiedOneHoldsNothing()
      {
         dynamic settings = _settings;

         (int status, string body) alone = Admin("PUT", "/api/v1/settings", "{\"sandbox_enabled\":true}");
         Assert.AreEqual(400, alone.status, alone.body);
         StringAssert.Contains("cannot be on while both addresses are empty", alone.body);
         Assert.Throws<COMException>(() => { settings.SandboxEnabled = true; });

         using (FakeHttpEndpoint sandbox = NewSandbox_("job-w"))
         {
            sandbox.SetSequence("/verdict", (200, Pending));

            (int status, string body) together = Admin("PUT", "/api/v1/settings",
               "{\"sandbox_submit_url\":\"" + sandbox.UrlFor("/submit") + "\",\"sandbox_verdict_url\":\"" + sandbox.UrlFor("/verdict") + "\"," +
               "\"sandbox_enabled\":true,\"sandbox_poll_seconds\":5,\"sandbox_cache_hours\":0}");
            Assert.AreEqual(200, together.status, together.body);

            (int status, string body) emptied = Admin("PUT", "/api/v1/settings", "{\"sandbox_submit_url\":\"\"}");
            Assert.AreEqual(400, emptied.status, "REST does not leave the switch on with an address empty. " + emptied.body);

            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox waiting", "wait.exe", FileBytes_("waiting")));
            WaitForSubmitted_(1);

            // COM saves one property at a time, so it takes the address emptied;
            // what was waiting goes, saying it was not examined.
            settings.SandboxSubmitUrl = "";
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 1, TimeSpan.FromSeconds(30));
            StringAssert.Contains("X-hMailServer-Sandbox: not examined: the sandbox was turned off, or an address emptied",
               Messages_(_recipient.Address)[0]);

            // And nothing more is held while an address is empty.
            SmtpClientSimulator.StaticSendRaw(Outsider, _recipient.Address,
               MessageWithAttachment_(Outsider, _recipient.Address, "sandbox not held", "next.exe", FileBytes_("not-held")));
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, Password, 2, TimeSpan.FromSeconds(30));
            StringAssert.DoesNotContain("X-hMailServer-Sandbox", Messages_(_recipient.Address).First(m => m.Contains("sandbox not held")));
            Assert.AreEqual(1, sandbox.CountRequests("POST", "/submit"));

            LogHandler.DeleteCurrentDefaultLog();
            _application.Reinitialize();
            Assert.IsTrue(WaitFor_(() => LogHandler.ReadCurrentDefaultLog().Contains("SandboxEnabled is on, but SandboxSubmitUrl is empty"), 30),
               "The start says why nothing is held.");
         }
      }

      [Test]
      [Description("The settings over REST and COM: the same values both ways, the types list canonicalised, an address trimmed alike, the token never read back and refused with a line break or a space, the file cap and the size ranges, and an http address to a remote host, a timeout of 0 and a type with a slash refused on both")]
      public void TheSettingsRoundTripOverRestAndComAndRefuseWhatTheyMust()
      {
         dynamic settings = _settings;

         (int status, string body) put = Admin("PUT", "/api/v1/settings",
            "{\"sandbox_submit_url\":\" https://sandbox.example/submit \",\"sandbox_verdict_url\":\"http://127.0.0.1:" + SandboxPort + "/verdict\"," +
            "\"sandbox_types\":\"EXE; .Dll  application/PDF *.js exe\",\"sandbox_timeout_action\":\"bounce\",\"sandbox_timeout_minutes\":30," +
            "\"sandbox_token\":\"never-read-back\",\"sandbox_score_threshold\":60,\"sandbox_max_files\":5}");
         Assert.AreEqual(200, put.status, put.body);

         JsonElement group = Settings_();
         Assert.AreEqual("https://sandbox.example/submit", group.GetProperty("sandbox_submit_url").GetString(), "Stored trimmed, as COM stores it.");
         Assert.AreEqual("exe,dll,application/pdf,js", group.GetProperty("sandbox_types").GetString(), "Lower case, each once, the dots and stars dropped.");
         Assert.AreEqual("bounce", group.GetProperty("sandbox_timeout_action").GetString());
         Assert.AreEqual(30, group.GetProperty("sandbox_timeout_minutes").GetInt32());
         Assert.AreEqual(5, group.GetProperty("sandbox_max_files").GetInt32());
         ClassicAssert.IsFalse(group.TryGetProperty("sandbox_token", out _), "The token is write-only.");

         Assert.AreEqual("https://sandbox.example/submit", (string) settings.SandboxSubmitUrl);
         Assert.AreEqual("exe,dll,application/pdf,js", (string) settings.SandboxTypes);
         Assert.AreEqual(1, (int) settings.SandboxTimeoutAction);
         Assert.AreEqual(60, (int) settings.SandboxScoreThreshold);
         Assert.AreEqual(5, (int) settings.SandboxMaxFiles);

         settings.SandboxTypes = "7Z;Rar";
         Assert.AreEqual("7z,rar", Settings_().GetProperty("sandbox_types").GetString());

         // Refused over REST, in the setting's own words, with nothing changed.
         (int status, string body) remote = Admin("PUT", "/api/v1/settings", "{\"sandbox_submit_url\":\"http://sandbox.example/submit\"}");
         Assert.AreEqual(400, remote.status, remote.body);
         StringAssert.Contains("https", remote.body);
         (int status, string body) zero = Admin("PUT", "/api/v1/settings", "{\"sandbox_timeout_minutes\":0}");
         Assert.AreEqual(400, zero.status, zero.body);
         StringAssert.Contains("between 1 and 1440", zero.body);
         (int status, string body) slash = Admin("PUT", "/api/v1/settings", "{\"sandbox_types\":\"exe/bat\"}");
         Assert.AreEqual(400, slash.status, slash.body);
         StringAssert.Contains("neither an extension", slash.body);
         (int status, string body) lineBreak = Admin("PUT", "/api/v1/settings", "{\"sandbox_token\":\"abc\\r\\nX-Injected: 1\"}");
         Assert.AreEqual(400, lineBreak.status, lineBreak.body);
         StringAssert.Contains("visible ASCII", lineBreak.body);
         (int status, string body) tooLong = Admin("PUT", "/api/v1/settings", "{\"sandbox_token\":\"" + new string('t', 2049) + "\"}");
         Assert.AreEqual(400, tooLong.status, tooLong.body);
         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"sandbox_max_files\":0}").status);
         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"sandbox_max_size_kb\":81921}").status);

         group = Settings_();
         Assert.AreEqual("https://sandbox.example/submit", group.GetProperty("sandbox_submit_url").GetString());
         Assert.AreEqual(30, group.GetProperty("sandbox_timeout_minutes").GetInt32());
         Assert.AreEqual("7z,rar", group.GetProperty("sandbox_types").GetString());

         // And over COM.
         Assert.Throws<COMException>(() => { settings.SandboxSubmitUrl = "http://sandbox.example/submit"; });
         Assert.Throws<COMException>(() => { settings.SandboxTimeoutMinutes = 0; });
         Assert.Throws<COMException>(() => { settings.SandboxTypes = "exe/bat"; });
         Assert.Throws<COMException>(() => { settings.SandboxTimeoutAction = 2; });
         Assert.Throws<COMException>(() => { settings.SandboxMaxFiles = 101; });
         Assert.Throws<COMException>(() => { settings.SandboxMaxSizeKB = 81921; });
         Assert.Throws<COMException>(() => { settings.SetSandboxToken("two words"); });
         Assert.AreEqual("https://sandbox.example/submit", (string) settings.SandboxSubmitUrl);
         Assert.AreEqual(30, (int) settings.SandboxTimeoutMinutes);

         // Plain http to this machine is accepted, as the fixtures need it.
         settings.SandboxSubmitUrl = "http://127.0.0.1:" + SandboxPort + "/submit";
         settings.SandboxSubmitUrl = "http://localhost:" + SandboxPort + "/submit";
      }

      // ---- helpers ------------------------------------------------------------

      private static void Try_(Action step)
      {
         try
         {
            step();
         }
         catch (Exception ex)
         {
            TestContext.Progress.WriteLine("Sandbox teardown step failed: " + ex.Message);
         }
      }

      private void ResetSandbox_()
      {
         dynamic settings = _settings;
         settings.SandboxEnabled = false;
         settings.SandboxSubmitUrl = "";
         settings.SandboxVerdictUrl = "";
         settings.SetSandboxToken("");
         settings.SandboxTimeoutMinutes = 15;
         settings.SandboxTimeoutAction = 0;
         settings.SandboxMaxSizeKB = 20480;
         settings.SandboxTypes = DefaultTypes;
         settings.SandboxPollSeconds = 30;
         settings.SandboxScoreThreshold = 70;
         settings.SandboxCacheHours = 24;
         settings.SandboxInternalMail = false;
         settings.SandboxMaxFiles = 10;
      }

      // The addresses before the switch: it is refused on while either is empty.
      private void Enable_(FakeHttpEndpoint sandbox)
      {
         dynamic settings = _settings;
         settings.SandboxSubmitUrl = sandbox.UrlFor("/submit");
         settings.SandboxVerdictUrl = sandbox.UrlFor("/verdict");
         settings.SandboxPollSeconds = 5;
         settings.SandboxCacheHours = 0;
         settings.SandboxEnabled = true;
      }

      private static FakeHttpEndpoint NewSandbox_(string submissionId)
      {
         var sandbox = new FakeHttpEndpoint(SandboxPort, 200, Pending, "application/json");
         sandbox.SetResponse("/submit", 200, Encoding.UTF8.GetBytes("{\"id\":\"" + submissionId + "\"}"), "application/json");
         return sandbox;
      }

      private static string Done_(string verdict, int score, string detail)
         => "{\"state\":\"done\",\"verdict\":\"" + verdict + "\",\"score\":" + score + ",\"detail\":\"" + detail + "\"}";

      // Bytes that no other test sends, so that nothing a verdict cache remembered
      // from elsewhere answers for them - and bytes that no text encoding carries
      // unchanged: a NUL, the high half, every byte value once.
      private static byte[] FileBytes_(string tag)
      {
         var bytes = new List<byte> { 0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0xFF, 0xFE, 0x80, 0xC3, 0x28 };
         bytes.AddRange(Encoding.ASCII.GetBytes(" sandbox fixture file " + tag + " " + Guid.NewGuid().ToString("N") + " "));
         for (int i = 0; i < 256; i++)
            bytes.Add((byte) i);
         return bytes.ToArray();
      }

      private static string Sha256Hex_(byte[] bytes)
      {
         using (SHA256 sha = SHA256.Create())
            return string.Concat(sha.ComputeHash(bytes).Select(b => b.ToString("x2")));
      }

      // The file part of a submit's multipart body, as the bytes that arrived.
      private static byte[] FilePart_(byte[] body)
      {
         byte[] start = Encoding.ASCII.GetBytes("Content-Type: application/octet-stream\r\n\r\n");
         byte[] end = Encoding.ASCII.GetBytes("\r\n--hMailServerSandbox");

         int from = IndexOf_(body, start, 0);
         Assert.GreaterOrEqual(from, 0, "The submit has a file part.");
         from += start.Length;

         int to = IndexOf_(body, end, from);
         Assert.GreaterOrEqual(to, 0, "The file part ends at the boundary.");

         var part = new byte[to - from];
         Array.Copy(body, from, part, 0, part.Length);
         return part;
      }

      private static int IndexOf_(byte[] haystack, byte[] needle, int from)
      {
         for (int i = from; i <= haystack.Length - needle.Length; i++)
         {
            bool match = true;
            for (int j = 0; j < needle.Length && match; j++)
               match = haystack[i + j] == needle[j];
            if (match)
               return i;
         }

         return -1;
      }

      private static int CountOccurrences_(string text, string what)
      {
         int count = 0;
         for (int at = text.IndexOf(what, StringComparison.Ordinal); at >= 0; at = text.IndexOf(what, at + what.Length, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static string AttachmentPart_(string contentType, string fileName, byte[] content)
      {
         return "Content-Type: " + contentType + "; name=\"" + fileName + "\"\r\n" +
                "Content-Disposition: attachment; filename=\"" + fileName + "\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "\r\n" +
                Convert.ToBase64String(content, Base64FormattingOptions.InsertLineBreaks) + "\r\n";
      }

      private static string MessageWithAttachment_(string from, string to, string subject, string fileName, byte[] content,
                                                   string contentType = "application/octet-stream")
      {
         return MessageWithParts_(from, to, subject, AttachmentPart_(contentType, fileName, content));
      }

      private static string MessageWithParts_(string from, string to, string subject, params string[] parts)
      {
         return MessageWithBodyAndParts_(from, to, subject, "The body.", parts);
      }

      private static string MessageWithBodyAndParts_(string from, string to, string subject, string bodyText, params string[] parts)
      {
         var message = new StringBuilder();
         message.Append("From: " + from + "\r\n");
         message.Append("To: " + to + "\r\n");
         message.Append("Subject: " + subject + "\r\n");
         message.Append("MIME-Version: 1.0\r\n");
         message.Append("Content-Type: multipart/mixed; boundary=\"sandbox-boundary\"\r\n");
         message.Append("\r\n");
         message.Append("--sandbox-boundary\r\n");
         message.Append("Content-Type: text/plain\r\n");
         message.Append("\r\n");
         message.Append(bodyText + "\r\n");
         foreach (string part in parts)
         {
            message.Append("--sandbox-boundary\r\n");
            message.Append(part);
         }
         message.Append("--sandbox-boundary--\r\n");
         return message.ToString();
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private static (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private static (int status, string body) Bearer_(string method, string path, string token, string requestBody = null)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + token, requestBody);

      private static (string id, string token) CreateKey_(string requestBody)
      {
         (int status, string body) created = Admin("POST", "/api/v1/apikeys", requestBody);
         Assert.AreEqual(201, created.status, "POST /api/v1/apikeys must create a key. Body: " + created.body);
         JsonElement key = JsonDocument.Parse(created.body).RootElement;
         return (key.GetProperty("id").GetString(), key.GetProperty("key").GetString());
      }

      private static JsonElement Held_(string query = "")
      {
         (int status, string body) answer = Admin("GET", "/api/v1/sandbox/held" + query, null);
         Assert.AreEqual(200, answer.status, "GET /api/v1/sandbox/held: " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement;
      }

      private static JsonElement WaitForHeld_(int count)
      {
         JsonElement held = default;
         bool reached = WaitFor_(() =>
         {
            held = Held_();
            return held.GetProperty("count").GetInt32() == count;
         }, 30);

         Assert.IsTrue(reached, "Expected " + count + " message(s) held for the sandbox. Last answer: " + held);
         return held;
      }

      // Held, and every file handed to the sandbox: the delivery thread only
      // records a hold, and the poller submits within a second or so.
      private static JsonElement WaitForSubmitted_(int count)
      {
         JsonElement held = default;
         bool reached = WaitFor_(() =>
         {
            held = Held_();
            return held.GetProperty("count").GetInt32() == count &&
                   held.GetProperty("messages").EnumerateArray().All(m => m.GetProperty("files").EnumerateArray()
                      .All(f => f.GetProperty("state").GetString() != "not_submitted"));
         }, 40);

         Assert.IsTrue(reached, "Expected " + count + " message(s) held with every file submitted. Last answer: " + held);
         return held;
      }

      private static List<JsonElement> Audit_(string objectType)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/audit?limit=200&object_type=" + Uri.EscapeDataString(objectType), null);
         Assert.AreEqual(200, answer.status, answer.body);
         return JsonDocument.Parse(answer.body).RootElement.GetProperty("entries").EnumerateArray()
            .Where(e => e.GetProperty("object_type").GetString() == objectType)
            .Select(e => e.Clone()).ToList();
      }

      private static JsonElement Settings_()
      {
         (int status, string body) answer = Admin("GET", "/api/v1/settings", null);
         Assert.AreEqual(200, answer.status, answer.body);
         return JsonDocument.Parse(answer.body).RootElement;
      }

      private static JsonElement? QuarantineEntry_(string sender)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/quarantine", null);
         Assert.AreEqual(200, answer.status, answer.body);
         foreach (JsonElement entry in JsonDocument.Parse(answer.body).RootElement.EnumerateArray())
         {
            if (string.Equals(entry.GetProperty("sender").GetString(), sender, StringComparison.OrdinalIgnoreCase))
               return entry;
         }

         return null;
      }

      private static bool QuarantineHolds_(string sender) => QuarantineEntry_(sender).HasValue;

      private static int QuarantineCount_(string sender)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/quarantine", null);
         Assert.AreEqual(200, answer.status, answer.body);
         return JsonDocument.Parse(answer.body).RootElement.EnumerateArray()
            .Count(entry => string.Equals(entry.GetProperty("sender").GetString(), sender, StringComparison.OrdinalIgnoreCase));
      }

      private static void DeleteQuarantinedFrom_(string sender)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/quarantine", null);
         if (answer.status != 200)
            return;

         foreach (JsonElement entry in JsonDocument.Parse(answer.body).RootElement.EnumerateArray())
         {
            if (string.Equals(entry.GetProperty("sender").GetString(), sender, StringComparison.OrdinalIgnoreCase))
               Admin("DELETE", "/api/v1/quarantine/" + entry.GetProperty("id").GetInt64(), null);
         }
      }

      private static void DiscardEverythingHeld_()
      {
         (int status, string body) answer = Admin("GET", "/api/v1/sandbox/held?limit=1000", null);
         if (answer.status != 200)
            return;

         foreach (JsonElement message in JsonDocument.Parse(answer.body).RootElement.GetProperty("messages").EnumerateArray())
            Admin("DELETE", "/api/v1/sandbox/held/" + message.GetProperty("queue_id").GetInt64(), null);
      }

      private static List<string> Messages_(string address)
      {
         int count = new Pop3ClientSimulator().GetMessageCount(address, Password);
         var pop3 = new Pop3ClientSimulator();
         Assert.IsTrue(pop3.ConnectAndLogon(address, Password), "POP3 logon for " + address);

         var messages = new List<string>();
         for (int i = 1; i <= count; i++)
            messages.Add(pop3.RETR(i));

         pop3.QUIT();
         return messages;
      }

      private static bool WaitFor_(Func<bool> condition, int seconds)
      {
         DateTime deadline = DateTime.UtcNow.AddSeconds(seconds);

         while (DateTime.UtcNow < deadline)
         {
            if (condition())
               return true;

            Thread.Sleep(250);
         }

         return condition();
      }
   }
}
