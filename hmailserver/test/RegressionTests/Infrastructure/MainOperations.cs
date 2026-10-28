// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

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