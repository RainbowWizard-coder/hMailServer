// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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
   ///    Message recall within the server (Common/Util/MessageRecall.h,
   ///    RestApiMessageRecall.cpp, docs/MessageRecall.md): an unread message sent
   ///    to accounts on this server can be withdrawn by its sender.
   ///
   ///    What is under test: that a webmail send to two local accounts and an
   ///    outside address is recalled from both unread copies - the POP3 counts,
   ///    the EXPUNGE an IMAP session with the folder selected sees, the outside
   ///    address counted as elsewhere, the Sent copy's $Recalled keyword, a second
   ///    call answering the same; that a copy marked \Seen stays and is answered
   ///    read while the unread other goes; that an authenticated SMTP submission
   ///    is recalled by the Message-ID the client wrote; that another account's
   ///    recall of that id is a 404 with nothing deleted; that an unauthenticated
   ///    submission from a local range in a local account's name is NOT recorded
   ///    (the negative control of the identity rule); that a distribution list's
   ///    two members are both recalled; that rows older than RecallDays are swept
   ///    by the retention task and answer 404 under a window that would otherwise
   ///    include them, while the same rows without the sweep answer 200; and that
   ///    an app password is refused with 403. Each with its negative control, so
   ///    a recall that stopped deleting, or started deleting what it must not,
   ///    fails here and not merely passes.
   /// </summary>
   [TestFixture]
   public class RestApiMessageRecall : TestFixtureBase
   {
      private static int RestPort = 9806;
      private const string AdminPassword = "testar";
      private const string Password = "Recall-Passw0rd!";

      private Account _sender;
      private Account _alice;
      private Account _bob;
      private Account _other;
      private string _me;
      private string _asOther;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recall_days\":14}").status);

         _sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recall-sender@" + _domain.Name, Password);
         _alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recall-alice@" + _domain.Name, Password);
         _bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recall-bob@" + _domain.Name, Password);
         _other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recall-other@" + _domain.Name, Password);
         _me = Basic(_sender.Address, Password);
         _asOther = Basic(_other.Address, Password);

         // The webmail keeps a copy of what it sends in the folder designated
         // \Sent, which a folder by that name is.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_sender.Address, Password));
         Assert.IsTrue(imap.CreateFolder("Sent"));
         imap.Disconnect();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings", "{\"recall_days\":14,\"recover_deleted_days\":0}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A webmail send to two local accounts and one outside address: both local copies unread; the recall deletes both (POP3 count 0, and an IMAP session that had SELECTed the folder sees the expunge), counts the outside one as elsewhere, marks the Sent copy $Recalled, and a second call answers the same outcomes. A message from outside in the same mailbox is not touched.")]
      public void AWebmailSendIsRecalledFromBothUnreadLocalCopies()
      {
         // The negative control in Bob's mailbox: a message from outside that
         // no recall may reach.
         SmtpClientSimulator.StaticSend("outsider@example.org", _bob.Address, "Stays", "Not the sender's to recall.");
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);

         long sentId;
         string messageId = SendFromWebmailToTwoLocalsAndOneOutside(out sentId);

         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 2);

         // Alice has the folder open in a client.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_alice.Address, Password));
         Assert.IsTrue(imap.SelectFolder("INBOX"));

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me,
            "{\"message_id\":\"" + messageId + "\",\"sent_id\":" + sentId + "}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"recalled\":2", recall.body);
         StringAssert.Contains("\"read\":0", recall.body);
         StringAssert.Contains("\"gone\":0", recall.body);
         StringAssert.Contains("\"elsewhere\":1", recall.body, "The outside address is counted from the Sent copy's To.");
         StringAssert.Contains("\"sent_marked\":true", recall.body);
         StringAssert.Contains("{\"address\":\"" + _alice.Address + "\",\"outcome\":\"recalled\"}", recall.body);
         StringAssert.Contains("{\"address\":\"" + _bob.Address + "\",\"outcome\":\"recalled\"}", recall.body);

         // The session that had the folder selected is told, as an EXPUNGE by
         // another client would tell it.
         string noop = imap.SendSingleCommand("A03 NOOP");
         StringAssert.Contains("EXPUNGE", noop, "The IMAP session with the folder selected did not see the expunge: " + noop);
         imap.Disconnect();

         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);

         // The Sent copy wears the keyword; that is the only record anybody gets.
         (int status, string body) sent = Send("GET", "/api/v1/me/messages/" + sentId, _me, null);
         Assert.AreEqual(200, sent.status, sent.body);
         StringAssert.Contains("$Recalled", sent.body, "The Sent copy carries $Recalled.");

         // Nothing was sent to the recipients or to the sender.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(_sender.Address, Password, 0);

         // Idempotent: the rows stay, marked, and the second call says the same,
         // the Sent copy still marked.
         (int status, string body) again = Send("POST", "/api/v1/me/recall", _me,
            "{\"message_id\":\"" + messageId + "\",\"sent_id\":" + sentId + "}");
         Assert.AreEqual(200, again.status, again.body);
         StringAssert.Contains("\"recalled\":2", again.body);
         StringAssert.Contains("\"gone\":0", again.body);
         StringAssert.Contains("\"sent_marked\":true", again.body);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);
      }

      [Test]
      [Description("A copy the recipient deleted before the recall is answered gone, and the other recipient's unread copy still goes; the rows stay marked and a second call answers the same.")]
      public void ACopyDeletedBeforeTheRecallIsGone()
      {
         long sentId;
         string messageId = SendFromWebmail("Gone before", _alice.Address + ", " + _bob.Address, out sentId);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_alice.Address, Password));
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         Assert.IsTrue(imap.SetDeletedFlag(1));
         Assert.IsTrue(imap.Expunge());
         imap.Disconnect();
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageId + "\",\"sent_id\":" + sentId + "}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"recalled\":1", recall.body);
         StringAssert.Contains("\"gone\":1", recall.body);
         StringAssert.Contains("\"read\":0", recall.body);
         StringAssert.Contains("{\"address\":\"" + _alice.Address + "\",\"outcome\":\"gone\"}", recall.body);
         StringAssert.Contains("{\"address\":\"" + _bob.Address + "\",\"outcome\":\"recalled\"}", recall.body);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 0);

         (int status, string body) again = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageId + "\"}");
         Assert.AreEqual(200, again.status, again.body);
         StringAssert.Contains("\"gone\":1", again.body);
         StringAssert.Contains("\"recalled\":1", again.body);
      }

      [Test]
      [Description("A copy the recipient moved or copied within their own mailbox is followed: Alice's MOVE to Archive is recalled from Archive, Bob's COPY leaves two unread copies and both go, and Carol's copy of a message she had read stays with both copies, answered read.")]
      public void AMovedOrCopiedCopyIsFollowed()
      {
         Account carol = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recall-carol@" + _domain.Name, Password);

         long sentId;
         string messageId = SendFromWebmail("Filed elsewhere", _alice.Address + ", " + _bob.Address + ", " + carol.Address, out sentId);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(carol.Address, Password, 1);

         var alice = new ImapClientSimulator();
         Assert.IsTrue(alice.ConnectAndLogon(_alice.Address, Password));
         Assert.IsTrue(alice.CreateFolder("Archive"));
         Assert.IsTrue(alice.SelectFolder("INBOX"));
         StringAssert.Contains("A1 OK", alice.SendSingleCommand("A1 MOVE 1 Archive"));
         Assert.AreEqual(0, alice.GetMessageCount("INBOX"));
         Assert.AreEqual(1, alice.GetMessageCount("Archive"));
         alice.Disconnect();

         var bob = new ImapClientSimulator();
         Assert.IsTrue(bob.ConnectAndLogon(_bob.Address, Password));
         Assert.IsTrue(bob.CreateFolder("Keep"));
         Assert.IsTrue(bob.SelectFolder("INBOX"));
         Assert.IsTrue(bob.Copy(1, "Keep"));
         Assert.AreEqual(1, bob.GetMessageCount("INBOX"));
         Assert.AreEqual(1, bob.GetMessageCount("Keep"));
         bob.Disconnect();

         var carolSession = new ImapClientSimulator();
         Assert.IsTrue(carolSession.ConnectAndLogon(carol.Address, Password));
         Assert.IsTrue(carolSession.CreateFolder("Keep"));
         Assert.IsTrue(carolSession.SelectFolder("INBOX"));
         Assert.IsTrue(carolSession.SetSeenFlag(1));
         Assert.IsTrue(carolSession.Copy(1, "Keep"));
         carolSession.Disconnect();

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageId + "\",\"sent_id\":" + sentId + "}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"recalled\":2", recall.body, "Per recipient: Alice and Bob.");
         StringAssert.Contains("\"read\":1", recall.body, "Carol had read it.");
         StringAssert.Contains("\"gone\":0", recall.body);
         StringAssert.Contains("{\"address\":\"" + _alice.Address + "\",\"outcome\":\"recalled\"}", recall.body);
         StringAssert.Contains("{\"address\":\"" + _bob.Address + "\",\"outcome\":\"recalled\"}", recall.body);
         StringAssert.Contains("{\"address\":\"" + carol.Address + "\",\"outcome\":\"read\"}", recall.body);

         alice = new ImapClientSimulator();
         Assert.IsTrue(alice.ConnectAndLogon(_alice.Address, Password));
         Assert.AreEqual(0, alice.GetMessageCount("Archive"), "The moved copy was recalled from where Alice filed it.");
         alice.Disconnect();

         bob = new ImapClientSimulator();
         Assert.IsTrue(bob.ConnectAndLogon(_bob.Address, Password));
         Assert.AreEqual(0, bob.GetMessageCount("INBOX"), "Both of Bob's unread copies went.");
         Assert.AreEqual(0, bob.GetMessageCount("Keep"));
         bob.Disconnect();

         carolSession = new ImapClientSimulator();
         Assert.IsTrue(carolSession.ConnectAndLogon(carol.Address, Password));
         Assert.AreEqual(1, carolSession.GetMessageCount("INBOX"), "Whoever has read it keeps it - every copy.");
         Assert.AreEqual(1, carolSession.GetMessageCount("Keep"));
         carolSession.Disconnect();
      }

      [Test]
      [Description("At most 200 recipient rows per sender per day are recorded: the send that crosses the line records what fits, the next records nothing, the application log says so once, and the recall of each says what was recorded.")]
      public void TheDaysCeilingRecordsTwoHundredAndSaysSoOnce()
      {
         const int accounts = 30;
         var members = new List<string>();
         for (int i = 0; i < accounts; i++)
            members.Add(SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recall-many" + i + "@" + _domain.Name, Password).Address);
         string to = string.Join(", ", members);

         // Six sends fill 180 rows; the seventh has room for 20 of its 30; the
         // eighth has room for none.
         var messageIds = new List<string>();
         var sentIds = new List<long>();
         for (int send = 0; send < 8; send++)
         {
            long sentId;
            messageIds.Add(SendFromWebmail("Bulk " + send, to, out sentId));
            sentIds.Add(sentId);
         }
         Pop3ClientSimulator.AssertMessageCount(members[0], Password, 8);
         Pop3ClientSimulator.AssertMessageCount(members[accounts - 1], Password, 8);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         const string ceilingLine = "which is the most that is recorded";
         string log = LogHandler.ReadCurrentDefaultLog();
         Assert.AreEqual(1, Regex.Matches(log, Regex.Escape(ceilingLine)).Count, "The ceiling is said once per sender per day, not once per message: " + log);

         (int status, string body) sixth = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageIds[5] + "\"}");
         Assert.AreEqual(200, sixth.status, sixth.body);
         StringAssert.Contains("\"recalled\":" + accounts + ",", sixth.body, "The sixth send was recorded in full.");

         (int status, string body) seventh = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageIds[6] + "\"}");
         Assert.AreEqual(200, seventh.status, seventh.body);
         StringAssert.Contains("\"recalled\":20,", seventh.body, "The seventh send was recorded up to the ceiling and no further.");

         (int status, string body) eighth = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageIds[7] + "\"}");
         Assert.AreEqual(404, eighth.status, "Past the ceiling nothing is recorded: " + eighth.body);

         Pop3ClientSimulator.AssertMessageCount(members[accounts - 1], Password, 7, TimeSpan.FromSeconds(5));
      }

      [Test]
      [Description("A submission without a Message-ID is given one at acceptance and is recallable by it; one with a Message-ID over 255 characters is delivered but not recorded, the debug log says so, and a recall by the long id is refused with a sentence.")]
      public void AMessageWithoutAUsableMessageIdIsNotRecorded()
      {
         // No Message-ID: the submission server writes one (RFC 6409), and the
         // rows are keyed by it. It is read back from the delivered copy.
         new SmtpClientSimulator().SendRaw(_sender.Address, Password, _sender.Address, _alice.Address,
            "From: " + _sender.Address + "\r\nTo: " + _alice.Address + "\r\nSubject: No id of its own\r\n\r\nThe server gives it one.\r\n");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         string prefix = "/api/v1/accounts/" + Uri.EscapeDataString(_alice.Address);
         (int status, string body) listed = Admin("GET", prefix + "/messages", null);
         Assert.AreEqual(200, listed.status, listed.body);
         long deliveredId = long.Parse(Regex.Match(listed.body, "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) source = Admin("GET", prefix + "/messages/" + deliveredId + "/source", null);
         Assert.AreEqual(200, source.status, source.body);
         string given = Regex.Match(source.body, "Message-ID:\\s*<([^>]+)>").Groups[1].Value;
         Assert.IsFalse(string.IsNullOrEmpty(given), "The submission was given a Message-ID: " + source.body);

         (int status, string body) byGiven = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + given + ">\"}");
         Assert.AreEqual(200, byGiven.status, byGiven.body);
         StringAssert.Contains("\"recalled\":1", byGiven.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);

         // Over 255 characters: delivered, not recorded, the log saying why,
         // and the route refusing the id with a sentence.
         string longId = new string('x', 300) + "@recall.test";
         new SmtpClientSimulator().SendRaw(_sender.Address, Password, _sender.Address, _alice.Address,
            "From: " + _sender.Address + "\r\nTo: " + _alice.Address + "\r\nSubject: Too long an id\r\nMessage-ID: <" + longId + ">\r\n\r\nNothing to recall this by.\r\n");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Assert.IsTrue(LogHandler.DefaultLogContains("has no Message-ID, or one longer than 255 characters, and is not recallable"), "The log says why: " + LogHandler.ReadCurrentDefaultLog());
         Assert.AreEqual(1, Regex.Matches(LogHandler.ReadCurrentDefaultLog(), "is not recallable").Count, "Said for the long id and for nothing else.");

         (int status, string body) tooLong = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + longId + ">\"}");
         Assert.AreEqual(400, tooLong.status, tooLong.body);
         StringAssert.Contains("255", tooLong.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         // The control: an ordinary id, recorded and recalled.
         SendAuthenticated(_sender, _alice.Address, "ordinary-id@recall.test", "An ordinary id");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 2);
         (int status, string body) ordinary = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<ordinary-id@recall.test>\"}");
         Assert.AreEqual(200, ordinary.status, ordinary.body);
         StringAssert.Contains("\"recalled\":1", ordinary.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
      }

      [Test]
      [Description("The identity rule's other half: a session signed in as the sender whose MAIL FROM is another local account's address is credited to the login - the sender recalls it, the address named in the envelope cannot.")]
      public void AnAuthenticatedSessionIsCreditedWhateverTheEnvelopeSays()
      {
         const string messageId = "envelope-someone-else@recall.test";
         new SmtpClientSimulator().SendRaw(_sender.Address, Password, _bob.Address, _alice.Address,
            "From: " + _bob.Address + "\r\nTo: " + _alice.Address + "\r\nSubject: In Bob's name\r\nMessage-ID: <" + messageId + ">\r\n\r\nSigned in as the sender, sent as Bob.\r\n");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         string asBob = Basic(_bob.Address, Password);
         (int status, string body) bobs = Send("POST", "/api/v1/me/recall", asBob, "{\"message_id\":\"<" + messageId + ">\"}");
         Assert.AreEqual(404, bobs.status, "The envelope named Bob; the session was the sender's: " + bobs.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         (int status, string body) senders = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + messageId + ">\"}");
         Assert.AreEqual(200, senders.status, senders.body);
         StringAssert.Contains("\"recalled\":1", senders.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);
      }

      [Test]
      [Description("With the recover-deleted window on, a recalled copy is kept aside for its recipient, who can put it back; with the window off nothing is kept.")]
      public void ARecalledCopyReachesTheRecoverDeletedStore()
      {
         string asAlice = Basic(_alice.Address, Password);

         // The window off: nothing is kept.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":0}").status);
         SendAuthenticated(_sender, _alice.Address, "not-kept@recall.test", "Not kept");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Assert.AreEqual(200, Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<not-kept@recall.test>\"}").status);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);
         (int status, string body) nothing = Send("GET", "/api/v1/me/recoverable", asAlice, null);
         Assert.AreEqual(200, nothing.status, nothing.body);
         StringAssert.DoesNotContain("Not kept", nothing.body);

         // The window on: the recalled copy is Alice's to put back.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recover_deleted_days\":14}").status);
         SendAuthenticated(_sender, _alice.Address, "kept-aside@recall.test", "Kept aside");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<kept-aside@recall.test>\"}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"recalled\":1", recall.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);

         (int status, string body) kept = Send("GET", "/api/v1/me/recoverable", asAlice, null);
         Assert.AreEqual(200, kept.status, kept.body);
         StringAssert.Contains("\"subject\":\"Kept aside\"", kept.body, "The recalled copy is in Alice's recover-deleted list.");
         string id = Regex.Match(kept.body, "\"id\":(\\d+)").Groups[1].Value;
         Assert.AreEqual(200, Send("POST", "/api/v1/me/recoverable/" + id + "/restore", asAlice, null).status);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
      }

      [Test]
      [Description("An address at one of this server's domain aliases is on this server: it resolves to the account, is recalled, and is not counted as elsewhere.")]
      public void AnAddressAtADomainAliasIsHere()
      {
         DomainAlias alias = _domain.DomainAliases.Add();
         alias.AliasName = "recall-alias.test";
         alias.Save();

         long sentId;
         string messageId = SendFromWebmail("Via the alias name", "recall-alice@recall-alias.test", out sentId);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageId + "\",\"sent_id\":" + sentId + "}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"recalled\":1", recall.body);
         StringAssert.Contains("\"elsewhere\":0", recall.body, "The alias name is this server's.");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);
      }

      [Test]
      [Description("One copy marked \\Seen over IMAP before the recall stays and is reported read; the unread other goes.")]
      public void AReadCopyStaysAndIsReportedRead()
      {
         long sentId;
         string messageId = SendFromWebmail("Read by one", _alice.Address + ", " + _bob.Address, out sentId);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_alice.Address, Password));
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         Assert.IsTrue(imap.SetSeenFlag(1), "Alice reads the message.");
         imap.Disconnect();

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me,
            "{\"message_id\":\"" + messageId + "\",\"sent_id\":" + sentId + "}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"recalled\":1", recall.body);
         StringAssert.Contains("\"read\":1", recall.body);
         StringAssert.Contains("\"elsewhere\":0", recall.body);
         StringAssert.Contains("{\"address\":\"" + _alice.Address + "\",\"outcome\":\"read\"}", recall.body);
         StringAssert.Contains("{\"address\":\"" + _bob.Address + "\",\"outcome\":\"recalled\"}", recall.body);

         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 0);

         // Marked read after the fact, the answer stays what it was: the recall
         // was asked and answered once.
         (int status, string body) again = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"" + messageId + "\"}");
         Assert.AreEqual(200, again.status, again.body);
         StringAssert.Contains("\"read\":1", again.body);
         StringAssert.Contains("\"recalled\":1", again.body);
      }

      [Test]
      [Description("An authenticated SMTP submission, with the Message-ID the client wrote, is recalled through the API by that Message-ID, with or without its angle brackets; a Message-ID nobody sent is a 404.")]
      public void AnAuthenticatedSmtpSubmissionIsRecalledByItsMessageId()
      {
         const string messageId = "smtp-recall-1@recall.test";
         SendAuthenticated(_sender, _alice.Address, messageId, "Over SMTP");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         // The negative control: an id that was never sent.
         (int status, string body) unknown = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<never-sent@recall.test>\"}");
         Assert.AreEqual(404, unknown.status, unknown.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + messageId + ">\"}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"message_id\":\"" + messageId + "\"", recall.body, "The brackets are taken off.");
         StringAssert.Contains("\"recalled\":1", recall.body);
         StringAssert.Contains("\"elsewhere\":0", recall.body, "No sent_id, nothing to count.");
         StringAssert.Contains("\"sent_marked\":false", recall.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);

         // Without the brackets is the same id.
         SendAuthenticated(_sender, _alice.Address, "smtp-recall-2@recall.test", "Over SMTP again");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         (int status, string body) bare = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"smtp-recall-2@recall.test\"}");
         Assert.AreEqual(200, bare.status, bare.body);
         StringAssert.Contains("\"recalled\":1", bare.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);
      }

      [Test]
      [Description("Another account calling recall with the sender's Message-ID: 404, and nothing is deleted; the sender's own call afterwards recalls it.")]
      public void AnotherAccountCannotRecallSomebodyElsesMessage()
      {
         const string messageId = "smtp-recall-other@recall.test";
         SendAuthenticated(_sender, _alice.Address, messageId, "Not yours to recall");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         (int status, string body) theirs = Send("POST", "/api/v1/me/recall", _asOther, "{\"message_id\":\"<" + messageId + ">\"}");
         Assert.AreEqual(404, theirs.status, theirs.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         // The control: the sender it belongs to.
         (int status, string body) mine = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + messageId + ">\"}");
         Assert.AreEqual(200, mine.status, mine.body);
         StringAssert.Contains("\"recalled\":1", mine.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);
      }

      [Test]
      [Description("An unauthenticated submission from a local range in a local account's name is NOT recorded, so its recall is 404 and the copy stays: the sender's identity is the session's, not the envelope's. The same message through the authenticated door is recallable.")]
      public void AnUnauthenticatedLocalRangeSubmissionIsNotRecorded()
      {
         const string claimed = "unauth-recall-1@recall.test";
         SmtpClientSimulator.StaticSendRaw(_sender.Address, _alice.Address,
            "From: " + _sender.Address + "\r\nTo: " + _alice.Address + "\r\nSubject: In the sender's name\r\nMessage-ID: <" + claimed + ">\r\n\r\nAnybody on the local range could write this envelope.\r\n");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + claimed + ">\"}");
         Assert.AreEqual(404, recall.status, recall.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         // The control: the same message, the session signed in as the sender.
         const string authenticated = "unauth-recall-2@recall.test";
         SendAuthenticated(_sender, _alice.Address, authenticated, "In the sender's name, signed in");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 2);
         (int status, string body) signedIn = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + authenticated + ">\"}");
         Assert.AreEqual(200, signedIn.status, signedIn.body);
         StringAssert.Contains("\"recalled\":1", signedIn.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
      }

      [Test]
      [Description("A distribution list with two local members: both members' copies are recalled, and a member's other mail stays.")]
      public void ADistributionListsMembersAreBothRecalled()
      {
         string listAddress = "recall-list@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, listAddress, new List<string> { _alice.Address, _bob.Address });

         SmtpClientSimulator.StaticSend("outsider@example.org", _alice.Address, "Stays", "Not on the list.");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         long sentId;
         string messageId = SendFromWebmail("To the list", listAddress, out sentId);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 2);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);

         (int status, string body) recall = Send("POST", "/api/v1/me/recall", _me,
            "{\"message_id\":\"" + messageId + "\",\"sent_id\":" + sentId + "}");
         Assert.AreEqual(200, recall.status, recall.body);
         StringAssert.Contains("\"recalled\":2", recall.body);
         StringAssert.Contains("\"elsewhere\":0", recall.body, "The list's address is at this server's own domain.");
         StringAssert.Contains("{\"address\":\"" + _alice.Address + "\",\"outcome\":\"recalled\"}", recall.body);
         StringAssert.Contains("{\"address\":\"" + _bob.Address + "\",\"outcome\":\"recalled\"}", recall.body);

         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 0);
      }

      [Test]
      [Description("Rows older than RecallDays are swept by the retention task: with recall_days 1 and the rows backdated, the sweep removes them and the recall answers 404 even under a window widened to include them afterwards; the same rows backdated but not swept still answer 200.")]
      public void RowsOlderThanRecallDaysAreSweptAndAnswer404()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recall_days\":1}").status);

         const string swept = "sweep-recall-1@recall.test";
         SendAuthenticated(_sender, _alice.Address, swept, "Old enough to be swept");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         // Backdated past the day, then the sweep the retention task runs.
         _application.Database.ExecuteSQL("update hm_recall set recallcreated = recallcreated - 200000 where recallmsgid = '" + swept + "'");
         _application.Utilities.RunMessageRetention();

         // Under a window that would include the backdated rows had they
         // survived, the answer is 404: the sweep took them.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recall_days\":365}").status);
         (int status, string body) gone = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + swept + ">\"}");
         Assert.AreEqual(404, gone.status, gone.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1, TimeSpan.FromSeconds(2));

         // The control: the same backdating without the sweep leaves the rows,
         // and the widened window finds them.
         const string kept = "sweep-recall-2@recall.test";
         SendAuthenticated(_sender, _alice.Address, kept, "Backdated, not swept");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 2);
         _application.Database.ExecuteSQL("update hm_recall set recallcreated = recallcreated - 200000 where recallmsgid = '" + kept + "'");
         (int status, string body) found = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + kept + ">\"}");
         Assert.AreEqual(200, found.status, found.body);
         StringAssert.Contains("\"recalled\":1", found.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         // And under the narrow window the backdated row is 404 before any sweep.
         const string narrow = "sweep-recall-3@recall.test";
         SendAuthenticated(_sender, _alice.Address, narrow, "Outside the window");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 2);
         _application.Database.ExecuteSQL("update hm_recall set recallcreated = recallcreated - 200000 where recallmsgid = '" + narrow + "'");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recall_days\":1}").status);
         (int status, string body) outside = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + narrow + ">\"}");
         Assert.AreEqual(404, outside.status, outside.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 2, TimeSpan.FromSeconds(2));

         // The setting's own bounds.
         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"recall_days\":0}").status);
         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"recall_days\":366}").status);
      }

      [Test]
      [Description("An app password cannot recall: 403, nothing deleted; the account's own password can.")]
      public void AnAppPasswordCannotRecall()
      {
         const string messageId = "app-recall-1@recall.test";
         SendAuthenticated(_sender, _alice.Address, messageId, "Not for a program");
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", _me, "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(_sender.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);

         (int status, string body) refused = Send("POST", "/api/v1/me/recall", asProgram, "{\"message_id\":\"<" + messageId + ">\"}");
         Assert.AreEqual(403, refused.status, refused.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 1);

         (int status, string body) allowed = Send("POST", "/api/v1/me/recall", _me, "{\"message_id\":\"<" + messageId + ">\"}");
         Assert.AreEqual(200, allowed.status, allowed.body);
         StringAssert.Contains("\"recalled\":1", allowed.body);
         Pop3ClientSimulator.AssertMessageCount(_alice.Address, Password, 0);

         // A body without the id, and one that is not an object, are refused
         // with a sentence and delete nothing.
         Assert.AreEqual(400, Send("POST", "/api/v1/me/recall", _me, "{}").status);
         Assert.AreEqual(400, Send("POST", "/api/v1/me/recall", _me, "[]").status);
      }

      /// <summary>
      ///    A webmail send to Alice, Bob and an outside address, the outside one
      ///    delivered to a listener of this test's own so the queue drains;
      ///    answers the Message-ID of the Sent copy.
      /// </summary>
      private string SendFromWebmailToTwoLocalsAndOneOutside(out long sentId)
      {
         const string outside = "outside@dummy-example.com";
         int capturePort = TestSetup.GetNextFreePort();

         using (var capture = new SmtpServerSimulator(1, capturePort))
         {
            capture.AddRecipientResult(new Dictionary<string, int> { { outside, 250 } });
            capture.StartListen();

            Route route = TestSetup.AddRoutePointingAtLocalhost(1, capturePort, false);
            try
            {
               string messageId = SendFromWebmail("Two here, one elsewhere", _alice.Address + ", " + _bob.Address + ", " + outside, out sentId);
               capture.WaitForCompletion();
               StringAssert.Contains("Two here, one elsewhere", capture.MessageData, "The outside copy went out as any other.");
               return messageId;
            }
            finally
            {
               if (route.ID > 0)
                  _settings.Routes.DeleteByDBID(route.ID);
            }
         }
      }

      /// <summary>POST /api/v1/me/messages as the page posts it; answers the Sent copy's Message-ID.</summary>
      private string SendFromWebmail(string subject, string to, out long sentId)
      {
         (int status, string body) sent = Send("POST", "/api/v1/me/messages", _me,
            "{\"to\":\"" + to + "\",\"subject\":\"" + subject + "\",\"text\":\"Withdrawable while unread.\"}");
         Assert.AreEqual(201, sent.status, sent.body);
         sentId = long.Parse(Regex.Match(sent.body, "\"sent_id\":(\\d+)").Groups[1].Value);
         Assert.Greater(sentId, 0, "The webmail kept no Sent copy: " + sent.body);

         (int status, string body) copy = Send("GET", "/api/v1/me/messages/" + sentId, _me, null);
         Assert.AreEqual(200, copy.status, copy.body);
         string messageId = Regex.Match(copy.body, "\"message_id\":\"([^\"]*)\"").Groups[1].Value;
         Assert.IsFalse(string.IsNullOrEmpty(messageId), "The Sent copy carries no Message-ID: " + copy.body);
         return messageId;
      }

      /// <summary>An SMTP submission signed in as the account, with the Message-ID the client chose.</summary>
      private static void SendAuthenticated(Account from, string to, string messageId, string subject)
      {
         new SmtpClientSimulator().SendRaw(from.Address, Password, from.Address, to,
            "From: " + from.Address + "\r\nTo: " + to + "\r\nSubject: " + subject + "\r\nMessage-ID: <" + messageId + ">\r\n\r\n" + subject + ", sent over authenticated SMTP.\r\n");
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
