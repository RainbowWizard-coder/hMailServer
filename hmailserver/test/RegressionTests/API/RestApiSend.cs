// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using DBUpdater;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The send API (docs/SendApi.md): POST /api/v1/send under an API key whose role
   ///    holds send_through_api and which names its own sender, and GET /api/v1/send and
   ///    /api/v1/send/{queue_id}, which answer the key that sent - and nobody else - what
   ///    became of it. Every test holds its claim against its negative control: the same
   ///    body from a key that may not send, a sender outside the pattern, another key's
   ///    send, a ceiling not reached, a header the route takes, the trace off.
   ///
   ///    Keys are issued per test through POST /api/v1/apikeys and revoked in the
   ///    teardown, as RestApiApiKeys issues them; external recipients are at
   ///    dummy-example.com, which a route of the test's own points at the SMTP simulator.
   /// </summary>
   [TestFixture]
   public class RestApiSend : TestFixtureBase
   {
      private static int RestPort = 9896;
      private const string AdminPassword = "testar";
      private const string Password = "Send-Api-Passw0rd!";
      private const string KeyStoreFileName = "hMailServerApiKeys.ini";
      private const string CustomRole = "sendtest-mailer";

      private readonly List<string> _keys = new List<string>();
      private Account _noreply;
      private Account _reader;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         // The sender is an account as well, so that "no Sent copy anywhere" is asked
         // of a mailbox that could have one; and the reader is who most sends go to.
         _noreply = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "noreply@" + _domain.Name, Password);
         _reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         _application.Database.ExecuteSQL("delete from hm_apisends");
         _application.Database.ExecuteSQL("delete from hm_messagetrace");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            foreach (string id in _keys)
               Admin("DELETE", "/api/v1/apikeys/" + id, null);
            _keys.Clear();

            Admin("DELETE", "/api/v1/roles/" + CustomRole, null);

            _application.Database.ExecuteSQL("delete from hm_apisends");
            _application.Database.ExecuteSQL("delete from hm_messagetrace");
         }
         finally
         {
            ServerIniFile.SetSetting("MessageTraceEnabled", null);
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // --------------------------------------------------------------- tests ---

      [Test]
      [Description("A key with send_from noreply@ sends text, html and an attachment to a local account: 202 with a queue id; the message arrives from noreply@ with that Return-Path, the attachment's bytes intact and a Received line naming the send API and the key's label, and nothing is kept in any Sent folder. The same body from a key without a sender, a from outside the key's sender, a read-only key, the administrator password and an account are each refused.")]
      public void AKeyWithASenderSendsAndTheMessageArrivesAsTheSender()
      {
         var key = MakeKey("the invoicing app", "noreply@" + _domain.Name);

         byte[] bytes = new byte[3000];
         new Random(334).NextBytes(bytes);
         string expectedDigest = Sha256(bytes);

         string body = "{\"from\":\"Invoicing <noreply@" + _domain.Name + ">\",\"to\":[\"" + _reader.Address + "\"]," +
                       "\"subject\":\"Your invoice\",\"text\":\"The invoice is attached.\",\"html\":\"<p>The invoice is attached.</p>\"," +
                       "\"attachments\":[{\"filename\":\"invoice.bin\",\"content_type\":\"application/octet-stream\",\"data\":\"" + Convert.ToBase64String(bytes) + "\"}]}";

         (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send", body);
         Assert.AreEqual(202, sent.status, sent.body);
         Assert.Greater(long.Parse(Value(sent.body, "queue_id")), 0, sent.body);
         StringAssert.Contains("\"message_id\":\"<", sent.body, "The answer names the Message-ID.");
         StringAssert.Contains("\"recorded\":true", sent.body);

         IMAPFolder inbox = _reader.IMAPFolders.get_ItemByName("INBOX");
         CustomAsserts.AssertFolderMessageCount(inbox, 1);

         // The file as it was delivered, read where it lies rather than over POP3,
         // which would take it out of the mailbox the rest of the test counts.
         hMailServer.Message delivered = inbox.Messages[0];
         string text = File.ReadAllText(delivered.Filename);
         StringAssert.Contains("Return-Path: <noreply@" + _domain.Name + ">", text, "The envelope sender is the From address.");
         string fromLine = text.Split(new[] { "\r\n" }, StringSplitOptions.None).FirstOrDefault(line => line.StartsWith("From:", StringComparison.OrdinalIgnoreCase));
         Assert.IsNotNull(fromLine, text);
         StringAssert.Contains("noreply@" + _domain.Name, fromLine);
         StringAssert.Contains("Invoicing", fromLine, "The display name is the application's.");
         StringAssert.Contains("(hMailServer send API, key \"the invoicing app\"", text, "The Received line names the API and the key's label.");
         Assert.IsTrue(Regex.IsMatch(text, "^Received: from \\[127\\.0\\.0\\.1\\] \\(hMailServer send API", RegexOptions.Multiline), text);

         Assert.AreEqual(1, delivered.Attachments.Count, "The attachment arrived.");
         Assert.AreEqual("invoice.bin", delivered.Attachments[0].Filename);
         string saved = Path.Combine(Path.GetTempPath(), "restapisend-" + Guid.NewGuid().ToString("N") + ".bin");
         try
         {
            delivered.Attachments[0].SaveAs(saved);
            Assert.AreEqual(expectedDigest, Sha256(File.ReadAllBytes(saved)), "The attachment's bytes arrived as they were sent.");
         }
         finally
         {
            File.Delete(saved);
         }

         for (int i = 0; i < _noreply.IMAPFolders.Count; i++)
            Assert.AreEqual(0, _noreply.IMAPFolders[i].Messages.Count, "The send API keeps no copy in any folder of the sender: " + _noreply.IMAPFolders[i].Name);

         // The controls: the same body from a key that names no sender, a from the key's
         // sender does not allow, a read-only key that was given a sender by hand, the
         // administrator password and an account's own credentials.
         var noSender = MakeKey("no sender", null);
         (int status, string body) refused = AsKey(noSender.token, "POST", "/api/v1/send", body);
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("names no sender", refused.body);

         (int status, string body) outside = AsKey(key.token, "POST", "/api/v1/send", body.Replace("Invoicing <noreply@", "Invoicing <billing@"));
         Assert.AreEqual(400, outside.status, outside.body);
         StringAssert.Contains("the sender this key may use", outside.body);

         (int status, string body) readOnlyWithSender = Admin("POST", "/api/v1/apikeys", "{\"label\":\"reader with a sender\",\"send_from\":\"noreply@" + _domain.Name + "\"}");
         Assert.AreEqual(400, readOnlyWithSender.status, "A read-only key cannot carry send_from. " + readOnlyWithSender.body);

         var readOnly = MakeKey("reads only", null, "readonly");
         EditStoredKey(readOnly.id, "SendFrom", "noreply@" + _domain.Name);
         (int status, string body) readOnlyRefused = AsKey(readOnly.token, "POST", "/api/v1/send", body);
         Assert.AreEqual(403, readOnlyRefused.status, readOnlyRefused.body);
         StringAssert.Contains("read-only", readOnlyRefused.body);

         Assert.AreEqual(403, Admin("POST", "/api/v1/send", body).status, "The administrator password names no sender.");
         (int status, string body) asAccount = Send("POST", "/api/v1/send", Basic(_noreply.Address, Password), body);
         Assert.AreEqual(403, asAccount.status, "A person sends with /me/messages. " + asAccount.body);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(_reader.Address, Password, 1);
      }

      [Test]
      [Description("A key whose sender is *@domain sends as any address of that domain and names it in from; another hosted domain's address is refused, and a key cannot be made with a sender at a domain this server does not host.")]
      public void APatternKeySendsAsAnyAddressOfItsDomainAndNoOther()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("other-send.test");
         Assert.IsNotNull(other);

         var key = MakeKey("the alerts app", "*@" + _domain.Name);

         foreach (string localPart in new[] { "alerts", "billing" })
         {
            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
               "{\"from\":\"" + localPart + "@" + _domain.Name + "\",\"to\":[\"" + _reader.Address + "\"],\"subject\":\"From " + localPart + "\",\"text\":\"Hello.\"}");
            Assert.AreEqual(202, sent.status, localPart + ": " + sent.body);
         }

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 2);

         (int status, string body) unnamed = AsKey(key.token, "POST", "/api/v1/send",
            "{\"to\":[\"" + _reader.Address + "\"],\"subject\":\"No from\",\"text\":\"Hello.\"}");
         Assert.AreEqual(400, unnamed.status, "A pattern names no one address, so the body must. " + unnamed.body);
         StringAssert.Contains("from is required", unnamed.body);

         (int status, string body) otherDomain = AsKey(key.token, "POST", "/api/v1/send",
            "{\"from\":\"alerts@other-send.test\",\"to\":[\"" + _reader.Address + "\"],\"subject\":\"Other\",\"text\":\"Hello.\"}");
         Assert.AreEqual(400, otherDomain.status, otherDomain.body);

         foreach (string sender in new[] { "*@not-hosted-here.test", "noreply@not-hosted-here.test" })
         {
            (int status, string body) refused = Admin("POST", "/api/v1/apikeys", "{\"label\":\"elsewhere\",\"scope\":\"full\",\"send_from\":\"" + sender + "\"}");
            Assert.AreEqual(400, refused.status, sender + ": " + refused.body);
            StringAssert.Contains("not one of them", refused.body);
         }

         Assert.AreEqual(400, Admin("POST", "/api/v1/apikeys", "{\"label\":\"not a sender\",\"scope\":\"full\",\"send_from\":\"not an address\"}").status);

         // The control: the other hosted domain is a sender a key may be made with.
         var otherKey = MakeKey("the other domain's app", "*@other-send.test");
         StringAssert.Contains("\"send_from\":\"*@other-send.test\"", Admin("GET", "/api/v1/apikeys", null).body);
         Assert.IsNotNull(otherKey.token);
      }

      [Test]
      [Description("A send to an address elsewhere reads queued while the remote holds its reply, then delivered with the time; a recipient the remote refuses with 550 5.1.1 reads bounced with that status and the remote's reply, beside one delivered in the same message.")]
      public void AnExternalSendReadsQueuedThenDeliveredOrBouncedWithTheRemotesReply()
      {
         EnableTrace();
         var key = MakeKey("the notifier", "noreply@" + _domain.Name);

         const string partner = "partner@dummy-example.com";
         const string ghost = "ghost@dummy-example.com";

         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { partner, 250 }, { ghost, 550 } });
            server.RecipientReplyTexts[ghost] = "5.1.1 no such user";
            server.HoldMailFromReplyFor = TimeSpan.FromSeconds(8);
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, port, false);

            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
               "{\"to\":[\"" + partner + "\",\"" + ghost + "\"],\"subject\":\"Your order\",\"text\":\"Shipped.\"}");
            Assert.AreEqual(202, sent.status, sent.body);
            string id = Value(sent.body, "queue_id");

            // While the remote holds its reply to MAIL FROM, nothing has happened yet.
            WaitUntil(() => server.CommandsReceived.Any(command => command.StartsWith("MAIL", StringComparison.OrdinalIgnoreCase)), "the remote was never reached");
            (int status, string body) waiting = AsKey(key.token, "GET", "/api/v1/send/" + id, null);
            Assert.AreEqual(200, waiting.status, waiting.body);
            Assert.AreEqual("queued", RecipientState(waiting.body, partner).state, waiting.body);
            StringAssert.Contains("\"state\":\"queued\"", waiting.body);

            server.WaitForCompletion();

            string final = WaitForSendState(key.token, id, "bounced");
            var delivered = RecipientState(final, partner);
            Assert.AreEqual("delivered", delivered.state, final);
            Assert.IsFalse(string.IsNullOrEmpty(delivered.when), "A delivery says when. " + final);

            var bounced = RecipientState(final, ghost);
            Assert.AreEqual("bounced", bounced.state, final);
            Assert.AreEqual("5.1.1", bounced.status, final);
            Assert.AreEqual(550, bounced.code, final);
            StringAssert.Contains("no such user", bounced.reason, final);
            StringAssert.Contains("\"trace\":true", final);
         }
      }

      [Test]
      [Description("A recipient the remote answers 451 4.3.0 reads deferred with that status and the remote's reply while the message waits, and delivered once the next attempt is taken.")]
      public void ADeferredRecipientReadsDeferredUntilItIsDelivered()
      {
         EnableTrace();
         var key = MakeKey("the retrier", "noreply@" + _domain.Name);

         const string busy = "busy@dummy-example.com";

         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(2, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { busy, 451 } });
            server.AddRecipientResult(new Dictionary<string, int> { { busy, 250 } });
            server.RecipientReplyTexts[busy] = "4.3.0 mailbox busy, try later";
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(3, port, false);

            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
               "{\"to\":[\"" + busy + "\"],\"subject\":\"Retry me\",\"text\":\"Later.\"}");
            Assert.AreEqual(202, sent.status, sent.body);
            string id = Value(sent.body, "queue_id");

            string deferred = WaitForSendState(key.token, id, "deferred");
            var state = RecipientState(deferred, busy);
            Assert.AreEqual("deferred", state.state, deferred);
            Assert.AreEqual("4.3.0", state.status, deferred);
            Assert.AreEqual(451, state.code, deferred);
            StringAssert.Contains("try later", state.reason, deferred);

            server.RecipientReplyTexts.Remove(busy);
            TestSetup.SendMessagesInQueue();
            server.WaitForCompletion();

            string delivered = WaitForSendState(key.token, id, "delivered");
            Assert.AreEqual("delivered", RecipientState(delivered, busy).state, delivered);
         }
      }

      [Test]
      [Description("A second key cannot read the first key's send - the same 404 as an id nobody sent - and the listing shows only the caller's own sends, newest first, paged; an administrator's key that names no sender lists nothing, and the administrator password is refused the listing.")]
      public void AKeyReadsItsOwnSendsAndNoOtherKeys()
      {
         var first = MakeKey("the first app", "noreply@" + _domain.Name);
         var second = MakeKey("the second app", "noreply@" + _domain.Name);

         var firstIds = new List<long>();
         for (int i = 1; i <= 3; i++)
            firstIds.Add(long.Parse(Value(SendTo(first.token, _reader.Address, "First " + i).body, "queue_id")));

         long secondId = long.Parse(Value(SendTo(second.token, _reader.Address, "Second").body, "queue_id"));

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 4);

         (int status, string body) unknown = AsKey(second.token, "GET", "/api/v1/send/999999999", null);
         (int status, string body) othersSend = AsKey(second.token, "GET", "/api/v1/send/" + firstIds[0], null);
         (int status, string body) notANumber = AsKey(second.token, "GET", "/api/v1/send/abc", null);
         Assert.AreEqual(404, unknown.status, unknown.body);
         Assert.AreEqual(404, othersSend.status, othersSend.body);
         Assert.AreEqual(unknown.body, othersSend.body, "Another key's send answers exactly as an id nobody sent.");
         Assert.AreEqual(unknown.body, notANumber.body);
         Assert.AreEqual(200, AsKey(first.token, "GET", "/api/v1/send/" + firstIds[0], null).status, "The control: the key that sent it reads it.");

         (int status, string body) listed = AsKey(first.token, "GET", "/api/v1/send", null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"total\":3", listed.body);
         CollectionAssert.AreEqual(firstIds.OrderByDescending(id => id).ToList(), QueueIds(listed.body), "Newest first, and only its own. " + listed.body);
         CollectionAssert.DoesNotContain(QueueIds(listed.body), secondId);

         CollectionAssert.AreEqual(new List<long> { firstIds[2], firstIds[1] }, QueueIds(AsKey(first.token, "GET", "/api/v1/send?page=1&size=2", null).body));
         CollectionAssert.AreEqual(new List<long> { firstIds[0] }, QueueIds(AsKey(first.token, "GET", "/api/v1/send?page=2&size=2", null).body));
         Assert.AreEqual(400, AsKey(first.token, "GET", "/api/v1/send?size=201", null).status);
         Assert.AreEqual(400, AsKey(first.token, "GET", "/api/v1/send?page=0", null).status);

         CollectionAssert.AreEqual(new List<long> { secondId }, QueueIds(AsKey(second.token, "GET", "/api/v1/send", null).body));

         var administrators = MakeKey("an administrator's key", null);
         (int status, string body) nothing = AsKey(administrators.token, "GET", "/api/v1/send", null);
         Assert.AreEqual(200, nothing.status, nothing.body);
         StringAssert.Contains("\"total\":0", nothing.body, "The listing is the key's own; the trace is the server-wide view.");

         Assert.AreEqual(403, Admin("GET", "/api/v1/send", null).status);
         Assert.AreEqual(403, Admin("GET", "/api/v1/send/" + firstIds[0], null).status);
      }

      [Test]
      [Description("send_per_minute = 2 refuses the third send with 429, Retry-After and the sentence, while a key without the ceiling sends three; a message over the server's size is 413 and one under it is sent; a header the route does not take is 400 and the ones it takes arrive; a recipient list over the server's ceiling is refused with the server's own sentence, and one at it is sent.")]
      public void TheCeilingsAreTheKeysAndTheServers()
      {
         var limited = MakeKey("two a minute", "noreply@" + _domain.Name, "full", null, 2, 0);
         var free = MakeKey("no ceiling", "noreply@" + _domain.Name);

         Assert.AreEqual(202, SendTo(limited.token, _reader.Address, "One").status);
         Assert.AreEqual(202, SendTo(limited.token, _reader.Address, "Two").status);
         var third = Raw("POST", "/api/v1/send", "Bearer " + limited.token, SendBody(_reader.Address, "Three"));
         Assert.AreEqual(429, third.status, third.body);
         StringAssert.Contains("Retry-After: 60", third.headers);
         StringAssert.Contains("send_per_minute", third.body);

         for (int i = 1; i <= 3; i++)
            Assert.AreEqual(202, SendTo(free.token, _reader.Address, "Free " + i).status, "The control: a key without a ceiling of its own.");

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 5);

         // The server's maximum message size, in kilobytes.
         _settings.MaxMessageSize = 40;
         try
         {
            (int status, string body) tooLarge = AsKey(free.token, "POST", "/api/v1/send", WithAttachment(_reader.Address, "Large", 60 * 1024));
            Assert.AreEqual(413, tooLarge.status, tooLarge.body);
            Assert.AreEqual(202, AsKey(free.token, "POST", "/api/v1/send", WithAttachment(_reader.Address, "Small", 4 * 1024)).status);
         }
         finally
         {
            _settings.MaxMessageSize = 20480;
         }

         foreach (string header in new[] { "Subject", "X-hMailServer-Spam", "Content-Type", "Bcc" })
         {
            (int status, string body) refused = AsKey(free.token, "POST", "/api/v1/send",
               "{\"to\":[\"" + _reader.Address + "\"],\"subject\":\"Header\",\"text\":\"Body.\",\"headers\":{\"" + header + "\":\"value\"}}");
            Assert.AreEqual(400, refused.status, header + ": " + refused.body);
         }

         Assert.AreEqual(202, AsKey(free.token, "POST", "/api/v1/send",
            "{\"to\":[\"" + _reader.Address + "\"],\"subject\":\"With headers\",\"text\":\"Body.\"," +
            "\"headers\":{\"X-Campaign\":\"autumn-334\",\"List-Unsubscribe\":\"<mailto:unsubscribe@" + _domain.Name + ">\"}}").status);

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 7);
         bool found = false;
         for (int i = 0; i < _reader.IMAPFolders.get_ItemByName("INBOX").Messages.Count; i++)
         {
            hMailServer.Message message = _reader.IMAPFolders.get_ItemByName("INBOX").Messages[i];
            if (message.Subject == "With headers")
            {
               found = true;
               Assert.AreEqual("autumn-334", message.get_HeaderValue("X-Campaign"));
               StringAssert.Contains("unsubscribe@" + _domain.Name, message.get_HeaderValue("List-Unsubscribe"));
            }
         }
         Assert.IsTrue(found, "The message with the headers arrived.");

         // The server's recipient ceiling, and its own sentence.
         _settings.MaxSMTPRecipientsInBatch = 3;
         try
         {
            string three = "[\"" + _reader.Address + "\",\"" + _noreply.Address + "\",\"" + _reader.Address + "\"]";
            string four = "[\"" + _reader.Address + "\",\"" + _noreply.Address + "\",\"" + _reader.Address + "\",\"" + _noreply.Address + "\"]";

            (int status, string body) over = AsKey(free.token, "POST", "/api/v1/send", "{\"to\":" + four + ",\"subject\":\"Four\",\"text\":\"Body.\"}");
            Assert.AreEqual(400, over.status, over.body);
            StringAssert.Contains("too many recipients; the server allows 3", over.body);

            Assert.AreEqual(202, AsKey(free.token, "POST", "/api/v1/send", "{\"to\":" + three + ",\"subject\":\"Three\",\"text\":\"Body.\"}").status);
         }
         finally
         {
            _settings.MaxSMTPRecipientsInBatch = 100;
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("A key whose role does not hold send_through_api is refused with 403 naming the permission, though it has a sender; while its role held the permission the same key sent. A key cannot be made with a sender and a role that lacks the permission.")]
      public void ARoleWithoutSendThroughApiCannotSend()
      {
         (int status, string body) role = Admin("POST", "/api/v1/roles",
            "{\"name\":\"" + CustomRole + "\",\"description\":\"sends mail\",\"permissions\":\"view_configuration,send_through_api\"}");
         Assert.AreEqual(201, role.status, role.body);
         StringAssert.Contains("send_through_api", role.body, "A custom role may hold the permission.");

         var key = MakeKey("the role's app", "noreply@" + _domain.Name, null, CustomRole);
         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "While the role may send").status, "The control: the role holds the permission.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/roles/" + CustomRole, "{\"permissions\":\"view_configuration\"}").status);

         (int status, string body) refused = AsKey(key.token, "POST", "/api/v1/send", SendBody(_reader.Address, "After the role lost it"));
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("send_through_api", refused.body);

         (int status, string body) cannotBeMade = Admin("POST", "/api/v1/apikeys",
            "{\"label\":\"a role that does not send\",\"role\":\"" + CustomRole + "\",\"send_from\":\"noreply@" + _domain.Name + "\"}");
         Assert.AreEqual(400, cannotBeMade.status, cannotBeMade.body);
         StringAssert.Contains("send_through_api", cannotBeMade.body);

         StringAssert.Contains("send_through_api", Admin("GET", "/api/v1/roles", null).body, "The permission is in the vocabulary the editors read.");

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 1);
      }

      [Test]
      [Description("With the message trace off, a send that has left the queue reads unknown and says why; with it on, the same send reads delivered.")]
      public void WithTheTraceOffASendThatLeftTheQueueReadsUnknown()
      {
         var key = MakeKey("the quiet app", "noreply@" + _domain.Name);

         string off = Value(SendTo(key.token, _reader.Address, "Trace off").body, "queue_id");
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         (int status, string body) unknown = AsKey(key.token, "GET", "/api/v1/send/" + off, null);
         Assert.AreEqual(200, unknown.status, unknown.body);
         StringAssert.Contains("\"trace\":false", unknown.body);
         Assert.AreEqual("unknown", RecipientState(unknown.body, _reader.Address).state, unknown.body);
         StringAssert.Contains("MessageTraceEnabled", unknown.body);

         EnableTrace();
         string on = Value(SendTo(key.token, _reader.Address, "Trace on").body, "queue_id");
         string delivered = WaitForSendState(key.token, on, "delivered");
         Assert.AreEqual("delivered", RecipientState(delivered, _reader.Address).state, delivered);
      }

      [Test]
      [Description("DBUpdater's probe for schema 6089 - the column a key's read of its own sends filters on - succeeds on the bench's database, and the same probe naming a column that is not there fails.")]
      public void TheSchemaProbeForTheSendRecordSucceeds()
      {
         Database database = _application.Database;
         SchemaProbe probe = SchemaVerification.GetProbesFor(6089).First(p => p.Describes == "hm_apisends.apskeyid");

         database.ExecuteSQL(probe.Statement);

         string absent = probe.Statement.Replace("apskeyid", "apsnosuchcolumn");
         Assert.AreNotEqual(probe.Statement, absent);
         var refused = Assert.Throws<COMException>(() => database.ExecuteSQL(absent), "The probe must fail for a column that is not there.");
         StringAssert.Contains("HM10044", refused.Message, "The probe failed for a reason other than the backend evaluating it.");

         // The refusal is reported through the API error and also, by the connection
         // that saw it, to the error log; that line is this test's, not a fault.
         LogHandler.ReadAndDeleteErrorLog();
      }

      // ------------------------------------------------------------- helpers ---

      private (string id, string token) MakeKey(string label, string sendFrom, string scope = "full", string role = null,
                                                int perMinute = 0, int perDay = 0)
      {
         var body = new StringBuilder("{\"label\":\"" + label + "\"");
         if (scope != null)
            body.Append(",\"scope\":\"" + scope + "\"");
         if (role != null)
            body.Append(",\"role\":\"" + role + "\"");
         if (sendFrom != null)
            body.Append(",\"send_from\":\"" + sendFrom + "\"");
         if (perMinute > 0)
            body.Append(",\"send_per_minute\":" + perMinute);
         if (perDay > 0)
            body.Append(",\"send_per_day\":" + perDay);
         body.Append("}");

         (int status, string body) created = Admin("POST", "/api/v1/apikeys", body.ToString());
         Assert.AreEqual(201, created.status, created.body);

         string id = Value(created.body, "id");
         string token = Value(created.body, "key");
         _keys.Add(id);

         if (sendFrom != null)
            StringAssert.Contains("\"send_from\":\"" + sendFrom.ToLowerInvariant() + "\"", created.body);

         return (id, token);
      }

      private static string SendBody(string to, string subject)
      {
         return "{\"to\":[\"" + to + "\"],\"subject\":\"" + subject + "\",\"text\":\"Sent through the send API.\"}";
      }

      private static (int status, string body) SendTo(string token, string to, string subject)
      {
         (int status, string body) sent = AsKey(token, "POST", "/api/v1/send", SendBody(to, subject));
         Assert.AreEqual(202, sent.status, sent.body);
         return sent;
      }

      private static string WithAttachment(string to, string subject, int bytes)
      {
         byte[] data = new byte[bytes];
         new Random(bytes).NextBytes(data);
         return "{\"to\":[\"" + to + "\"],\"subject\":\"" + subject + "\",\"text\":\"Body.\"," +
                "\"attachments\":[{\"filename\":\"data.bin\",\"data\":\"" + Convert.ToBase64String(data) + "\"}]}";
      }

      private void EnableTrace()
      {
         ServerIniFile.SetSetting("MessageTraceEnabled", "1");
         _application.Reinitialize();
      }

      // Polls one send until it reads the state asked for, and answers the body.
      private static string WaitForSendState(string token, string id, string state)
      {
         string last = "";
         DateTime until = DateTime.UtcNow.AddSeconds(40);
         while (DateTime.UtcNow < until)
         {
            (int status, string body) read = AsKey(token, "GET", "/api/v1/send/" + id, null);
            last = read.body;
            if (read.status == 200 && Regex.IsMatch(read.body, "\"recipients_state\"") &&
                Regex.Match(read.body, "\"state\":\"([a-z]+)\",\"trace\"").Groups[1].Value == state)
               return read.body;
            Thread.Sleep(250);
         }

         Assert.Fail("The send never read " + state + ": " + last);
         return last;
      }

      private static void WaitUntil(Func<bool> condition, string what)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(30);
         while (DateTime.UtcNow < until)
         {
            if (condition())
               return;
            Thread.Sleep(100);
         }

         Assert.Fail("Timed out: " + what);
      }

      private static (string state, string when, string status, int code, string reason) RecipientState(string body, string address)
      {
         Match match = Regex.Match(body,
            "\\{\"address\":\"" + Regex.Escape(address) + "\",\"state\":\"([a-z]+)\",\"when\":\"([^\"]*)\",\"status\":\"([^\"]*)\",\"code\":(\\d+),\"reason\":\"((?:[^\"\\\\]|\\\\.)*)\"\\}",
            RegexOptions.IgnoreCase);
         Assert.IsTrue(match.Success, "No state for " + address + " in " + body);
         return (match.Groups[1].Value, match.Groups[2].Value, match.Groups[3].Value, int.Parse(match.Groups[4].Value), match.Groups[5].Value);
      }

      private static List<long> QueueIds(string body)
      {
         return Regex.Matches(body, "\"queue_id\":(\\d+)").Cast<Match>().Select(m => long.Parse(m.Groups[1].Value)).ToList();
      }

      private static string Value(string json, string name)
      {
         Match match = Regex.Match(json, "\"" + name + "\"\\s*:\\s*(?:\"([^\"]*)\"|(\\d+))");
         Assert.IsTrue(match.Success, "No " + name + " in " + json);
         return match.Groups[1].Success ? match.Groups[1].Value : match.Groups[2].Value;
      }

      private static string Sha256(byte[] bytes)
      {
         using (SHA256 sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "");
      }

      // Rewrites one field of a stored key in place, as an administrator may edit the
      // store by hand: here, a sender on a read-only key, which the routes refuse.
      private static void EditStoredKey(string id, string field, string value)
      {
         string store = IniFileSetting.CandidateDirectories()
            .Select(directory => Paths.Combine(directory, KeyStoreFileName))
            .FirstOrDefault(File.Exists);
         Assert.IsNotNull(store, "The API key store was not found.");

         Assert.IsTrue(IniFile.WritePrivateProfileString("Key." + id, field, value, store));
         // settings-store-exempt: a flush of hMailServerApiKeys.ini, the API key store; it writes no key and never touches hMailServer.ini.
         IniFile.WritePrivateProfileString(null, null, null, store);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) AsKey(string token, string method, string path, string requestBody)
      {
         return Send(method, path, "Bearer " + token, requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      // One request with the response's header block kept, for Retry-After.
      private static (int status, string headers, string body) Raw(string method, string path, string authorization, string requestBody)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               head.Append("Authorization: " + authorization + "\r\n");
               if (requestBody != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string headers = separator >= 0 ? raw.Substring(0, separator) : raw;
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               int status = 0;
               string[] parts = headers.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               if (parts.Length >= 2)
                  int.TryParse(parts[1], out status);

               return (status, headers, body);
            }
         }
      }
   }
}
