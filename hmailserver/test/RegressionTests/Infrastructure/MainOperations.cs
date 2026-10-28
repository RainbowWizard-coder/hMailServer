// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System.Collections.Generic;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   [TestFixture]
   public class MainOperations : TestFixtureBase
   {
      [SetUp]
      public void Setup()
      {
         // The domain the base class's setup made is made again here: the one to add to.
         _domain = SingletonProvider<TestSetup>.Instance.PerformBasicSetup();
      }

      [Test]
      public void RestartServer()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         for (var i = 0; i < 5; i++)
         {
            application.Stop();

            application.Start();
         }
      }

      [Test]
      [Description("A message saved into an IMAP folder through COM with the servers stopped is stored and the call succeeds. Saving tells the notification server, which IMAP sessions subscribe to; until 6.3.8 the stop let go of it, so that the save stored the message and then read through a null pointer, and the call answered an error for a message it had kept.")]
      public void AMessageSavedIntoAFolderWithTheServersStoppedIsSavedWithoutAnError()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "stopped-save@example.test", "test");
         var inbox = account.IMAPFolders.get_ItemByName("INBOX");

         application.Stop();
         try
         {
            var message = inbox.Messages.Add();
            message.set_Flag(eMessageFlag.eMFSeen, true);
            message.Save();
         }
         finally
         {
            application.Start();
         }

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      [Description("The test of the connection to SpamAssassin, run through COM with the servers stopped, answers that the servers are stopped. The connection is made on the servers' TCP/IP service, which the stop lets go of; until 6.3.8 it was built on the empty pointer, and the call answered an error after an access violation.")]
      public void TheSpamAssassinConnectionTestWithTheServersStoppedSaysTheyAreStopped()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         bool connected;
         string resultText;
         application.Stop();
         try
         {
            connected = application.Settings.AntiSpam.TestSpamAssassinConnection("localhost", 783, out resultText);
         }
         finally
         {
            application.Start();
         }

         Assert.IsFalse(connected, resultText);
         StringAssert.Contains("The servers are stopped", resultText);
      }

      [Test]
      [Description("An external account's DownloadNow through COM with the servers stopped succeeds, and the account is fetched when they start. The fetch manager is let go of by the stop; until 6.3.8 the call was made through the empty pointer and answered an error after an access violation.")]
      public void DownloadNowWithTheServersStoppedFetchesTheAccountWhenTheyStart()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         var messages = new List<string>
         {
            "From: someone@example.com\r\n" +
            "To: stopped-fetch@example.test\r\n" +
            "Subject: Fetched after the start\r\n" +
            "\r\n" +
            "Hello.\r\n"
         };

         var port = TestSetup.GetNextFreePort();
         using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
         {
            pop3Server.StartListen();

            var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "stopped-fetch@example.test", "test");

            FetchAccount fetchAccount;
            application.Stop();
            try
            {
               fetchAccount = account.FetchAccounts.Add();
               fetchAccount.Enabled = true;
               fetchAccount.MinutesBetweenFetch = 10;
               fetchAccount.Name = "Stopped";
               fetchAccount.Username = "test@example.com";
               fetchAccount.Password = "test";
               fetchAccount.UseSSL = false;
               fetchAccount.ServerAddress = "127.0.0.1";
               fetchAccount.Port = port;
               fetchAccount.ProcessMIMERecipients = false;
               fetchAccount.DaysToKeepMessages = -1;
               fetchAccount.Save();

               fetchAccount.DownloadNow();
            }
            finally
            {
               application.Start();
            }

            pop3Server.WaitForCompletion();

            fetchAccount.Delete();

            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
         }
      }

      [Test]
      [Description("A clear of the delivery queue asked for through COM with the servers stopped reports that they are stopped and clears nothing, and a clear after the start clears the queue. The delivery manager is let go of by the stop; until 6.3.8 the clear read through the empty pointer, an access violation on the maintenance queue, and never said it was over, so that every clear asked for after it, after the start too, did nothing until the process ended.")]
      public void TheDeliveryQueueClearedWithTheServersStoppedSaysSoAndIsClearedAfterTheStart()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         _settings.SMTPNoOfTries = 3;
         _settings.SMTPMinutesBetweenTry = 60;

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "stopped-clear@example.test", "test");

         var deliveryResults = new Dictionary<string, int>();
         deliveryResults["queued@dummy-example.com"] = 250;

         var smtpServerPort = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, smtpServerPort))
         {
            // The first attempt fails, so the message stays queued for its next.
            server.AddRecipientResult(deliveryResults);
            server.SimulatedError = SimulatedErrorType.DisconnectAfterSessionStart;
            server.StartListen();

            TestSetup.AddRoutePointingAtLocalhost(5, smtpServerPort, false);

            var smtp = new SmtpClientSimulator();
            smtp.Send(sender.Address, new List<string> { "queued@dummy-example.com" }, "Queued", "Queued for a retry.");

            server.WaitForCompletion();
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(1, false);

         application.Stop();
         try
         {
            application.GlobalObjects.DeliveryQueue.Clear();

            CustomAsserts.AssertReportedError("HM4210", "the servers are stopped");
         }
         finally
         {
            application.Start();
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(1, false);

         application.GlobalObjects.DeliveryQueue.Clear();

         CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);
      }

      [Test]
      public void TestInternals()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         application.Utilities.RunTestSuite("I know what I am doing.");
      }

      [Test]
      public void TestLanguages()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         var oLanguageSwedish = application.GlobalObjects.Languages.get_ItemByName("swedish");
         var oLanguageEnglish = application.GlobalObjects.Languages.get_ItemByName("english");

         var sSWE = oLanguageSwedish.get_String("File");
         var sENG = oLanguageEnglish.get_String("File");

         Assert.AreEqual(sENG, "File");
         Assert.AreNotEqual(sSWE, sENG);
         Assert.AreNotEqual(sSWE, "");
      }
   }
}