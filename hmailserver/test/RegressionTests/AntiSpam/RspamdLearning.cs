// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    Learning from the Junk folder with Rspamd as the backend (GitLab work item
   ///    #108, docs/JunkFolderLearning.md): a message a user moves into their Junk
   ///    folder is POSTed to the controller's /learnspam, one moved out of it to
   ///    /learnham, with the controller's password, the classifier and the mailbox
   ///    owner as Deliver-To and Rcpt.
   ///
   ///    Against a stand-in controller (FakeHttpEndpoint, on a loopback port the
   ///    system gives it), because the question is what was sent and when; the real
   ///    Rspamd is taught and read back by the Linux end-to-end run. And the settings:
   ///    their defaults, what they keep, and what they refuse.
   /// </summary>
   [TestFixture]
   public class RspamdLearning : TestFixtureBase
   {
      private const string Password = "test";
      private const string ControllerPassword = "c0ntroller-Pw";
      private const string Classifier = "bayes_user";
      private Account _account;
      private FakeHttpEndpoint _controller;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rspamd-learner@example.test", Password);
         _controller = new FakeHttpEndpoint(200, "{\"success\":true}");

         var antiSpam = _settings.AntiSpam;
         antiSpam.MoveLearningBackend = 1;
         antiSpam.RspamdControllerUrl = "http://127.0.0.1:" + _controller.Port;
         antiSpam.SetRspamdControllerPassword(ControllerPassword);
         antiSpam.RspamdClassifier = Classifier;
         antiSpam.RspamdTimeoutSeconds = 15;
         antiSpam.MoveLearningFromCopy = true;
      }

      [TearDown]
      public new void TearDown()
      {
         var antiSpam = _settings.AntiSpam;
         antiSpam.MoveLearningBackend = 0;
         antiSpam.MoveLearningFromCopy = true;
         antiSpam.RspamdControllerUrl = "";
         antiSpam.SetRspamdControllerPassword("");
         antiSpam.RspamdClassifier = "";
         antiSpam.RspamdTimeoutSeconds = 15;

         IniFileSetting.Delete("SpamAssassinLearnOnMove");
         _application.Reinitialize();
         antiSpam.SpamAssassinEnabled = false;

         if (_controller != null)
            _controller.Dispose();
      }

      // Delivers one message, creates the Junk folder, and returns a logged-in client
      // on the INBOX.
      private ImapClientSimulator DeliverOneAndOpenTheInbox(string subject = "Lesson")
      {
         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, subject, "Body text of the lesson.");
         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, 1);

         var client = new ImapClientSimulator();
         Assert.IsTrue(client.ConnectAndLogon(_account.Address, Password));
         Assert.IsTrue(client.CreateFolder("Junk"));
         Assert.IsTrue(client.SelectFolder("INBOX"));
         return client;
      }

      private List<string> WaitForRequests(int count, int timeoutSeconds = 20)
      {
         var watch = Stopwatch.StartNew();
         while (watch.Elapsed.TotalSeconds < timeoutSeconds)
         {
            var requests = _controller.Requests;
            if (requests.Count >= count)
               return requests;
            Thread.Sleep(100);
         }

         Assert.Fail(string.Format("The stand-in controller was sent {0} request(s), and {1} were expected.", _controller.Requests.Count, count));
         return null;
      }

      // Gives a lesson every chance to arrive, then makes sure there are only count.
      private void AssertNoMoreThan(int count)
      {
         Thread.Sleep(1500);
         Assert.AreEqual(count, _controller.Requests.Count, "No other lesson was sent to the controller.");
      }

      private void AssertLesson(string request, string endpoint)
      {
         StringAssert.StartsWith("POST " + endpoint + " HTTP/1.0\r\n", request, "The lesson is a POST to the controller's " + endpoint + ".");
         StringAssert.Contains("\r\nPassword: " + ControllerPassword + "\r\n", request, "The controller's password is sent.");
         StringAssert.Contains("\r\nClassifier: " + Classifier + "\r\n", request, "The classifier is named.");
         StringAssert.Contains("\r\nDeliver-To: " + _account.Address + "\r\n", request, "The user is the mailbox's owner.");
         StringAssert.Contains("\r\nRcpt: " + _account.Address + "\r\n", request, "The owner is the request's one recipient.");
         StringAssert.Contains("\r\nContent-Type: message/rfc822\r\n", request, "The body is the message.");
         StringAssert.DoesNotContain("Deliver-To: sender@example.test", request, "Never the sender.");
         StringAssert.Contains("Subject: Lesson", request, "The whole message, its header among it.");
         StringAssert.Contains("Body text of the lesson.", request, "The whole message, its body among it.");
      }

      [Test]
      [Description("A MOVE into Junk is a lesson of spam for the owner, POSTed to /learnspam with the password, the classifier and the owner; a MOVE back out a lesson of ham, to /learnham.")]
      public void MovingIntoAndOutOfJunkTeachesRspamdForTheOwner()
      {
         ImapClientSimulator client = DeliverOneAndOpenTheInbox();
         StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Junk"));
         AssertLesson(WaitForRequests(1)[0], "/learnspam");

         Assert.IsTrue(client.SelectFolder("Junk"));
         StringAssert.Contains("A11 OK", client.SendSingleCommand("A11 UID MOVE 1:* INBOX"));
         AssertLesson(WaitForRequests(2)[1], "/learnham");
         client.Disconnect();

         AssertNoMoreThan(2);
      }

      [Test]
      [Description("SetRspamdControllerPassword(\"\"), which the Control Panel's Remove the password calls, removes a stored password: the next lesson carries no Password header, and is otherwise the lesson it was.")]
      public void ARemovedPasswordIsNotSent()
      {
         _settings.AntiSpam.SetRspamdControllerPassword("");

         ImapClientSimulator client = DeliverOneAndOpenTheInbox();
         StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Junk"));
         client.Disconnect();

         string request = WaitForRequests(1)[0];
         StringAssert.StartsWith("POST /learnspam HTTP/1.0\r\n", request, "A move into Junk is still a lesson of spam.");
         StringAssert.Contains("\r\nClassifier: " + Classifier + "\r\n", request, "The classifier is still named.");
         StringAssert.Contains("\r\nDeliver-To: " + _account.Address + "\r\n", request, "The lesson is still the owner's.");
         string head = request.Substring(0, request.IndexOf("\r\n\r\n", StringComparison.Ordinal) + 2);
         StringAssert.DoesNotContain("\r\nPassword:", head, "With the password removed, the lesson carries none.");
         StringAssert.DoesNotContain(ControllerPassword, request, "The removed password is sent nowhere in the lesson.");

         AssertNoMoreThan(1);
      }

      [Test]
      [Description("A COPY into Junk teaches while MoveLearningFromCopy is on, as a COPY always did, and nothing while it is off.")]
      public void ACopyTeachesByTheSetting()
      {
         ImapClientSimulator client = DeliverOneAndOpenTheInbox();
         Assert.IsTrue(client.Copy(1, "Junk"), "COPY into Junk");
         AssertLesson(WaitForRequests(1)[0], "/learnspam");

         _settings.AntiSpam.MoveLearningFromCopy = false;
         Assert.IsTrue(client.Copy(1, "Junk"), "a second COPY into Junk");
         client.Disconnect();

         AssertNoMoreThan(1);
      }

      [Test]
      [Description("Nothing between two ordinary folders, nothing for a delete and an expunge in Junk, and nothing with the backend set back to SpamAssassin.")]
      public void NothingIsTaughtForAnOrdinaryMoveADeleteOrAnExpunge()
      {
         ImapClientSimulator client = DeliverOneAndOpenTheInbox();
         Assert.IsTrue(client.CreateFolder("Archive"));
         StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Archive"));
         AssertNoMoreThan(0);

         Assert.IsTrue(client.SelectFolder("Archive"));
         StringAssert.Contains("A11 OK", client.SendSingleCommand("A11 MOVE 1 Junk"));
         WaitForRequests(1);

         Assert.IsTrue(client.SelectFolder("Junk"));
         StringAssert.Contains("A12 OK", client.SendSingleCommand("A12 STORE 1 +FLAGS (\\Deleted)"));
         StringAssert.Contains("A13 OK", client.SendSingleCommand("A13 EXPUNGE"));
         StringAssert.Contains("(MESSAGES 0)", client.Status("Junk", "MESSAGES"));
         client.Disconnect();

         AssertNoMoreThan(1);
      }

      [Test]
      [Description("A controller that takes the connection and never answers costs the IMAP command nothing: the MOVE answers at once, and the lesson waits for the timeout off the IMAP thread.")]
      public void AControllerThatHangsDoesNotHoldTheMove()
      {
         _controller.SetStall("/learnspam", 8);
         _settings.AntiSpam.RspamdTimeoutSeconds = 3;

         ImapClientSimulator client = DeliverOneAndOpenTheInbox();
         var watch = Stopwatch.StartNew();
         StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Junk"));
         watch.Stop();
         client.Disconnect();

         Assert.Less(watch.Elapsed.TotalSeconds, 2.0, "The MOVE is answered without waiting for the controller.");
         WaitForRequests(1);

         // The lesson is given up on at the timeout, and logged; nothing is retried.
         Thread.Sleep(4000);
         Assert.AreEqual(1, _controller.Requests.Count, "Nothing is retried.");
      }

      [Test]
      [Description("With SpamAssassinLearnOnMove on and Rspamd chosen, a move teaches Rspamd and tells spamd nothing; set back to SpamAssassin, the next move tells spamd and teaches Rspamd nothing.")]
      public void OneBackendAtATime()
      {
         using (var spamd = new SpamdSimulator())
         {
            var antiSpam = _settings.AntiSpam;
            antiSpam.SpamMarkThreshold = 1;
            antiSpam.SpamDeleteThreshold = 10000;
            antiSpam.SpamAssassinEnabled = true;
            antiSpam.SpamAssassinHost = "127.0.0.1";
            antiSpam.SpamAssassinPort = spamd.Port;
            antiSpam.SpamAssassinMergeScore = false;
            antiSpam.SpamAssassinScore = 5;
            IniFileSetting.Write("SpamAssassinLearnOnMove", "1");
            _application.Reinitialize();

            ImapClientSimulator client = DeliverOneAndOpenTheInbox();
            spamd.WaitForRequest(0);

            StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Junk"));
            WaitForRequests(1);
            Thread.Sleep(1500);
            Assert.AreEqual(1, spamd.Requests.Count, "spamd had the delivery's scan and no TELL while Rspamd is chosen.");

            antiSpam.MoveLearningBackend = 0;
            Assert.IsTrue(client.SelectFolder("Junk"));
            StringAssert.Contains("A11 OK", client.SendSingleCommand("A11 MOVE 1 INBOX"));
            client.Disconnect();

            Dictionary<string, string> tell = spamd.WaitForRequest(1);
            StringAssert.StartsWith("TELL SPAMC/", tell["Request"]);
            Assert.AreEqual("ham", tell["Message-class"]);
            AssertNoMoreThan(1);
         }
      }

      [Test]
      [Description("With Rspamd chosen and no controller address, a move teaches nothing and says so: the application log for the lesson, HM8580 in the error log for the administrator.")]
      public void NoAddressTeachesNothingAndIsReported()
      {
         _settings.AntiSpam.RspamdControllerUrl = "";

         ImapClientSimulator client = DeliverOneAndOpenTheInbox();
         StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Junk"));
         client.Disconnect();

         CustomAsserts.AssertReportedError("HM8580", "RspamdControllerUrl");
         AssertNoMoreThan(0);
      }

      [Test]
      [Description("The settings: their defaults as shipped, what they keep, and the values they refuse; the password is write-only.")]
      public void TheSettingsKeepWhatTheyAreGivenAndRefuseTheRest()
      {
         var antiSpam = _settings.AntiSpam;

         // The defaults, as TearDown and TestSetup put them back.
         antiSpam.MoveLearningBackend = 0;
         antiSpam.RspamdControllerUrl = "";
         antiSpam.RspamdClassifier = "";
         Assert.AreEqual(0, antiSpam.MoveLearningBackend);
         Assert.IsTrue(antiSpam.MoveLearningFromCopy);
         Assert.AreEqual(15, antiSpam.RspamdTimeoutSeconds);

         antiSpam.MoveLearningBackend = 1;
         Assert.AreEqual(1, antiSpam.MoveLearningBackend);
         antiSpam.RspamdControllerUrl = "  https://rspamd.example.test/controller  ";
         Assert.AreEqual("https://rspamd.example.test/controller", antiSpam.RspamdControllerUrl, "Stored trimmed.");
         antiSpam.RspamdControllerUrl = "http://[::1]:11334";
         Assert.AreEqual("http://[::1]:11334", antiSpam.RspamdControllerUrl);
         antiSpam.RspamdClassifier = "bayes_user";
         Assert.AreEqual("bayes_user", antiSpam.RspamdClassifier);
         antiSpam.RspamdTimeoutSeconds = 300;
         Assert.AreEqual(300, antiSpam.RspamdTimeoutSeconds);
         antiSpam.MoveLearningFromCopy = false;
         Assert.IsFalse(antiSpam.MoveLearningFromCopy);

         Assert.Throws<COMException>(() => antiSpam.MoveLearningBackend = 2, "A backend that is neither is refused.");
         Assert.Throws<COMException>(() => antiSpam.RspamdControllerUrl = "http://rspamd.example.test:11334", "Plain http to another machine is refused.");
         Assert.Throws<COMException>(() => antiSpam.RspamdControllerUrl = "http://127.0.0.1:11334/?password=x", "A query is refused.");
         Assert.Throws<COMException>(() => antiSpam.RspamdControllerUrl = "https://user:pw@rspamd.example.test", "A password in the URL is refused.");
         Assert.Throws<COMException>(() => antiSpam.RspamdClassifier = "bayes user", "A classifier's name has no space.");
         Assert.Throws<COMException>(() => antiSpam.RspamdTimeoutSeconds = 0, "The timeout is at least a second.");
         Assert.Throws<COMException>(() => antiSpam.RspamdTimeoutSeconds = 301, "The timeout is at most 300 seconds.");
         Assert.Throws<COMException>(() => antiSpam.SetRspamdControllerPassword("two words"), "A password with a space is refused.");
         Assert.Throws<COMException>(() => antiSpam.SetRspamdControllerPassword(new string('x', 129)), "A password of more than 128 characters is refused.");

         Assert.AreEqual("http://[::1]:11334", antiSpam.RspamdControllerUrl, "A refused address changes nothing.");
         Assert.AreEqual(300, antiSpam.RspamdTimeoutSeconds, "A refused timeout changes nothing.");
      }
   }
}
