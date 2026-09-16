// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    Auto-delete and auto-archive the reader chooses. Beside the administrator's
   ///    retention policy (Domain and Account MessageRetentionDays), an account holder
   ///    keeps three choices with the account, as preferences the webmail writes to
   ///    hm_accountprefs: retention_trash_days, retention_junk_days and
   ///    retention_archive_days. The same sweep the policy runs on -
   ///    MailboxRetentionTask, on demand through Utilities.RunMessageRetention - empties
   ///    the Trash and Junk folders of what is older than the choice and moves Inbox mail
   ///    older than the choice to the Archive folder, making that folder if there is none.
   ///
   ///    The preferences are written straight into the table here: the REST route that
   ///    keeps them (PUT /api/v1/me/preferences) is tested in its own fixture, and what
   ///    this one tests is what the server does with the rows.
   /// </summary>
   [TestFixture]
   public class ReaderRetentionTests : TestFixtureBase
   {
      private Account _account;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-retention@example.test", "test");

         // The policy off, so that nothing here is removed by anything but the choice.
         _domain.MessageRetentionDays = 0;
         _domain.Save();
      }

      [Test]
      [Description("Trash older than the reader's choice is emptied; what is newer, and what is in Junk with no choice for it, stays.")]
      public void TrashOlderThanTheChoiceIsEmptied()
      {
         IMAPFolder trash = _account.IMAPFolders.Add("Trash");
         trash.Save();
         IMAPFolder junk = _account.IMAPFolders.Add("Junk");
         junk.Save();

         MoveNewMessageTo(trash, "old rubbish");
         MoveNewMessageTo(junk, "old junk");
         AgeEveryMessage(40);
         MoveNewMessageTo(trash, "fresh rubbish");

         Choose("retention_trash_days", "30");

         ClassicAssert.AreEqual(1, _application.Utilities.RunMessageRetention(), "One message in Trash was older than thirty days.");

         ClassicAssert.AreEqual(1, Count(trash), "The fresh one stays.");
         ClassicAssert.AreEqual(1, Count(junk), "Junk has no choice and is not touched by Trash's.");
         StringAssert.Contains("fresh rubbish", FirstSubject(trash));
      }

      [Test]
      [Description("Inbox mail older than the reader's choice is moved to the Archive folder, which is made if it does not exist; newer mail stays where it is.")]
      public void InboxOlderThanTheChoiceIsArchived()
      {
         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, "last year", "It was a good year.");
         Pop3ClientSimulator.AssertMessageCount(_account.Address, "test", 1);
         AgeEveryMessage(400);
         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, "this week", "Still current.");
         Pop3ClientSimulator.AssertMessageCount(_account.Address, "test", 2);

         Choose("retention_archive_days", "365");

         ClassicAssert.AreEqual(0, _application.Utilities.RunMessageRetention(), "Archiving removes nothing: the message moves.");

         IMAPFolder inbox = _account.IMAPFolders.get_ItemByName("INBOX");
         ClassicAssert.AreEqual(1, Count(inbox), "The recent message stays in the Inbox.");
         StringAssert.Contains("this week", FirstSubject(inbox));

         IMAPFolder archive = _account.IMAPFolders.get_ItemByName("Archive");
         ClassicAssert.IsNotNull(archive, "The Archive folder was made for the move.");
         ClassicAssert.AreEqual(1, Count(archive));
         StringAssert.Contains("last year", FirstSubject(archive));
      }

      [Test]
      [Description("A choice that is not a number of days, or is out of range, is off: nothing is removed on the strength of it.")]
      public void AChoiceThatIsNotDaysIsOff()
      {
         IMAPFolder trash = _account.IMAPFolders.Add("Trash");
         trash.Save();

         MoveNewMessageTo(trash, "old rubbish");
         AgeEveryMessage(400);

         Choose("retention_trash_days", "soon");
         Choose("retention_junk_days", "0");
         Choose("retention_archive_days", "99999");

         ClassicAssert.AreEqual(0, _application.Utilities.RunMessageRetention());
         ClassicAssert.AreEqual(1, Count(trash));
         Pop3ClientSimulator.AssertMessageCount(_account.Address, "test", 0);
      }

      // ----------------------------------------------------------------- helpers

      /// <summary>Delivers a message to the Inbox and moves it into the folder, the way a client's move does.</summary>
      private void MoveNewMessageTo(IMAPFolder folder, string subject)
      {
         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, subject, "Body of " + subject);
         IMAPFolder inbox = _account.IMAPFolders.get_ItemByName("INBOX");
         Message message = WaitForFirstMessage(inbox);
         message.Copy(folder.ID);
         inbox.Messages.DeleteByDBID(message.ID);
      }

      private static Message WaitForFirstMessage(IMAPFolder folder)
      {
         for (int attempt = 0; attempt < 100; attempt++)
         {
            if (folder.Messages.Count > 0)
               return folder.Messages[0];
            System.Threading.Thread.Sleep(100);
         }
         Assert.Fail("No message arrived in " + folder.Name);
         return null;
      }

      private static int Count(IMAPFolder folder)
      {
         return folder.Messages.Count;
      }

      private static string FirstSubject(IMAPFolder folder)
      {
         return folder.Messages.Count > 0 ? folder.Messages[0].Subject : "";
      }

      /// <summary>The preference as the webmail would have written it.</summary>
      private void Choose(string name, string value)
      {
         _application.Database.ExecuteSQL("delete from hm_accountprefs where prefaccountid = " + _account.ID + " and prefname = '" + name + "'");
         _application.Database.ExecuteSQL("insert into hm_accountprefs (prefaccountid, prefname, prefvalue) values (" + _account.ID + ", '" + name + "', '" + value + "')");
      }

      /// <summary>
      ///    Sets the receipt time of every message the account holds back by the given
      ///    number of days. Called between deliveries, so the messages delivered before
      ///    it are old and the ones after it are not.
      /// </summary>
      private void AgeEveryMessage(int days)
      {
         var created = DateTime.Now.AddDays(-days).ToString("yyyy-MM-dd HH:mm:ss");
         _application.Database.ExecuteSQL("update hm_messages set messagecreatetime = '" + created + "' where messageaccountid = " + _account.ID);
      }
   }
}
