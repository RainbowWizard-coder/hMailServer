// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    Compromised-account detection (CompromiseDetector.h): an authenticated account
   ///    that submits in the shape of a takeover - here, the two floors lowered to three
   ///    so that three messages to three strangers from an account with no baseline are
   ///    the shape - has the message that completed it kept out of the delivery queue,
   ///    listed at /api/v1/review with the reason, released from there to its recipient
   ///    or discarded; the account is locked out of SMTP AUTH and POP3 until unlocked,
   ///    by the compromise unlock alone or by the general one, and an unlock is a fresh
   ///    start; a bearer token is refused at every protocol while the account is locked;
   ///    a draft the webmail scheduled is judged when its minute comes and a released
   ///    webmail message gets its Sent copy; an API key can neither clear the lock nor
   ///    change a compromise_* setting; the account.compromised alert is raised, once an
   ///    hour per account; watch mode records the shape and holds nothing; an exempt
   ///    account is not judged; and the review PUT refuses what it does not take.
   /// </summary>
   [TestFixture]
   public class RestApiCompromisedAccount : TestFixtureBase
   {
      private static int RestPort = 9631;
      private const string AdminPassword = "testar";
      private const string Password = "Takeover-Passw0rd!";

      // The shipped values, put back after every test: watch, and floors no test in
      // the rest of the suite reaches.
      private const string Defaults = "{\"compromise_detection_mode\":\"watch\",\"compromise_volume_floor\":30,\"compromise_recipient_floor\":20}";

      private readonly List<string> _locked = new List<string>();

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         _locked.Clear();

         // Three messages to three strangers are the shape; the mode is each test's.
         (int status, string body) floors = Admin("PUT", "/api/v1/settings", "{\"compromise_volume_floor\":3,\"compromise_recipient_floor\":3,\"alerts_enabled\":true}");
         Assert.AreEqual(200, floors.status, floors.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // Whatever is still held is discarded, and every account this fixture
            // locked is let go, before the base fixture deletes the domain: the hold
            // on a name is in memory and the next fixture may make the same address.
            (int status, string body) held = Admin("GET", "/api/v1/review", null);
            if (held.status == 200)
            {
               foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(held.body), "messages").Cast<Dictionary<string, object>>())
                  Admin("DELETE", "/api/v1/review/" + Convert.ToInt64(entry["id"]), null);
            }

            foreach (string address in _locked)
               Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(address) + "/unlock", null);

            Admin("PUT", "/api/v1/settings", Defaults);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("In enforce mode the third message to a third stranger is held with the reason, the account is locked out of SMTP AUTH and POP3, the alert is raised; releasing delivers the message, and the compromise unlock alone lets the account back in.")]
      public void ATakeoverShapeIsHeldTheAccountLockedAndAReleaseDelivers()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"enforce\"}").status);

         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "takeover@" + _domain.Name, Password);
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "stranger1@" + _domain.Name, Password);
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "stranger2@" + _domain.Name, Password);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "stranger3@" + _domain.Name, Password);
         _locked.Add(sender.Address);

         // Before the shape: nothing held, not locked, no baseline.
         Dictionary<string, object> before = MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body);
         Assert.IsFalse(MiniJson.Flag(before, "locked"));
         Assert.IsFalse(MiniJson.Flag(before, "baseline.has_baseline"));
         Assert.AreEqual("enforce", MiniJson.Text(before, "mode"));

         Submit(sender.Address, first.Address, "Message 1");
         Submit(sender.Address, second.Address, "Message 2");
         Pop3ClientSimulator.AssertMessageCount(first.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(second.Address, Password, 1);

         // The third completes the shape. The client is answered 250 like any other.
         Submit(sender.Address, third.Address, "Message 3");

         (int status, string body) listed = Admin("GET", "/api/v1/review", null);
         Assert.AreEqual(200, listed.status, listed.body);
         Dictionary<string, object> list = MiniJson.Object(listed.body);
         Assert.AreEqual("enforce", MiniJson.Text(list, "mode"));
         List<Dictionary<string, object>> messages = MiniJson.List(list, "messages").Cast<Dictionary<string, object>>().ToList();
         Assert.AreEqual(1, messages.Count, listed.body);
         Dictionary<string, object> held = messages[0];
         Assert.AreEqual(sender.Address, (string) held["account"]);
         Assert.AreEqual("Message 3", (string) held["subject"]);
         Assert.AreEqual("smtp", (string) held["source"]);
         StringAssert.Contains(third.Address, (string) held["recipients"]);
         StringAssert.Contains("never written to", (string) held["reason"]);
         long id = Convert.ToInt64(held["id"]);

         // Locked: SMTP AUTH and POP3 refuse the right password.
         Assert.Throws<DeliveryFailedException>(() => Submit(sender.Address, first.Address, "Refused"), "A locked account cannot authenticate to submit.");
         Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password), "A locked account cannot log on to POP3.");

         Dictionary<string, object> state = MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body);
         Assert.IsTrue(MiniJson.Flag(state, "locked"));
         Assert.GreaterOrEqual(MiniJson.Number(state, "score"), 3);
         StringAssert.Contains("volume", MiniJson.Text(state, "signals"));
         StringAssert.Contains("recipients", MiniJson.Text(state, "signals"));
         Assert.AreEqual(3, MiniJson.Number(state, "baseline.history"), "Every submission is in the baseline, the held one included.");
         Assert.AreEqual(1, MiniJson.Number(state, "baseline.held"));

         // The alert, at once.
         Dictionary<string, object> alert = NewestAlert();
         Assert.IsNotNull(alert, "The account.compromised alert was raised.");
         StringAssert.Contains(sender.Address, (string) alert["summary"]);
         StringAssert.Contains("locked", (string) alert["summary"]);

         // Released: delivered to its recipient, and gone from the list. The account
         // stays locked until somebody says otherwise.
         (int status, string body) released = Admin("POST", "/api/v1/review/" + id + "/release", null);
         Assert.AreEqual(200, released.status, released.body);
         StringAssert.Contains("\"released\":true", released.body);
         StringAssert.Contains("Message 3", Pop3ClientSimulator.AssertGetFirstMessageText(third.Address, Password));
         Assert.AreEqual(0, MiniJson.List(MiniJson.Object(Admin("GET", "/api/v1/review", null).body), "messages").Count);
         Assert.AreEqual(404, Admin("POST", "/api/v1/review/" + id + "/release", null).status, "Released once.");
         Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password), "A release does not unlock the account.");

         // The compromise lock cleared alone.
         (int status, string body) unlocked = Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review/unlock", null);
         Assert.AreEqual(200, unlocked.status, unlocked.body);
         StringAssert.Contains("\"unlocked\":true", unlocked.body);
         Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password), "Unlocked, the account logs on again.");
         Dictionary<string, object> after = MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body);
         Assert.IsFalse(MiniJson.Flag(after, "locked"));
         Assert.Greater(MiniJson.Number(after, "unlocked_at"), 0, "The unlock is recorded on the account.");
         StringAssert.Contains("\"unlocked\":false", Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review/unlock", null).body, "Unlocking an unlocked account changes nothing.");

         // A fresh start: the surge that was reviewed does not count against the
         // next message, which is delivered rather than being the same shape again.
         Submit(sender.Address, first.Address, "After the unlock");
         Pop3ClientSimulator.AssertMessageCount(first.Address, Password, 2);
         Assert.AreEqual(0, MiniJson.List(MiniJson.Object(Admin("GET", "/api/v1/review", null).body), "messages").Count, "Nothing is held after the unlock.");
         Assert.IsFalse(MiniJson.Flag(MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body), "locked"), "The account is not locked again by its next message.");
         Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password));
      }

      [Test]
      [Description("In watch mode, the default, the same shape is recorded on the account and raises the alert saying what enforcing would have done, and nothing is held or locked.")]
      public void WatchModeRecordsTheShapeAndHoldsNothing()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"watch\"}").status);

         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "watched@" + _domain.Name, Password);
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "watched-one@" + _domain.Name, Password);
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "watched-two@" + _domain.Name, Password);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "watched-three@" + _domain.Name, Password);

         Submit(sender.Address, first.Address, "Watched 1");
         Submit(sender.Address, second.Address, "Watched 2");
         Submit(sender.Address, third.Address, "Watched 3");
         Pop3ClientSimulator.AssertMessageCount(third.Address, Password, 1);

         Assert.AreEqual(0, MiniJson.List(MiniJson.Object(Admin("GET", "/api/v1/review", null).body), "messages").Count, "Nothing is held in watch mode.");
         Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password), "Nothing is locked in watch mode.");

         Dictionary<string, object> state = MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body);
         Assert.IsFalse(MiniJson.Flag(state, "locked"));
         Assert.GreaterOrEqual(MiniJson.Number(state, "score"), 3, "The shape is written on the account.");
         StringAssert.Contains("never written to", MiniJson.Text(state, "reason"));

         Dictionary<string, object> alert = NewestAlert();
         Assert.IsNotNull(alert, "The alert is raised in watch mode too.");
         StringAssert.Contains("watch mode", (string) alert["summary"]);
         StringAssert.Contains("compromise_detection_mode", (string) alert["detail"]);
         Assert.Greater(MiniJson.Number(state, "alerted_at"), 0, "The alert is recorded on the account.");

         // Once an hour per account: every message after the shape completes scores
         // again for the rest of the hour, and none of them is a second alert.
         Submit(sender.Address, first.Address, "Watched 4");
         Submit(sender.Address, second.Address, "Watched 5");
         Pop3ClientSimulator.AssertMessageCount(second.Address, Password, 2);
         Assert.AreEqual(1, CountAlerts(sender.Address), "One alert for the surge, not one per message.");
         Assert.GreaterOrEqual(MiniJson.Number(MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body), "score"), 3, "The shape is still written on the account.");
      }

      [Test]
      [Description("A draft the webmail scheduled for later is judged when its minute comes: the one that completes the shape is held with source webmail, the next in the same run is held on the standing lock, and a release makes the Sent copy the send did not.")]
      public void AScheduledDraftIsJudgedLikeASendAndAReleasedWebmailMessageGetsItsSentCopy()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"enforce\"}").status);

         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "later@" + _domain.Name, Password);
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "later-one@" + _domain.Name, Password);
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "later-two@" + _domain.Name, Password);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "later-three@" + _domain.Name, Password);
         Account fourth = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "later-four@" + _domain.Name, Password);
         _locked.Add(sender.Address);

         // A folder for the copies the webmail keeps.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(sender.Address, Password));
         Assert.IsTrue(imap.CreateFolder("Sent"));
         imap.Disconnect();

         // One sent now, three scheduled for a moment from now. The one sent now is
         // the first submission, with its copy in Sent.
         (int status, string body) sent = User("POST", "/api/v1/me/messages", sender.Address, "{\"to\":\"" + first.Address + "\",\"subject\":\"Now\",\"text\":\"Sent now.\"}");
         Assert.AreEqual(201, sent.status, sent.body);
         StringAssert.Contains("\"held\":false", sent.body);
         Pop3ClientSimulator.AssertMessageCount(first.Address, Password, 1);

         // A second apart, so that the task runs them in this order.
         Account[] readers = { second, third, fourth };
         for (int i = 0; i < readers.Length; i++)
         {
            (int status, string body) draft = User("POST", "/api/v1/me/drafts", sender.Address, "{\"to\":\"" + readers[i].Address + "\",\"subject\":\"Later to " + readers[i].Address + "\",\"text\":\"Sent when the minute comes.\"}");
            Assert.AreEqual(201, draft.status, draft.body);
            long draftId = MiniJson.Number(MiniJson.Object(draft.body), "id");
            string at = DateTime.Now.AddSeconds(1 + i).ToString("yyyy-MM-dd HH:mm:ss");
            (int status, string body) scheduled = User("POST", "/api/v1/me/drafts/" + draftId + "/schedule", sender.Address, "{\"send_at\":\"" + at + "\"}");
            Assert.AreEqual(201, scheduled.status, scheduled.body);
         }

         // The minute comes. The second scheduled send is the third submission and
         // completes the shape; the third is submitted on the standing lock.
         Thread.Sleep(4500);
         (int status, string body) ran = Admin("POST", "/api/v1/scheduled/run", null);
         Assert.AreEqual(200, ran.status, ran.body);
         StringAssert.Contains("\"ran\":3", ran.body);

         Pop3ClientSimulator.AssertMessageCount(second.Address, Password, 1);

         List<Dictionary<string, object>> messages = MiniJson.List(MiniJson.Object(Admin("GET", "/api/v1/review", null).body), "messages").Cast<Dictionary<string, object>>().ToList();
         Assert.AreEqual(2, messages.Count, "The send that completed the shape and the one after it are held.");
         Dictionary<string, object> onLock = messages[0];
         Dictionary<string, object> completed = messages[1];
         Assert.AreEqual("webmail", (string) completed["source"]);
         Assert.AreEqual("webmail", (string) onLock["source"]);
         StringAssert.Contains(third.Address, (string) completed["recipients"]);
         StringAssert.Contains("never written to", (string) completed["reason"]);
         StringAssert.Contains(fourth.Address, (string) onLock["recipients"]);
         StringAssert.Contains("locked", (string) onLock["reason"]);
         Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password), "The account is locked by a scheduled send like any other.");

         // Released: delivered, and the Sent copy made now. Discarded: nothing.
         Assert.AreEqual(200, Admin("POST", "/api/v1/review/" + Convert.ToInt64(completed["id"]) + "/release", null).status);
         StringAssert.Contains("Later to " + third.Address, Pop3ClientSimulator.AssertGetFirstMessageText(third.Address, Password));
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/review/" + Convert.ToInt64(onLock["id"]), null).status);
         Pop3ClientSimulator.AssertMessageCount(fourth.Address, Password, 0);

         Assert.AreEqual(200, Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/unlock", null).status);
         Assert.IsTrue(imap.ConnectAndLogon(sender.Address, Password));
         Assert.AreEqual(3, imap.GetMessageCount("Sent"), "The send now, the scheduled send that went, and the released one; not the discarded one.");
         Assert.AreEqual(0, imap.GetMessageCount("Drafts"), "Every scheduled draft is gone.");
         imap.Disconnect();
      }

      [Test]
      [Description("A locked account is refused with a bearer token too, over SMTP, POP3 and IMAP, until it is unlocked.")]
      public void ATokenLogonIsRefusedWhileTheAccountIsLocked()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"enforce\"}").status);

         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bearer@" + _domain.Name, Password);
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bearer-one@" + _domain.Name, Password);
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bearer-two@" + _domain.Name, Password);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bearer-three@" + _domain.Name, Password);
         _locked.Add(sender.Address);

         try
         {
            EnableOAuth2();
            string token = MintHs256(sender.Address, UnixNow() + 3600);

            StringAssert.StartsWith("235", SmtpAuthXOAuth2(sender.Address, token), "The token signs the account in before it is locked.");
            StringAssert.StartsWith("+OK", Pop3AuthXOAuth2(sender.Address, token));
            StringAssert.Contains("A01 OK", ImapAuthXOAuth2(sender.Address, token));

            Submit(sender.Address, first.Address, "Bearer 1");
            Submit(sender.Address, second.Address, "Bearer 2");
            Submit(sender.Address, third.Address, "Bearer 3");
            Assert.IsTrue(MiniJson.Flag(MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body), "locked"));

            // The same valid token, refused at every protocol while the lock stands.
            StringAssert.StartsWith("535", SmtpAuthXOAuth2(sender.Address, token), "A locked account cannot sign in to SMTP with a token.");
            StringAssert.StartsWith("-ERR", Pop3AuthXOAuth2(sender.Address, token), "A locked account cannot sign in to POP3 with a token.");
            StringAssert.Contains("A01 NO", ImapAuthXOAuth2(sender.Address, token), "A locked account cannot sign in to IMAP with a token.");

            Assert.AreEqual(200, Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/unlock", null).status);
            StringAssert.StartsWith("235", SmtpAuthXOAuth2(sender.Address, token), "Unlocked, the token signs the account in again.");
            StringAssert.StartsWith("+OK", Pop3AuthXOAuth2(sender.Address, token));
            StringAssert.Contains("A01 OK", ImapAuthXOAuth2(sender.Address, token));
         }
         finally
         {
            DisableOAuth2();
         }
      }

      [Test]
      [Description("An API key of full scope is refused the compromise lock by the general unlock, which still clears the other two, and is refused every compromise_* setting; the administrator password clears and sets them.")]
      public void AnApiKeyCannotClearTheCompromiseLockNorBluntTheDetection()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"enforce\"}").status);

         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "keyed@" + _domain.Name, Password);
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "keyed-one@" + _domain.Name, Password);
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "keyed-two@" + _domain.Name, Password);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "keyed-three@" + _domain.Name, Password);
         _locked.Add(sender.Address);

         (int status, string body) created = Admin("POST", "/api/v1/apikeys", "{\"label\":\"compromise-fixture\",\"scope\":\"full\"}");
         Assert.AreEqual(201, created.status, created.body);
         Dictionary<string, object> key = MiniJson.Object(created.body);
         string keyId = Convert.ToString(key["id"]);
         string secret = (string) key["key"];

         try
         {
            Submit(sender.Address, first.Address, "Keyed 1");
            Submit(sender.Address, second.Address, "Keyed 2");
            Submit(sender.Address, third.Address, "Keyed 3");
            Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password));

            // The key's unlock clears the other two locks and says the compromise
            // lock was kept; the account stays locked.
            (int status, string body) keyed = Key("POST", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/unlock", secret, null);
            Assert.AreEqual(200, keyed.status, keyed.body);
            StringAssert.Contains("\"compromise_lock_cleared\":false", keyed.body);
            Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password), "An api key does not clear the compromise lock.");
            Assert.IsTrue(MiniJson.Flag(MiniJson.Object(Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review", null).body), "locked"));

            // Nor the review routes, nor the detection's settings.
            Assert.AreEqual(401, Key("GET", "/api/v1/review", secret, null).status);
            Assert.AreEqual(401, Key("POST", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/review/unlock", secret, null).status);
            Assert.AreEqual(403, Key("PUT", "/api/v1/settings", secret, "{\"compromise_detection_mode\":\"off\"}").status, "A key cannot switch the detection off.");
            Assert.AreEqual(403, Key("PUT", "/api/v1/settings", secret, "{\"compromise_volume_floor\":100000}").status, "A key cannot put the floors out of reach.");
            Assert.AreEqual(403, Key("PUT", "/api/v1/settings", secret, "{\"compromise_score_to_lock\":6,\"host_name\":\"keyed.test\"}").status, "Nothing in a body naming a compromise_* key is applied.");
            Assert.AreEqual("enforce", MiniJson.Text(MiniJson.Object(Admin("GET", "/api/v1/settings", null).body), "compromise_detection_mode"));

            // The administrator password does both.
            (int status, string body) unlocked = Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(sender.Address) + "/unlock", null);
            Assert.AreEqual(200, unlocked.status, unlocked.body);
            StringAssert.Contains("\"compromise_lock_cleared\":true", unlocked.body);
            Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(sender.Address, Password));
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_score_to_lock\":3}").status);
         }
         finally
         {
            Admin("DELETE", "/api/v1/apikeys/" + keyId, null);
         }
      }

      [Test]
      [Description("An exempt account is never judged; sensitivity is set and read back; a field the route does not take is refused; a held message is discarded rather than delivered, and the general unlock lets the account back in.")]
      public void AnExemptAccountIsNotJudgedAndADiscardedMessageIsGone()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"enforce\"}").status);

         Account newsletter = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "newsletter@" + _domain.Name, Password);
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "other@" + _domain.Name, Password);
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader1@" + _domain.Name, Password);
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader2@" + _domain.Name, Password);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader3@" + _domain.Name, Password);
         _locked.Add(other.Address);

         string reviewPath = "/api/v1/accounts/" + Uri.EscapeDataString(newsletter.Address) + "/review";
         Assert.AreEqual(400, Admin("PUT", reviewPath, "{\"bogus\":1}").status, "An unknown field is refused by name.");
         Assert.AreEqual(400, Admin("PUT", reviewPath, "{\"exempt\":\"yes\"}").status, "A wrong type is refused.");
         Assert.AreEqual(400, Admin("PUT", reviewPath, "{\"sensitivity\":\"paranoid\"}").status, "A sensitivity outside the three words is refused.");
         Assert.AreEqual(400, Admin("PUT", reviewPath, "{}").status, "Nothing to change is said so.");
         Assert.AreEqual(404, Admin("PUT", "/api/v1/accounts/nobody%40" + _domain.Name + "/review", "{\"exempt\":true}").status);

         (int status, string body) set = Admin("PUT", reviewPath, "{\"exempt\":true,\"sensitivity\":\"strict\"}");
         Assert.AreEqual(200, set.status, set.body);
         Dictionary<string, object> state = MiniJson.Object(Admin("GET", reviewPath, null).body);
         Assert.IsTrue(MiniJson.Flag(state, "exempt"));
         Assert.AreEqual("strict", MiniJson.Text(state, "sensitivity"));

         // Exempt: the shape that locks everybody else delivers all three.
         Submit(newsletter.Address, first.Address, "Issue 1");
         Submit(newsletter.Address, second.Address, "Issue 2");
         Submit(newsletter.Address, third.Address, "Issue 3");
         Pop3ClientSimulator.AssertMessageCount(third.Address, Password, 1);
         Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(newsletter.Address, Password), "An exempt account is not locked.");
         Assert.AreEqual(0, MiniJson.List(MiniJson.Object(Admin("GET", "/api/v1/review", null).body), "messages").Count);

         // The other account is judged, and its held message is discarded.
         Submit(other.Address, first.Address, "Other 1");
         Submit(other.Address, second.Address, "Other 2");
         Submit(other.Address, third.Address, "Other 3");

         List<Dictionary<string, object>> messages = MiniJson.List(MiniJson.Object(Admin("GET", "/api/v1/review", null).body), "messages").Cast<Dictionary<string, object>>().ToList();
         Assert.AreEqual(1, messages.Count);
         Assert.AreEqual(other.Address, (string) messages[0]["account"]);
         long id = Convert.ToInt64(messages[0]["id"]);

         (int status, string body) discarded = Admin("DELETE", "/api/v1/review/" + id, null);
         Assert.AreEqual(200, discarded.status, discarded.body);
         StringAssert.Contains("\"discarded\":true", discarded.body);
         Assert.AreEqual(404, Admin("DELETE", "/api/v1/review/" + id, null).status);
         Assert.AreEqual(0, MiniJson.List(MiniJson.Object(Admin("GET", "/api/v1/review", null).body), "messages").Count);

         // Nothing reached the third reader from the other account: its one message is the newsletter's.
         Pop3ClientSimulator.AssertMessageCount(third.Address, Password, 1);
         StringAssert.Contains("Issue 3", Pop3ClientSimulator.AssertGetFirstMessageText(third.Address, Password));

         // The general unlock clears every lock the account is behind, this one included.
         Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(other.Address, Password));
         Assert.AreEqual(200, Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(other.Address) + "/unlock", null).status);
         Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(other.Address, Password), "The general unlock lets the account back in.");
      }

      /// <summary>One authenticated submission over SMTP, as a mail client makes it.</summary>
      private static void Submit(string from, string to, string subject)
      {
         string error;
         new SmtpClientSimulator().Send(false, from, Password, from, to, subject, "The text of " + subject + ".", out error);
      }

      /// <summary>The newest account.compromised event, or null.</summary>
      private static Dictionary<string, object> NewestAlert()
      {
         (int status, string body) = Admin("GET", "/api/v1/alerts/events?limit=50", null);
         Assert.AreEqual(200, status, body);

         foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(body), "events").Cast<Dictionary<string, object>>())
         {
            if ((string) entry["condition"] == "account.compromised")
               return entry;
         }

         return null;
      }

      /// <summary>How many account.compromised events name the address.</summary>
      private static int CountAlerts(string address)
      {
         (int status, string body) = Admin("GET", "/api/v1/alerts/events?limit=200", null);
         Assert.AreEqual(200, status, body);

         return MiniJson.List(MiniJson.Object(body), "events").Cast<Dictionary<string, object>>()
            .Count(entry => (string) entry["condition"] == "account.compromised" && ((string) entry["summary"]).Contains(address));
      }

      // -- OAuth2 bearer tokens, as Security/OAuth2Bearer.cs mints and presents them --

      private const string HmacSecret = "compromise-fixture-oauth2-secret-0123456789";

      private void EnableOAuth2()
      {
         // Stored through the settings store, not written into hMailServer.ini: from
         // schema 6042 an edit to the file is undone at the next start.
         IniFileSetting.Write("OAuth2Enabled", "1");
         IniFileSetting.Write("OAuth2RequireTLS", "0");
         IniFileSetting.Write("OAuth2AllowedAlgorithms", "HS256");
         IniFileSetting.Write("OAuth2HmacSecret", HmacSecret);
         IniFileSetting.Write("OAuth2UsernameClaim", "email");
         IniFileSetting.Write("OAuth2Issuer", "");
         IniFileSetting.Write("OAuth2Audience", "");

         // IMAP AUTHENTICATE of every mechanism, XOAUTH2 included, is behind this
         // one switch (IMAPCommandAUTHENTICATE answers "IMAP AUTHENTICATE is not
         // enabled." without it), and the suite ships with it off. Put back below,
         // as Security/OAuth2Bearer.cs puts it back.
         _application.Settings.IMAPSASLPlainEnabled = true;
         _application.Reinitialize();
      }

      private void DisableOAuth2()
      {
         IniFileSetting.Write("OAuth2Enabled", "0");
         IniFileSetting.Write("OAuth2RequireTLS", "1");
         IniFileSetting.Write("OAuth2AllowedAlgorithms", "RS256");
         IniFileSetting.Write("OAuth2HmacSecret", "");
         IniFileSetting.Write("OAuth2UsernameClaim", "email");
         _application.Settings.IMAPSASLPlainEnabled = false;
         _application.Reinitialize();
      }

      private static string Base64Url(byte[] data)
      {
         return Convert.ToBase64String(data).TrimEnd('=').Replace('+', '-').Replace('/', '_');
      }

      private static string MintHs256(string email, long exp)
      {
         string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
         string payload = "{\"email\":\"" + email + "\",\"exp\":" + exp + "}";
         string signingInput = Base64Url(Encoding.UTF8.GetBytes(header)) + "." + Base64Url(Encoding.UTF8.GetBytes(payload));
         using (var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(HmacSecret)))
            return signingInput + "." + Base64Url(hmac.ComputeHash(Encoding.ASCII.GetBytes(signingInput)));
      }

      private static long UnixNow()
      {
         return (long) (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
      }

      private static string XOAuth2Response(string email, string token)
      {
         return Convert.ToBase64String(Encoding.ASCII.GetBytes("user=" + email + "auth=Bearer " + token + ""));
      }

      private static string SmtpAuthXOAuth2(string email, string token)
      {
         using (var con = new TcpConnection())
         {
            Assert.IsTrue(con.Connect(25), "Could not connect to SMTP.");
            con.ReadUntil("220");
            con.Send("EHLO example.test\r\n");
            con.ReadUntil("250 ");
            con.Send("AUTH XOAUTH2 " + XOAuth2Response(email, token) + "\r\n");
            return con.Receive();
         }
      }

      private static string Pop3AuthXOAuth2(string email, string token)
      {
         using (var con = new TcpConnection())
         {
            Assert.IsTrue(con.Connect(110), "Could not connect to POP3.");
            con.ReadUntil("+OK");
            con.Send("AUTH XOAUTH2 " + XOAuth2Response(email, token) + "\r\n");
            return con.Receive();
         }
      }

      private static string ImapAuthXOAuth2(string email, string token)
      {
         using (var con = new TcpConnection())
         {
            Assert.IsTrue(con.Connect(143), "Could not connect to IMAP.");
            con.ReadUntil("* OK");
            con.Send("A01 AUTHENTICATE XOAUTH2 " + XOAuth2Response(email, token) + "\r\n");
            return con.ReadUntil("A01 ");
         }
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      /// <summary>A request as the account itself, with its password.</summary>
      private static (int status, string body) User(string method, string path, string address, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic(address, Password), requestBody);
      }

      /// <summary>A request with an API key.</summary>
      private static (int status, string body) Key(string method, string path, string key, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + key, requestBody);
      }
   }
}
