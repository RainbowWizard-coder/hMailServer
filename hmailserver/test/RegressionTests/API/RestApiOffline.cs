// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
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
   ///    Offline in the webmail (RestApiOffline.cpp): the reader's choice of days lives
   ///    under the administrator's ceiling (reader_offline_days_max, 30 by default) and is
   ///    the offline_days preference; a ceiling lowered later clamps it, a ceiling of 0
   ///    offers nothing; and /api/v1/me/offline/messages lists what was received within
   ///    the window across every folder, by id from the oldest up, paged by after_id, with
   ///    the whole window's count and bytes on every page - and nothing older, nothing
   ///    marked deleted, and nothing of another account.
   /// </summary>
   [TestFixture]
   public class RestApiOffline : TestFixtureBase
   {
      private static int RestPort = 9641;
      private const string AdminPassword = "testar";
      private const string Password = "Offline-Passw0rd!";

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
            Admin("PUT", "/api/v1/settings", "{\"reader_offline_days_max\":30}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The choice is 0 until made, is saved as the offline_days preference, is refused above the ceiling, is clamped by a ceiling lowered later, and is nothing under a ceiling of 0.")]
      public void TheChoiceIsKeptUnderTheCeiling()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "offline@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         (int status, string body) window = Send("GET", "/api/v1/me/offline", me, null);
         Assert.AreEqual(200, window.status, window.body);
         StringAssert.Contains("\"days_max\":30", window.body, "The ceiling ships at thirty days.");
         StringAssert.Contains("\"days\":0", window.body, "Nothing is chosen until the reader chooses.");
         StringAssert.Contains("\"since\":\"\"", window.body);

         (int status, string body) chosen = Send("PUT", "/api/v1/me/offline", me, "{\"days\":14}");
         Assert.AreEqual(200, chosen.status, chosen.body);
         StringAssert.Contains("\"days\":14", chosen.body);
         StringAssert.IsMatch("\"since\":\"\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\"", chosen.body, "The window's start is a received time.");
         StringAssert.Contains("\"offline_days\":\"14\"", Send("GET", "/api/v1/me/preferences", me, null).body, "The choice is the offline_days preference.");

         (int status, string body) refused = Send("PUT", "/api/v1/me/offline", me, "{\"days\":45}");
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("\"days_max\":30", refused.body);
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/offline", me, "{\"days\":-1}").status);
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/offline", me, "{\"days\":\"soon\"}").status);
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/offline", me, "{}").status);
         StringAssert.Contains("\"days\":14", Send("GET", "/api/v1/me/offline", me, null).body, "A refused choice leaves the one before.");

         StringAssert.Contains("\"days\":7", Send("PUT", "/api/v1/me/offline", me, "{\"days\":\"7\"}").body, "A number written as a string is a number.");

         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"reader_offline_days_max\":400}").status, "The ceiling is at most 365.");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_offline_days_max\":5}").status);
         (int status, string body) clamped = Send("GET", "/api/v1/me/offline", me, null);
         StringAssert.Contains("\"days_max\":5", clamped.body);
         StringAssert.Contains("\"days\":5", clamped.body, "A choice above a ceiling lowered later counts as the ceiling.");
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/offline", me, "{\"days\":7}").status, "And cannot be chosen again.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_offline_days_max\":0}").status);
         (int status, string body) none = Send("GET", "/api/v1/me/offline", me, null);
         StringAssert.Contains("\"days_max\":0", none.body);
         StringAssert.Contains("\"days\":0", none.body, "A ceiling of 0 offers nothing.");
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/offline", me, "{\"days\":1}").status);
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/offline", me, "{\"days\":0}").status, "Nothing can still be chosen.");
         StringAssert.Contains("\"messages\":[]", Send("GET", "/api/v1/me/offline/messages", me, null).body, "And the listing is empty.");

         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "offline-other@" + _domain.Name, Password);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_offline_days_max\":30}").status);
         StringAssert.Contains("\"days\":0", Send("GET", "/api/v1/me/offline", Basic(other.Address, Password), null).body, "Another account's choice is its own.");
      }

      [Test]
      [Description("The listing carries what was received within the window in every folder, oldest id first, with the window's count and bytes, paged by after_id and limit; a message received before the window, one marked deleted, and another account's are not in it.")]
      public void TheListingIsWhatTheWindowHolds()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "offline-list@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/offline", me, "{\"days\":7}").status);

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Fresh", "Received today.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, Password, 1);

         // One received long before the window, and one received now in a folder of the reader's own.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(account.Address, Password));
         Assert.IsTrue(imap.CreateFolder("Projects"));
         string old = "From: a@example.com\r\nTo: " + account.Address + "\r\nSubject: Old\r\n\r\nLong ago.\r\n";
         StringAssert.Contains("A01 OK", imap.SendSingleCommandWithLiteral("A01 APPEND INBOX \"01-Jan-2020 12:30:00 +0000\" {" + old.Length + "}", old));
         string filed = "From: a@example.com\r\nTo: " + account.Address + "\r\nSubject: Filed\r\n\r\nIn a folder.\r\n";
         StringAssert.Contains("A02 OK", imap.SendSingleCommandWithLiteral("A02 APPEND Projects {" + filed.Length + "}", filed));

         (int status, string body) listed = Send("GET", "/api/v1/me/offline/messages", me, null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"days\":7", listed.body);
         StringAssert.Contains("\"count\":2", listed.body, "The fresh one and the filed one; not the old one.");
         StringAssert.Contains("\"more\":false", listed.body);
         StringAssert.DoesNotContain("\"received\":\"2020-", listed.body, "A message received before the window is not listed.");
         MatchCollection ids = Regex.Matches(listed.body, "\"id\":(\\d+)");
         Assert.AreEqual(2, ids.Count, listed.body);
         long first = long.Parse(ids[0].Groups[1].Value), second = long.Parse(ids[1].Groups[1].Value);
         Assert.Less(first, second, "Oldest id first.");
         StringAssert.IsMatch("\"bytes\":[1-9]\\d*", listed.body, "The window's size in bytes.");
         StringAssert.Contains("\"flags\":{\"seen\":", listed.body, "Each row carries its flags.");
         Assert.AreEqual(2, Regex.Matches(listed.body, "\"folder_id\":\\d+").Count, "And its folder.");

         (int status, string body) page = Send("GET", "/api/v1/me/offline/messages?limit=1", me, null);
         Assert.AreEqual(200, page.status, page.body);
         Assert.AreEqual(1, Regex.Matches(page.body, "\"id\":(\\d+)").Count);
         StringAssert.Contains("\"count\":2", page.body, "The count is the whole window's, whatever the page.");
         StringAssert.Contains("\"more\":true", page.body);
         StringAssert.Contains("\"next_after_id\":" + first, page.body);

         (int status, string body) rest = Send("GET", "/api/v1/me/offline/messages?after_id=" + first, me, null);
         Assert.AreEqual(200, rest.status, rest.body);
         StringAssert.Contains("\"id\":" + second, rest.body);
         StringAssert.DoesNotContain("\"id\":" + first + ",", rest.body, "after_id continues past the id given.");
         StringAssert.Contains("\"more\":false", rest.body);

         // Marked deleted but not yet expunged: no longer part of the window.
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         Assert.IsTrue(imap.SetDeletedFlag(1));
         StringAssert.Contains("\"count\":1", Send("GET", "/api/v1/me/offline/messages", me, null).body, "A message marked deleted is not listed.");
         imap.Disconnect();

         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "offline-list-other@" + _domain.Name, Password);
         string otherMe = Basic(other.Address, Password);
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/offline", otherMe, "{\"days\":7}").status);
         StringAssert.Contains("\"count\":0", Send("GET", "/api/v1/me/offline/messages", otherMe, null).body, "Another account sees none of it.");

         Assert.AreEqual(200, Send("PUT", "/api/v1/me/offline", me, "{\"days\":0}").status);
         (int status, string body) off = Send("GET", "/api/v1/me/offline/messages", me, null);
         StringAssert.Contains("\"days\":0", off.body);
         StringAssert.Contains("\"messages\":[]", off.body, "With nothing chosen the listing is empty.");
      }

      [Test]
      [Description("The choice is one of the account's hundred preferences: with all hundred taken by other keys it is refused with the preferences route's 400 and words, not a 500; a slot freed lets it in; and once it is among the hundred a new choice takes no new slot.")]
      public void TheChoiceTakesNoSlotPastTheCap()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "offline-cap@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         var hundred = new StringBuilder("{");
         for (int i = 1; i <= 100; i++)
            hundred.Append(i > 1 ? "," : "").Append("\"k").Append(i).Append("\":\"v\"");
         hundred.Append("}");
         (int status, string body) filled = Send("PUT", "/api/v1/me/preferences", me, hundred.ToString());
         Assert.AreEqual(200, filled.status, filled.body);

         (int status, string body) refused = Send("PUT", "/api/v1/me/offline", me, "{\"days\":7}");
         Assert.AreEqual(400, refused.status, "The hundred-and-first preference is the reader's to answer, not the server's: " + refused.body);
         StringAssert.Contains("at most 100 preferences", refused.body, "In the preferences route's words.");
         StringAssert.Contains("\"days\":0", Send("GET", "/api/v1/me/offline", me, null).body, "And nothing was chosen.");

         Assert.AreEqual(200, Send("PUT", "/api/v1/me/preferences", me, "{\"k100\":null}").status, "A slot freed.");
         (int status, string body) chosen = Send("PUT", "/api/v1/me/offline", me, "{\"days\":7}");
         Assert.AreEqual(200, chosen.status, chosen.body);
         StringAssert.Contains("\"days\":7", chosen.body);

         (int status, string body) again = Send("PUT", "/api/v1/me/offline", me, "{\"days\":14}");
         Assert.AreEqual(200, again.status, "A choice already among the hundred takes no new slot: " + again.body);
         StringAssert.Contains("\"days\":14", again.body);
         StringAssert.Contains("\"offline_days\":\"14\"", Send("GET", "/api/v1/me/preferences", me, null).body);
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
