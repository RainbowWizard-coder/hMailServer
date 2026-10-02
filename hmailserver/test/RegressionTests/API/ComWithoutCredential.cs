// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The COM objects that hold a live object without a credential behind them: a
   ///    Message or a FetchAccount a client creates itself in a process that holds no
   ///    Application signed in (where it does, the object answers with that Application's
   ///    credential - ComCallingProcess), and every object an Application handed out once
   ///    that Application has failed a later Authenticate (they share its credential, which
   ///    the failure empties). Since 6.3.6 such a holder may not save, copy, attach a file
   ///    from the server's disk, write an attachment to it, or point anything at an account -
   ///    outside the server's own event scripts, which API/Events.cs covers - and
   ///    Application.CreateMessage gives an authenticated client a message that carries its
   ///    credential.
   ///
   ///    Windows only - it is COM - and so linked into RegressionTests alone.
   /// </summary>
   [TestFixture]
   public class ComWithoutCredential : TestFixtureBase
   {
      private const string OwnerPassword = "Owner-Passw0rd!";
      private const string OtherPassword = "Other-Passw0rd!";

      private string _scratch;

      [SetUp]
      public void CreateScratchFolder()
      {
         _scratch = Path.Combine(Path.GetTempPath(), "hm-com-without-credential-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(_scratch);
      }

      [TearDown]
      public void RemoveScratchFolder()
      {
         try
         {
            Directory.Delete(_scratch, true);
         }
         catch (IOException)
         {
         }
         catch (UnauthorizedAccessException)
         {
         }
      }

      // With no administrator password every COM object answers as the server
      // administrator by design, so there is no holder without a credential to refuse.
      private static void RequireAnAdministratorPassword()
      {
         object administrator = new Application().Authenticate("Administrator", "");
         if (administrator != null)
            Assert.Ignore("This server has no administrator password, so COM answers anonymously as the " +
                          "server administrator and there is no holder without a credential to refuse.");
      }

      // Refused by the credential question, not by some other failure of the member. Each
      // member is tried and what was wrong is collected, so that a server without the
      // refusals reports every member it answered, not the first alone.
      private static void Try(List<string> problems, Action call, string member)
      {
         try
         {
            call();
            problems.Add(member + " was not refused");
         }
         catch (COMException refused)
         {
            if (!refused.Message.Contains("You do not have access"))
               problems.Add(member + " failed, but not as refused: " + refused.Message);
         }
      }

      private static void AssertNone(List<string> problems)
      {
         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));
      }

      private string WriteFile(string name, string content)
      {
         var path = Path.Combine(_scratch, name);
         File.WriteAllText(path, content);
         return path;
      }

      // A message with one text attachment, delivered over SMTP so that nothing about
      // its delivery depends on the COM members this fixture is about.
      private static void DeliverAMessageWithAnAttachment(string address, string password)
      {
         var attachment = Convert.ToBase64String(System.Text.Encoding.ASCII.GetBytes("attached by the sender"));
         var text = string.Join("\r\n", new[]
         {
            "From: someone@example.test",
            "To: " + address,
            "Subject: With an attachment",
            "MIME-Version: 1.0",
            "Content-Type: multipart/mixed; boundary=\"part-boundary\"",
            "",
            "--part-boundary",
            "Content-Type: text/plain; charset=us-ascii",
            "",
            "Body",
            "--part-boundary",
            "Content-Type: text/plain; name=\"attached.txt\"",
            "Content-Disposition: attachment; filename=\"attached.txt\"",
            "Content-Transfer-Encoding: base64",
            "",
            attachment,
            "--part-boundary--",
            ""
         });

         SmtpClientSimulator.StaticSendRaw("someone@example.test", address, text);
         Pop3ClientSimulator.AssertMessageCount(address, password, 1);
      }

      // What a script run as a process of its own printed for a member it was refused.
      private static void Refused(List<string> problems, string printed, string expected, string member)
      {
         if (!printed.Contains(expected))
            problems.Add(member + " was not refused: " + printed);
         else if (!printed.Contains("You do not have access"))
            problems.Add(member + " failed, but not as refused: " + printed);
      }

      [Test]
      [Description("A message a client creates itself, in a process that holds no Application signed in, carries " +
                   "no credential: outside an event script its Save and Attachments.Add are refused, and nothing it " +
                   "was given reaches anybody. Created by a script run as a process of its own, since this one holds " +
                   "the suite's signed-in Application.")]
      public void AMessageCreatedDirectlyIsRefusedItsSaveAndAFileFromTheServer()
      {
         RequireAnAdministratorPassword();
         var problems = new List<string>();

         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recipient@example.test", "test");
         var source = WriteFile("source.txt", "composed without a credential");

         Refused(problems, ComScripts.SendDirectly(_scratch, "-", "-", "someone@example.test", "-", recipient.Address,
            "Composed without a credential", source), "ADD-REFUSED", "Attachments.Add");
         Refused(problems, ComScripts.SendDirectly(_scratch, "-", "-", "someone@example.test", "-", recipient.Address,
            "Composed without a credential", "-"), "SAVE-REFUSED", "Message.Save");
         AssertNone(problems);

         // A message sent afterwards over SMTP is the only one that arrives.
         SmtpClientSimulator.StaticSend("someone@example.test", recipient.Address, "Control", "Control");
         Pop3ClientSimulator.AssertMessageCount(recipient.Address, "test", 1);
         StringAssert.Contains("Subject: Control", Pop3ClientSimulator.AssertGetFirstMessageText(recipient.Address, "test"));
      }

      [Test]
      [Description("A fetch account a client creates itself, in a process that holds no Application signed in, " +
                   "carries no credential: outside an event script it may not be pointed at an account, nor saved, " +
                   "and no fetch account appears on the account. Created by a script run as a process of its own.")]
      public void AFetchAccountCreatedDirectlyIsRefusedAnAccountAndItsSave()
      {
         RequireAnAdministratorPassword();
         var problems = new List<string>();

         var target = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "target@example.test", "test");

         var printed = ComScripts.FetchAccountDirectly(_scratch, "-", "-", target.ID);
         Refused(problems, printed, "ACCOUNTID-REFUSED", "FetchAccount.AccountID");
         Refused(problems, printed, "SAVE-REFUSED", "FetchAccount.Save");

         if (target.FetchAccounts.Count != 0)
            problems.Add("a fetch account was stored on the account");

         AssertNone(problems);
      }

      [Test]
      [Description("An Application that fails a later Authenticate empties the credential its objects share; " +
                   "those objects are then refused what an object with no credential is refused: an " +
                   "attachment written to the server's disk, a message copied, a fetch account moved to " +
                   "another account, an account saved.")]
      public void ObjectsOfAnApplicationThatFailedALaterAuthenticateAreRefused()
      {
         RequireAnAdministratorPassword();
         var problems = new List<string>();

         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "owner@example.test", OwnerPassword);
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "other@example.test", OtherPassword);
         var archive = owner.IMAPFolders.Add("Archive");

         DeliverAMessageWithAnAttachment(owner.Address, OwnerPassword);

         // The owner signs in on an Application of its own and takes its objects.
         var application = new Application();
         var account = application.Authenticate(owner.Address, OwnerPassword);
         ClassicAssert.IsNotNull(account, "The owner could not authenticate.");

         var message = CustomAsserts.AssertRetrieveFirstMessage(account.IMAPFolders.get_ItemByName("INBOX"));
         var attachment = message.Attachments[0];
         var fetchAccount = account.FetchAccounts.Add();

         // The same Application is now asked to sign in with a wrong password.
         ClassicAssert.IsNull(application.Authenticate(owner.Address, "not the password"));

         var written = Path.Combine(_scratch, "written.txt");
         Try(problems, () => attachment.SaveAs(written), "Attachment.SaveAs");
         if (File.Exists(written))
            problems.Add("the attachment was written to the server's disk");

         Try(problems, () => message.Copy(archive.ID), "Message.Copy");
         Try(problems, () => fetchAccount.AccountID = other.ID, "FetchAccount.AccountID");
         Try(problems, () => account.Save(), "Account.Save");

         if (other.FetchAccounts.Count != 0)
            problems.Add("a fetch account was moved to another account");

         AssertNone(problems);
      }

      [Test]
      [Description("An authenticated account that is not the server administrator may not attach a file from the " +
                   "server's disk or write an attachment to it, and still saves its own message.")]
      public void AnOrdinaryAccountIsRefusedTheServersDisk()
      {
         RequireAnAdministratorPassword();
         var problems = new List<string>();

         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "plain@example.test", OwnerPassword);
         var source = WriteFile("source.txt", "a file on the server's disk");

         DeliverAMessageWithAnAttachment(owner.Address, OwnerPassword);

         var application = new Application();
         var account = application.Authenticate(owner.Address, OwnerPassword);
         ClassicAssert.IsNotNull(account, "The account could not authenticate.");

         var message = CustomAsserts.AssertRetrieveFirstMessage(account.IMAPFolders.get_ItemByName("INBOX"));

         var written = Path.Combine(_scratch, "written.txt");
         Try(problems, () => message.Attachments[0].SaveAs(written), "Attachment.SaveAs");
         if (File.Exists(written))
            problems.Add("the attachment was written to the server's disk");

         Try(problems, () => message.Attachments.Add(source), "Attachments.Add");
         AssertNone(problems);

         // Its own message is still its own to save.
         message.set_Flag(eMessageFlag.eMFFlagged, true);
         message.Save();
      }

      [Test]
      [Description("Application.CreateMessage gives an authenticated client a message that carries its credential: " +
                   "the server administrator's attaches a file from the server's disk, is delivered, and its " +
                   "attachment is written back to disk.")]
      public void TheAdministratorsCreatedMessageIsSentWithAFile()
      {
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "created@example.test", "test");
         var source = WriteFile("created.txt", "attached through CreateMessage");

         var message = SingletonProvider<TestSetup>.Instance.CreateMessage();
         message.FromAddress = "someone@example.test";
         message.AddRecipient("", recipient.Address);
         message.Subject = "Created";
         message.Body = "Body";
         message.Attachments.Add(source);
         message.Save();

         var delivered = CustomAsserts.AssertRetrieveFirstMessage(recipient.IMAPFolders.get_ItemByName("INBOX"));
         ClassicAssert.AreEqual(1, delivered.Attachments.Count);

         var written = Path.Combine(_scratch, "written.txt");
         delivered.Attachments[0].SaveAs(written);
         ClassicAssert.AreEqual("attached through CreateMessage", File.ReadAllText(written));
      }
   }
}
