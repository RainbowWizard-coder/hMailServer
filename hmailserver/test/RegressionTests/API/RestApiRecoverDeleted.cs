// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    Recover deleted items (RecoverableStore.h): with recover_deleted_days set, a
   ///    message expunged over IMAP or deleted over POP3 is listed at
   ///    /api/v1/me/recoverable with its subject and folder, put back into that folder
   ///    with its flags, or deleted for good; a message moved to another folder is not
   ///    a deleted one; nothing is kept with the setting at 0, and the retention sweep
   ///    ends what the window no longer covers.
   /// </summary>
   [TestFixture]
   public class RestApiRecoverDeleted : TestFixtureBase
   {
      private static int RestPort = 9620;
      private const string AdminPassword = "testar";
      private const string Password = "Recover-Passw0rd!";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":0}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An expunged message is listed with its subject and folder, put back into the folder with its flags, and a moved one is not listed; a POP3 delete is kept too, and deleted for good on request.")]
      public void AnExpungedMessageIsPutBack()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":14}").status);
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recover@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Quarterly figures", "The numbers.");
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Moved not deleted", "Elsewhere.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, Password, 2);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(account.Address, Password));
         Assert.IsTrue(imap.CreateFolder("Archive"));
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         // The second is moved, which is not a delete.
         StringAssert.Contains("OK", imap.Send("A1 MOVE 2 Archive"));
         Assert.IsTrue(imap.SetSeenFlag(1));
         Assert.IsTrue(imap.SetDeletedFlag(1));
         Assert.IsTrue(imap.Expunge());
         imap.Disconnect();

         (int status, string body) listed = Send("GET", "/api/v1/me/recoverable", me, null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"days\":14", listed.body);
         StringAssert.Contains("\"subject\":\"Quarterly figures\"", listed.body);
         StringAssert.Contains("\"folder\":\"INBOX\"", listed.body);
         StringAssert.Contains("\"seen\":true", listed.body);
         StringAssert.DoesNotContain("Moved not deleted", listed.body, "A message moved to another folder is not a deleted one.");
         string id = Regex.Match(listed.body, "\"id\":(\\d+)").Groups[1].Value;

         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recover-other@" + _domain.Name, Password);
         Assert.AreEqual(404, Send("POST", "/api/v1/me/recoverable/" + id + "/restore", Basic(other.Address, Password), null).status, "Another account's item is not found.");

         (int status, string body) restored = Send("POST", "/api/v1/me/recoverable/" + id + "/restore", me, null);
         Assert.AreEqual(200, restored.status, restored.body);
         StringAssert.DoesNotContain("Quarterly figures", Send("GET", "/api/v1/me/recoverable", me, null).body, "A restored item leaves the list.");

         imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(account.Address, Password));
         Assert.AreEqual(1, imap.GetMessageCount("INBOX"), "The message is back in the INBOX.");
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         StringAssert.Contains("\\Seen", imap.GetFlags(1), "With its flags.");
         imap.Disconnect();

         // POP3: the DELE takes effect at QUIT, and is kept too.
         StringAssert.Contains("Quarterly figures", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, Password));
         (int status, string body) afterPop = Send("GET", "/api/v1/me/recoverable", me, null);
         StringAssert.Contains("\"subject\":\"Quarterly figures\"", afterPop.body, "A POP3 delete is kept.");
         string popId = Regex.Match(afterPop.body, "\"id\":(\\d+)").Groups[1].Value;

         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/recoverable/" + popId, me, null).status);
         StringAssert.DoesNotContain("Quarterly figures", Send("GET", "/api/v1/me/recoverable", me, null).body);
         Assert.AreEqual(404, Send("POST", "/api/v1/me/recoverable/" + popId + "/restore", me, null).status, "Deleted for good is gone.");
      }

      [Test]
      [Description("With recover_deleted_days at 0 nothing is kept, and the retention sweep ends what was kept once the setting no longer covers it, file and all.")]
      public void NothingIsKeptWhenOffAndTheSweepEndsWhatWas()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recover-sweep@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Not kept", "Body.");
         StringAssert.Contains("Not kept", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, Password));
         (int status, string body) off = Send("GET", "/api/v1/me/recoverable", me, null);
         StringAssert.Contains("\"items\":[]", off.body, "Nothing is kept with the setting off.");
         StringAssert.Contains("\"days\":0", off.body);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":14}").status);
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Kept a while", "Body.");
         StringAssert.Contains("Kept a while", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, Password));
         StringAssert.Contains("\"subject\":\"Kept a while\"", Send("GET", "/api/v1/me/recoverable", me, null).body);

         string kept = Path.Combine(_settings.Directories.DataDirectory, "Recoverable");
         Assert.IsTrue(Directory.Exists(kept) && Directory.GetFiles(kept, "*.eml", SearchOption.AllDirectories).Length >= 1, "The copy is on disk.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":0}").status);
         _application.Utilities.RunMessageRetention();
         StringAssert.Contains("\"items\":[]", Send("GET", "/api/v1/me/recoverable", me, null).body, "The sweep ends what the window no longer covers.");
         Assert.AreEqual(0, Directory.GetFiles(kept, "*.eml", SearchOption.AllDirectories).Length, "And its file.");
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
