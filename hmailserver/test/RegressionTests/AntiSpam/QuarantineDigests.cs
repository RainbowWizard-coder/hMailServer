// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The quarantine digest: one message to an account that has something newly held
// for it, a link per entry that delivers it, one for the lot and one that stops
// the digest.
//
// Messages are held the way a person holds them - the account blocks a sender,
// the quarantine is on, and the sender's mail is kept rather than delivered - so
// every entry here has a real file behind it and a release really delivers.
// A digest pass is forced through AntiSpam.SendQuarantineDigests, which ignores
// the digest hour and the once-a-day rule and nothing else; the hour and the
// spread across it are the scheduler's and are not a thing a test can wait for.
//
// What these tests pin:
//
//   * a digest for an account with three held messages lists all three with a
//     link each, a link for the lot and a way to stop; nothing in it points
//     anywhere but this server, it is plain text, and an account with nothing
//     held gets no digest at all - nor does the same account again when nothing
//     new has been held;
//   * a release link asks before it acts (a GET changes nothing), works once,
//     is refused for an entry that is not the account's, and is refused once it
//     has expired - with the entry still held each time;
//   * the stop link, including the RFC 8058 one-click POST a mail client sends,
//     ends the digest for that account, and the webmail's switch brings it back;
//   * without the address the links point at, or with the domain's digest off,
//     nothing is sent.

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   [TestFixture]
   public class QuarantineDigests : TestFixtureBase
   {
      // 9581 is this fixture's port; SenderLists has 9580.
      private static int RestPort = 9581;
      private const string AdminPassword = "testar";
      private const string Password = "Digest-Tests-1!";
      private const string Pest = "pest@example.com";

      private string BaseUrl
      {
         get { return "http://127.0.0.1:" + RestPort; }
      }

      [SetUp]
      public void StartRestApiQuarantineAndDigest()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("QuarantineEnabled", "1");
         _application.Reinitialize();

         EmptyTheQuarantine();

         (int status, string body) probe = Http("GET", "/api/v1/status", Basic("Administrator", AdminPassword));
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         dynamic settings = _settings;
         settings.QuarantineDigestBaseUrl = BaseUrl;
         settings.QuarantineDigestLinkHours = 72;

         dynamic domain = _domain;
         domain.QuarantineDigest = true;
         domain.QuarantineDigestHour = 7;
         _domain.Save();
      }

      [TearDown]
      public void StopRestApiQuarantineAndDigest()
      {
         EmptyTheQuarantine();

         dynamic settings = _settings;
         settings.QuarantineDigestBaseUrl = "";

         IniFileSetting.Delete("QuarantineEnabled");
         RestListener.Stop();
         _application.Reinitialize();
      }

      // ---- the tests -------------------------------------------------------

      [Test]
      [Description("A digest for an account with three held messages lists all three, points only at this server, and no digest goes to an account with nothing held")]
      public void ADigestListsWhatWasHeldAndNoneGoesToAnAccountWithNothingHeld()
      {
         Account held = AddAccount("held");
         Account clean = AddAccount("clean");

         Hold(held, 3);

         ClassicAssert.AreEqual(1, SendDigests(), "One account has something held; one digest.");

         string digest = Pop3ClientSimulator.AssertGetFirstMessageText(held.Address, Password);

         StringAssert.Contains("Subject: 3 messages held for you", digest);
         for (int i = 1; i <= 3; i++)
            StringAssert.Contains("Held " + i, digest);

         ClassicAssert.AreEqual(3, Regex.Matches(digest, "Deliver: " + Regex.Escape(BaseUrl) + "/held/release\\?t=[0-9a-f]{64}").Count,
            "A link per held message.");
         StringAssert.Contains("To have all of them delivered", digest);
         StringAssert.Contains("List-Unsubscribe: <" + BaseUrl + "/held/stop?t=", digest);
         StringAssert.Contains("List-Unsubscribe-Post: List-Unsubscribe=One-Click", digest);
         StringAssert.Contains("Auto-Submitted: auto-generated", digest);
         StringAssert.Contains("Content-Type: text/plain", digest);

         // Nothing remote and nothing elsewhere: plain text has no image to load,
         // and every link in it is one of this server's two pages.
         StringAssert.DoesNotContain("<img", digest);
         StringAssert.DoesNotContain("text/html", digest);
         foreach (Match link in Regex.Matches(digest, "https?://[^\\s>]+"))
            StringAssert.StartsWith(BaseUrl + "/held/", link.Value, "Every link points at this server's digest pages.");

         Pop3ClientSimulator.AssertMessageCount(clean.Address, Password, 0);

         ClassicAssert.AreEqual(0, SendDigests(),
            "Nothing new has been held since, and a digest that repeats what the reader was shown is not sent.");
      }

      [Test]
      [Description("A release link asks before it acts, works once, refuses an entry that is not the account's, and stops working when it expires")]
      public void AReleaseLinkAsksFirstWorksOnceRefusesAnotherAccountsEntryAndExpires()
      {
         Account held = AddAccount("held");
         Account stranger = AddAccount("stranger");

         Hold(held, 3);
         ClassicAssert.AreEqual(1, SendDigests());

         string digest = Pop3ClientSimulator.AssertGetFirstMessageText(held.Address, Password);
         MatchCollection links = Regex.Matches(digest, "/held/release\\?t=([0-9a-f]{64})");
         ClassicAssert.AreEqual(4, links.Count, "Three entries and the lot. Digest: " + digest);

         string first = links[0].Groups[1].Value;
         string second = links[1].Groups[1].Value;
         string everything = links[3].Groups[1].Value;

         // A GET is a question: a mail scanner that follows every link must not
         // release anything, nor spend the token.
         (int status, string body) asked = Http("GET", "/held/release?t=" + first, null);
         ClassicAssert.AreEqual(200, asked.status, asked.body);
         StringAssert.Contains("Deliver this message to your inbox?", asked.body);
         StringAssert.Contains("method=\"post\"", asked.body);
         AssertHeld(3);

         (int status, string body) released = Post("/held/release", "t=" + first);
         ClassicAssert.AreEqual(200, released.status, released.body);
         StringAssert.Contains("On its way", released.body);
         Pop3ClientSimulator.AssertMessageCount(held.Address, Password, 1);
         AssertHeld(2);

         // Once.
         ClassicAssert.AreEqual(410, Post("/held/release", "t=" + first).status);
         ClassicAssert.AreEqual(410, Http("GET", "/held/release?t=" + first, null).status);

         // Another account's entry: a token that binds the stranger to one of the
         // held account's entries answers exactly as a token that names nothing.
         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         int heldEntry = quarantine[0].ID;
         string forged = new string('c', 64);

         _application.Database.ExecuteSQL(
            "insert into hm_digesttokens (dtaccountid, dtkind, dtquarantineid, dttoken, dtexpirestime, dtused, dtcreated) values (" +
            stranger.ID + ", 0, " + heldEntry + ", '" + forged + "', '2099-01-01 00:00:00', 0, '2026-01-01 00:00:00')");

         ClassicAssert.AreEqual(410, Http("GET", "/held/release?t=" + forged, null).status);
         ClassicAssert.AreEqual(410, Post("/held/release", "t=" + forged).status);
         AssertHeld(2);
         Pop3ClientSimulator.AssertMessageCount(stranger.Address, Password, 0);

         // Expired.
         _application.Database.ExecuteSQL("update hm_digesttokens set dtexpirestime = '2001-01-01 00:00:00' where dttoken = '" + second + "'");
         ClassicAssert.AreEqual(410, Http("GET", "/held/release?t=" + second, null).status);
         ClassicAssert.AreEqual(410, Post("/held/release", "t=" + second).status);
         AssertHeld(2);

         // Not a token at all.
         ClassicAssert.AreEqual(410, Post("/held/release", "t=zz").status);
         ClassicAssert.AreEqual(410, Post("/held/release", "t=" + new string('f', 64)).status);

         // The lot: everything the digest listed that is still held, and nothing
         // more - which includes the entry whose own link expired.
         (int status, string body) askedAll = Http("GET", "/held/release?t=" + everything, null);
         ClassicAssert.AreEqual(200, askedAll.status, askedAll.body);
         StringAssert.Contains("Deliver the 2 messages the digest listed?", askedAll.body);

         (int status, string body) releasedAll = Post("/held/release", "t=" + everything);
         ClassicAssert.AreEqual(200, releasedAll.status, releasedAll.body);
         StringAssert.Contains("On their way", releasedAll.body);
         AssertHeld(0);
         Pop3ClientSimulator.AssertMessageCount(held.Address, Password, 3);
      }

      [Test]
      [Description("The stop link ends the digest for the account, the one-click POST a mail client sends included, and the webmail's switch brings it back")]
      public void TheStopLinkEndsTheDigestAndTheWebmailBringsItBack()
      {
         Account held = AddAccount("held");

         Hold(held, 1);
         ClassicAssert.AreEqual(1, SendDigests());

         string digest = Pop3ClientSimulator.AssertGetFirstMessageText(held.Address, Password);
         StringAssert.Contains("Subject: 1 message held for you", digest);
         string stop = Regex.Match(digest, "/held/stop\\?t=([0-9a-f]{64})").Groups[1].Value;
         ClassicAssert.IsNotEmpty(stop, digest);

         (int status, string body) asked = Http("GET", "/held/stop?t=" + stop, null);
         ClassicAssert.AreEqual(200, asked.status, asked.body);
         StringAssert.Contains("Stop these digests?", asked.body);

         // RFC 8058: the token in the URL, List-Unsubscribe=One-Click as the body.
         (int status, string body) stopped = Post("/held/stop?t=" + stop, "List-Unsubscribe=One-Click");
         ClassicAssert.AreEqual(200, stopped.status, stopped.body);

         (int status, string body) setting = Http("GET", "/api/v1/me/quarantine-digest", Basic(held.Address, Password));
         StringAssert.Contains("\"available\":true", setting.body);
         StringAssert.Contains("\"enabled\":false", setting.body);

         Hold(held, 1, 2);
         ClassicAssert.AreEqual(0, SendDigests(), "The account asked to stop.");

         (int status, string body) back = Http("PUT", "/api/v1/me/quarantine-digest", Basic(held.Address, Password), "{\"enabled\":true}");
         ClassicAssert.AreEqual(200, back.status, back.body);
         StringAssert.Contains("\"enabled\":true", back.body);

         ClassicAssert.AreEqual(1, SendDigests());
         string again = Pop3ClientSimulator.AssertGetFirstMessageText(held.Address, Password);
         StringAssert.Contains("Subject: 1 message held for you", again, "Only what was held since the last digest.");
         StringAssert.Contains("Held 2", again);
      }

      [Test]
      [Description("Without the address the links point at, or with the domain's digest off, no digest is sent")]
      public void NoDigestIsSentWithoutItsAddressOrWithTheDomainsDigestOff()
      {
         Account held = AddAccount("held");
         Hold(held, 1);

         dynamic settings = _settings;
         settings.QuarantineDigestBaseUrl = "";
         ClassicAssert.AreEqual(0, SendDigests(), "A link nobody can follow is worse than no message.");

         (int status, string body) setting = Http("GET", "/api/v1/me/quarantine-digest", Basic(held.Address, Password));
         StringAssert.Contains("\"available\":false", setting.body);

         settings.QuarantineDigestBaseUrl = BaseUrl;
         dynamic domain = _domain;
         domain.QuarantineDigest = false;
         _domain.Save();
         ClassicAssert.AreEqual(0, SendDigests(), "Off by default and per domain.");

         Assert.Throws<System.Runtime.InteropServices.COMException>(() => { settings.QuarantineDigestBaseUrl = "mail.example.com"; },
            "An address that is not an absolute http or https URL would switch every digest off silently; it is refused instead.");

         Pop3ClientSimulator.AssertMessageCount(held.Address, Password, 0);
      }

      // ---- helpers ---------------------------------------------------------

      private Account AddAccount(string localPart)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, localPart + "@" + _domain.Name, Password);
      }

      private int SendDigests()
      {
         dynamic antiSpam = _settings.AntiSpam;
         return (int)antiSpam.SendQuarantineDigests();
      }

      /// <summary>
      /// Holds messages for the account the way the account itself would: it
      /// blocks a sender, and the sender's mail is kept. Subjects are "Held n",
      /// numbered from firstNumber.
      /// </summary>
      private void Hold(Account account, int count, int firstNumber = 1)
      {
         (int status, string body) blocked = Http("POST", "/api/v1/me/sender-list", Basic(account.Address, Password),
            "{\"value\":\"" + Pest + "\",\"blocked\":true}");
         ClassicAssert.IsTrue(blocked.status == 201 || blocked.status == 200, blocked.body);

         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         int before = quarantine.Count;

         for (int i = 0; i < count; i++)
            new SmtpClientSimulator().Send(Pest, account.Address, "Held " + (firstNumber + i), "Nothing wrong with this message but its sender.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         AssertHeld(before + count);
      }

      private void AssertHeld(int expected)
      {
         var quarantine = _settings.AntiSpam.Quarantine;
         DateTime until = DateTime.UtcNow.AddSeconds(30);

         while (true)
         {
            quarantine.Refresh();

            if (quarantine.Count == expected || DateTime.UtcNow > until)
               break;

            Thread.Sleep(100);
         }

         ClassicAssert.AreEqual(expected, quarantine.Count, "Messages held.");
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Post(string path, string formBody)
      {
         return Http("POST", path, null, formBody, "application/x-www-form-urlencoded");
      }

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null, string contentType = "application/json")
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (requestBody != null)
               {
                  head.Append("Content-Type: " + contentType + "\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               string[] first = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               int status = 0;
               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
