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
   ///    became of each address its body named, and of no other. Every test holds its
   ///    claim against its negative control: the same body from a key that may not send,
   ///    a sender outside the pattern, another key's send, a ceiling not reached, a
   ///    header the route takes, the trace off, the trace's own view of the same queue id.
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

      // The public half of test/SSL examples/example.key, as a DKIM record carries it
      // (AntiSpam.DmarcRptReporting publishes the same key).
      private const string ExampleKeyPublic =
         "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAit1HZshVeIm3Yu3dBqKzIAQDM+k5hPu+S9RzJeaFnPQw888jfvuBQkVTinZWn65X4TLhcEjsV7iDgWzVhcEKUUphhpR9i+JgOjncOSxs7zvv2xOpFuYweOqVrWV9brr8DEt3f+MdfYUiz62toL82Za447DOhNI/YAVEJqCmgbeSycN2emmZC6Z8dXV7fxKM3IeJ6G8hVLbvWhZe8fHkJ0+tJXeARBHhowFW1VXgkOGOHFtPjpmNrJRbbDKf8+IqyUk9uV51y3GEIunovr1Yc3vvExpXwWLZIdqKtvGVFBxyvTtuAmtw7Ebmz0evN41wH7vTWgui0VgsZqNIIUwz+fQIDAQAB";

      private readonly List<string> _keys = new List<string>();
      private Account _noreply;
      private Account _reader;

      [SetUp]
      public void StartRestApi()
      {
         // The rows first: on a database below 6089 these throw, and NUnit does not
         // tear down a setup that threw - so nothing is started before them.
         _application.Database.ExecuteSQL("delete from hm_apisends");
         _application.Database.ExecuteSQL("delete from hm_messagetrace");

         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         // The sender is an account as well, so that "no Sent copy anywhere" is asked
         // of a mailbox that could have one; and the reader is who most sends go to.
         _noreply = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "noreply@" + _domain.Name, Password);
         _reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);
      }

      [TearDown]
      public void StopRestApi()
      {
         // Each key revoked through the route; any the route did not revoke - a
         // listener that stopped answering part way - is taken out of the store by
         // hand in the finally, so that no key of this fixture outlives it.
         var pending = new List<string>(_keys);
         _keys.Clear();

         try
         {
            foreach (string id in pending.ToList())
            {
               if (Admin("DELETE", "/api/v1/apikeys/" + id, null).status == 200)
                  pending.Remove(id);
            }

            Admin("DELETE", "/api/v1/roles/" + CustomRole, null);

            _application.Database.ExecuteSQL("delete from hm_apisends");
            _application.Database.ExecuteSQL("delete from hm_messagetrace");
         }
         finally
         {
            foreach (string id in pending)
               RemoveStoredKey(id);

            SuiteDns.Reset();
            ServerIniFile.SetSetting("MessageTraceEnabled", null);
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // --------------------------------------------------------------- tests ---

      [Test]
      [Description("A key with send_from noreply@ sends text, html and an attachment to a local account: 202 with a queue id and the one address it named; the message arrives from noreply@ with that Return-Path, the attachment's bytes intact and a Received field naming the send API and the key's label - and neither the key's id nor the application's address - and nothing is kept in any Sent folder. The same body from a key without a sender, a from outside the key's sender, a read-only key, the administrator password and an account are each refused.")]
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
         StringAssert.Contains("\"recipients\":1", sent.body, "The one address the body named.");
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

         // The trace field of the hop, unfolded: the API and the label, and nothing a
         // recipient does not need - not the key's id, which is its revocation handle,
         // nor the address the application called from.
         Match received = Regex.Match(text, "^Received: by [^\\r\\n]*(?:\\r\\n[ \\t][^\\r\\n]*)*", RegexOptions.Multiline);
         Assert.IsTrue(received.Success, "No Received field of the send API's: " + text);
         string field = Regex.Replace(received.Value, "\\r\\n[ \\t]+", " ");
         StringAssert.Contains("(hMailServer send API, key \"the invoicing app\")", field, "The Received field names the API and the key's label.");
         StringAssert.Contains(" with HTTP;", field);
         StringAssert.DoesNotContain("127.0.0.1", field, "The application's address is the server log's, not every recipient's.");
         StringAssert.DoesNotContain(key.id, text, "The key's id is nowhere in the message.");

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
         // sender does not allow, a read-only key, the administrator password and an
         // account's own credentials.
         var noSender = MakeKey("no sender", null);
         (int status, string body) refused = AsKey(noSender.token, "POST", "/api/v1/send", body);
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("names no sender", refused.body);

         (int status, string body) outside = AsKey(key.token, "POST", "/api/v1/send", body.Replace("Invoicing <noreply@", "Invoicing <billing@"));
         Assert.AreEqual(400, outside.status, outside.body);
         StringAssert.Contains("the sender this key may use", outside.body);

         (int status, string body) readOnlyWithSender = Admin("POST", "/api/v1/apikeys", "{\"label\":\"reader with a sender\",\"send_from\":\"noreply@" + _domain.Name + "\"}");
         Assert.AreEqual(400, readOnlyWithSender.status, readOnlyWithSender.body);
         StringAssert.Contains("a read-only key cannot carry one", readOnlyWithSender.body);

         var readOnly = MakeKey("reads only", null, "readonly");
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
      [Description("A key is made with a sender only as the server can hold it: a send_from carrying a line break, an equals sign or a bracket - which the key store would read as lines and sections of its own - is refused and nothing is written; a sender at a domain alias or at a domain switched off is refused; *@domain, or an address that is no account's, needs send_per_day; a confined key's sender is in its domains; a ceiling needs a sender and a range. Each against the request that is made.")]
      public void AKeyIsMadeOnlyWithASenderTheServerCanHold()
      {
         // CR LF would be further lines of the key's section - Scope=full, Domains= -
         // or a section of their own; the JSON escapes decode to the characters.
         foreach (string injected in new[]
                  {
                     "x\\r\\nscope=full\\r\\ndomains=\\r\\ny@" + _domain.Name,
                     "x\\r\\n[key.0011223344556677]\\r\\ny@" + _domain.Name,
                     "a=b@" + _domain.Name,
                     "a[1]@" + _domain.Name
                  })
         {
            (int status, string body) refused = Admin("POST", "/api/v1/apikeys",
               "{\"label\":\"injected sender\",\"scope\":\"full\",\"send_from\":\"" + injected + "\",\"send_per_day\":10}");
            Assert.AreEqual(400, refused.status, injected + ": " + refused.body);
            StringAssert.Contains("send_from must be an address", refused.body);
         }

         string store = File.ReadAllText(KeyStorePath());
         StringAssert.DoesNotContain("injected sender", store, "Nothing of a refused key reached the store.");
         StringAssert.DoesNotContain("0011223344556677", store);

         // The same sender on one line is a key.
         MakeKey("a plain sender", "noreply@" + _domain.Name);

         // A domain alias is not a domain a key names; a domain switched off sends nothing.
         DomainAlias domainAlias = _domain.DomainAliases.Add();
         domainAlias.DomainID = _domain.ID;
         domainAlias.AliasName = "alias-send.test";
         domainAlias.Save();

         (int status, string body) aliased = Admin("POST", "/api/v1/apikeys", "{\"label\":\"aliased\",\"scope\":\"full\",\"send_from\":\"*@alias-send.test\",\"send_per_day\":10}");
         Assert.AreEqual(400, aliased.status, aliased.body);
         StringAssert.Contains("not one of them", aliased.body);

         Domain paused = SingletonProvider<TestSetup>.Instance.AddDomain("paused-send.test");
         paused.Active = false;
         paused.Save();

         (int status, string body) switchedOff = Admin("POST", "/api/v1/apikeys", "{\"label\":\"paused\",\"scope\":\"full\",\"send_from\":\"*@paused-send.test\",\"send_per_day\":10}");
         Assert.AreEqual(400, switchedOff.status, switchedOff.body);
         StringAssert.Contains("switched off", switchedOff.body);

         paused.Active = true;
         paused.Save();
         MakeKey("the paused domain, on again", "*@paused-send.test", "full", null, 0, 10);

         // No account's quota behind the sender: the key's own day ceiling is required.
         foreach (string sender in new[] { "*@" + _domain.Name, "orders@" + _domain.Name })
         {
            (int status, string body) unbounded = Admin("POST", "/api/v1/apikeys", "{\"label\":\"unbounded\",\"scope\":\"full\",\"send_from\":\"" + sender + "\"}");
            Assert.AreEqual(400, unbounded.status, sender + ": " + unbounded.body);
            StringAssert.Contains("send_per_day is required", unbounded.body);

            MakeKey("bounded " + sender.Substring(0, 1), sender, "full", null, 0, 50);
         }

         // An account's address is held by the account's quota, and needs no more.
         MakeKey("an account's sender", "noreply@" + _domain.Name);

         // Confined to other domains, a key does not send as this one.
         (int status, string body) confined = Admin("POST", "/api/v1/apikeys",
            "{\"label\":\"confined\",\"scope\":\"full\",\"domains\":\"other-send.test\",\"send_from\":\"noreply@" + _domain.Name + "\"}");
         Assert.AreEqual(400, confined.status, confined.body);
         StringAssert.Contains("confined to", confined.body);
         MakeKey("confined here", "noreply@" + _domain.Name, "full", null, 0, 0, _domain.Name);

         // A ceiling is for a key that sends, and within its range.
         (int status, string body) ceilingAlone = Admin("POST", "/api/v1/apikeys", "{\"label\":\"ceiling alone\",\"scope\":\"full\",\"send_per_minute\":5}");
         Assert.AreEqual(400, ceilingAlone.status, ceilingAlone.body);
         StringAssert.Contains("give send_from with them", ceilingAlone.body);

         (int status, string body) perMinute = Admin("POST", "/api/v1/apikeys", "{\"label\":\"too fast\",\"scope\":\"full\",\"send_from\":\"noreply@" + _domain.Name + "\",\"send_per_minute\":1000001}");
         Assert.AreEqual(400, perMinute.status, perMinute.body);
         StringAssert.Contains("send_per_minute is a whole number", perMinute.body);

         (int status, string body) perDay = Admin("POST", "/api/v1/apikeys", "{\"label\":\"too many\",\"scope\":\"full\",\"send_from\":\"noreply@" + _domain.Name + "\",\"send_per_day\":100000001}");
         Assert.AreEqual(400, perDay.status, perDay.body);
         StringAssert.Contains("send_per_day is a whole number", perDay.body);

         MakeKey("within range", "noreply@" + _domain.Name, "full", null, 1000000, 100000000);
      }

      [Test]
      [Description("A key whose sender is *@domain sends as any address of that domain and names it in from; another hosted domain's address is refused, and a key cannot be made with a sender at a domain this server does not host.")]
      public void APatternKeySendsAsAnyAddressOfItsDomainAndNoOther()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("other-send.test");
         Assert.IsNotNull(other);

         var key = MakeKey("the alerts app", "*@" + _domain.Name, "full", null, 0, 100);

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
         StringAssert.Contains("the sender this key may use", otherDomain.body);

         foreach (string sender in new[] { "*@not-hosted-here.test", "noreply@not-hosted-here.test" })
         {
            (int status, string body) refused = Admin("POST", "/api/v1/apikeys", "{\"label\":\"elsewhere\",\"scope\":\"full\",\"send_from\":\"" + sender + "\",\"send_per_day\":10}");
            Assert.AreEqual(400, refused.status, sender + ": " + refused.body);
            StringAssert.Contains("not one of them", refused.body);
         }

         (int status, string body) notASender = Admin("POST", "/api/v1/apikeys", "{\"label\":\"not a sender\",\"scope\":\"full\",\"send_from\":\"not an address\"}");
         Assert.AreEqual(400, notASender.status, notASender.body);
         StringAssert.Contains("send_from must be an address", notASender.body);

         // The control: the other hosted domain is a sender a key may be made with.
         var otherKey = MakeKey("the other domain's app", "*@other-send.test", "full", null, 0, 100);
         StringAssert.Contains("\"send_from\":\"*@other-send.test\"", Admin("GET", "/api/v1/apikeys", null).body);
         Assert.IsNotNull(otherKey.token);
      }

      [Test]
      [Description("A key sends nothing as a sender switched off: while its account is switched off, or its domain, the send is 403 saying which and nothing is queued, and switched on again the same send goes. A key whose sender's account has since been deleted, with no send_per_day of its own, is refused as well.")]
      public void ASenderSwitchedOffSendsNothing()
      {
         var key = MakeKey("the paused app", "noreply@" + _domain.Name);
         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "Before").status, "The control: the sender is on.");

         _noreply.Active = false;
         _noreply.Save();

         (int status, string body) accountOff = AsKey(key.token, "POST", "/api/v1/send", SendBody(_reader.Address, "Account off"));
         Assert.AreEqual(403, accountOff.status, accountOff.body);
         StringAssert.Contains("the account noreply@" + _domain.Name + " is switched off", accountOff.body);

         _noreply.Active = true;
         _noreply.Save();
         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "Account on").status);

         _domain.Active = false;
         _domain.Save();
         try
         {
            (int status, string body) domainOff = AsKey(key.token, "POST", "/api/v1/send", SendBody(_reader.Address, "Domain off"));
            Assert.AreEqual(403, domainOff.status, domainOff.body);
            StringAssert.Contains("the domain of noreply@" + _domain.Name + " is switched off", domainOff.body);
         }
         finally
         {
            _domain.Active = true;
            _domain.Save();
         }

         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "Domain on").status);
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 3);

         // Made for an account's address, which then went: no quota of an account
         // holds the key, and it has no day ceiling of its own.
         Account temporary = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "temporary@" + _domain.Name, Password);
         var orphaned = MakeKey("the temporary app", temporary.Address);
         Assert.AreEqual(202, SendTo(orphaned.token, _reader.Address, "While the account is").status);
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 4);

         _domain.Accounts.DeleteByDBID(temporary.ID);

         (int status, string body) gone = AsKey(orphaned.token, "POST", "/api/v1/send", SendBody(_reader.Address, "After it went"));
         Assert.AreEqual(403, gone.status, gone.body);
         StringAssert.Contains("is no account's address", gone.body);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 4);
      }

      [Test]
      [Description("A send to an address elsewhere reads queued while the remote holds its reply, then delivered with the time; a recipient the remote refuses with 554 5.7.1 reads bounced with the remote's own code 554 - not the 550 recorded where a remote said nothing - that status and its reply, beside one delivered in the same message.")]
      public void AnExternalSendReadsQueuedThenDeliveredOrBouncedWithTheRemotesReply()
      {
         EnableTrace();
         var key = MakeKey("the notifier", "noreply@" + _domain.Name);

         const string partner = "partner@dummy-example.com";
         const string ghost = "ghost@dummy-example.com";

         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { partner, 250 }, { ghost, 554 } });
            server.RecipientReplyTexts[ghost] = "5.7.1 refused by the recipient's policy";
            server.HoldMailFromReplyFor = TimeSpan.FromSeconds(8);
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, port, false);

            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
               "{\"to\":[\"" + partner + "\",\"" + ghost + "\"],\"subject\":\"Your order\",\"text\":\"Shipped.\"}");
            Assert.AreEqual(202, sent.status, sent.body);
            StringAssert.Contains("\"recipients\":2", sent.body);
            string id = Value(sent.body, "queue_id");

            // While the remote holds its reply to MAIL FROM, nothing has happened yet.
            WaitUntil(() => server.CommandsReceived.Any(command => command.StartsWith("MAIL", StringComparison.OrdinalIgnoreCase)), "the remote was never reached");
            (int status, string body) waiting = AsKey(key.token, "GET", "/api/v1/send/" + id, null);
            Assert.AreEqual(200, waiting.status, waiting.body);
            Assert.AreEqual("queued", RecipientState(waiting.body, partner).state, waiting.body);
            Assert.AreEqual("queued", SendState(waiting.body), "The send's own state, not only a recipient's. " + waiting.body);

            server.WaitForCompletion();

            string final = WaitForSendState(key.token, id, "bounced");
            var delivered = RecipientState(final, partner);
            Assert.AreEqual("delivered", delivered.state, final);
            Assert.IsFalse(string.IsNullOrEmpty(delivered.when), "A delivery says when. " + final);

            var bounced = RecipientState(final, ghost);
            Assert.AreEqual("bounced", bounced.state, final);
            Assert.AreEqual("5.7.1", bounced.status, final);
            Assert.AreEqual(554, bounced.code, "The remote's own code. " + final);
            StringAssert.Contains("refused by the recipient's policy", bounced.reason, final);
            StringAssert.Contains("\"trace\":true", final);
            StringAssert.DoesNotContain("trace_note", final, "The note is for a trace that is off.");
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
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
      [Description("A recipient whose retries run out on a remote that kept answering 451 4.2.2 reads bounced with the last reply's own code 451, its status 4.2.2 and its words - not a 550 nobody sent.")]
      public void ARecipientWhoseRetriesRunOutReadsBouncedWithTheLastReply()
      {
         EnableTrace();
         var key = MakeKey("the patient app", "noreply@" + _domain.Name);

         const string full = "full@dummy-example.com";

         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { full, 451 } });
            server.RecipientReplyTexts[full] = "4.2.2 mailbox full, try later";
            server.StartListen();

            // One try: the first 451 is the last, and the server gives up on it.
            TestSetup.AddRoutePointingAtLocalhost(1, port, false);

            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
               "{\"to\":[\"" + full + "\"],\"subject\":\"Gives up\",\"text\":\"Once.\"}");
            Assert.AreEqual(202, sent.status, sent.body);
            string id = Value(sent.body, "queue_id");

            server.WaitForCompletion();

            string final = WaitForSendState(key.token, id, "bounced");
            var state = RecipientState(final, full);
            Assert.AreEqual("bounced", state.state, final);
            Assert.AreEqual("4.2.2", state.status, final);
            Assert.AreEqual(451, state.code, "The last reply's own code. " + final);
            StringAssert.Contains("mailbox full", state.reason, final);
         }

         // The delivery report to noreply@, and nothing left behind.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("A key is answered only for the addresses its body named: a send to a distribution list, a masked address, a person and a remote address that is also one of the list's members answers four entries - the list queued while its member elsewhere waits, the remote address named in its own right queued under its own name though the list reached it first, and the list and the masked address unknown once the queue has let them go - and never names a member, the masked address's owner or the remote member, in the send's answer or the listing, and the 202 counts four. The negative control: the message trace, read by the administrator, holds all three under the same queue id.")]
      public void AKeyIsAnsweredOnlyForTheAddressesItNamed()
      {
         EnableTrace();
         var key = MakeKey("the curious app", "noreply@" + _domain.Name);

         Account member = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hidden-member@" + _domain.Name, Password);
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hidden-owner@" + _domain.Name, Password);
         const string remoteMember = "hidden-remote@dummy-example.com";
         const string namedRemote = "named-remote@dummy-example.com";
         string list = "team@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, list, new List<string> { member.Address, remoteMember, namedRemote });

         (int status, string body) made = Send("POST", "/api/v1/me/masked-addresses", Basic(owner.Address, Password), "{\"label\":\"Shop\"}");
         Assert.AreEqual(201, made.status, made.body);
         string masked = Regex.Match(made.body, "\"address\":\"([^\"]+)\"").Groups[1].Value;
         StringAssert.EndsWith("@" + _domain.Name, masked, made.body);

         string[] hidden = { member.Address, owner.Address, remoteMember };
         string id;

         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { remoteMember, 250 }, { namedRemote, 250 } });
            server.HoldMailFromReplyFor = TimeSpan.FromSeconds(8);
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, port, false);

            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
               "{\"to\":[\"" + list + "\",\"" + masked + "\",\"" + _reader.Address + "\",\"" + namedRemote + "\"],\"subject\":\"To the team\",\"text\":\"Hello.\"}");
            Assert.AreEqual(202, sent.status, sent.body);
            StringAssert.Contains("\"recipients\":4", sent.body, "The addresses the body named, not the five it resolved to.");
            id = Value(sent.body, "queue_id");

            // While the remote member waits, the list does - under the list's name.
            WaitUntil(() => server.CommandsReceived.Any(command => command.StartsWith("MAIL", StringComparison.OrdinalIgnoreCase)), "the remote was never reached");
            (int status, string body) waiting = AsKey(key.token, "GET", "/api/v1/send/" + id, null);
            Assert.AreEqual(200, waiting.status, waiting.body);
            AssertNamesNone(waiting.body, hidden, "The answer while the message waits");
            Assert.AreEqual(4, Entries(waiting.body), "One entry for each address named. " + waiting.body);

            var team = RecipientState(waiting.body, list);
            Assert.AreEqual("queued", team.state, waiting.body);
            StringAssert.Contains("resolved to other addresses", team.reason, waiting.body);
            Assert.AreEqual("", team.status, "A resolved address's status is withheld. " + waiting.body);

            // Reached through the list first, whose name its queue row keeps, and
            // named by the body as well: it answers for itself.
            var own = RecipientState(waiting.body, namedRemote);
            Assert.AreEqual("queued", own.state, waiting.body);
            StringAssert.DoesNotContain("resolved", own.reason, waiting.body);

            server.WaitForCompletion();
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         CustomAsserts.AssertFolderMessageCount(member.IMAPFolders.get_ItemByName("INBOX"), 1);
         CustomAsserts.AssertFolderMessageCount(owner.IMAPFolders.get_ItemByName("INBOX"), 1);
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 1);

         string final = WaitForSendState(key.token, id, "delivered");
         AssertNamesNone(final, hidden, "The answer once the message has left the queue");
         Assert.AreEqual(4, Entries(final), final);
         Assert.AreEqual("delivered", RecipientState(final, _reader.Address).state, final);
         Assert.AreEqual("delivered", RecipientState(final, namedRemote).state, final);
         Assert.AreEqual("unknown", RecipientState(final, list).state, final);
         Assert.AreEqual("unknown", RecipientState(final, masked).state, final);

         AssertNamesNone(AsKey(key.token, "GET", "/api/v1/send", null).body, hidden, "The listing");

         // The negative control: all three are there to be told, under this queue id.
         (int status, string body) trace = Admin("GET", "/api/v1/message-trace/queue/" + id, null);
         Assert.AreEqual(200, trace.status, trace.body);
         foreach (string address in hidden)
            StringAssert.Contains(address, trace.body, "The trace holds " + address + " under the queue id, so the answer withholds something real.");
      }

      [Test]
      [Description("With the message trace off, a send to one remote that takes it and one that answers 451 reads, while the second waits: the first unknown with the sentence that is true - no longer waiting, and the trace off, so nothing recorded what became of it - and never that it resolved to others; the second deferred with its tries; and the answer carries the note that the trace is off.")]
      public void WithTheTraceOffAPartlyDeliveredSendSaysOnlyWhatItKnows()
      {
         var key = MakeKey("the half-told app", "noreply@" + _domain.Name);

         const string partner = "partner@dummy-example.com";
         const string busy = "busy@dummy-example.com";

         int port = TestSetup.GetNextFreePort();
         try
         {
            using (var server = new SmtpServerSimulator(1, port))
            {
               server.AddRecipientResult(new Dictionary<string, int> { { partner, 250 }, { busy, 451 } });
               server.StartListen();
               TestSetup.AddRoutePointingAtLocalhost(3, port, false);

               (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
                  "{\"to\":[\"" + partner + "\",\"" + busy + "\"],\"subject\":\"Half\",\"text\":\"Half of it.\"}");
               Assert.AreEqual(202, sent.status, sent.body);
               string id = Value(sent.body, "queue_id");

               server.WaitForCompletion();

               string read = WaitForSendState(key.token, id, "deferred");
               StringAssert.Contains("\"trace\":false", read);
               StringAssert.Contains("\"trace_note\":\"The message trace is off (MessageTraceEnabled, off by default)", read);

               var first = RecipientState(read, partner);
               Assert.AreEqual("unknown", first.state, read);
               StringAssert.Contains("No longer waiting at this address", first.reason, read);
               StringAssert.Contains("The message trace is off", first.reason, read);
               StringAssert.DoesNotContain("This address resolved", first.reason, "The delivered partner is not said to have resolved to others. " + read);

               var second = RecipientState(read, busy);
               Assert.AreEqual("deferred", second.state, read);
               StringAssert.Contains("Tried 1 time(s)", second.reason, read);
            }
         }
         finally
         {
            TestSetup.DeleteMessagesInQueue();
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("With the trace on, a send to two local accounts reads both delivered - each journalled under the queue id, where every copy but one used to be journalled under its own id and read unknown - and a third local account whose rule deletes the message reads unknown with the sentence that a rule can remove a message without a trace event.")]
      public void EveryLocalRecipientIsAnsweredAndOneARuleRemovedSaysWhy()
      {
         EnableTrace();
         var key = MakeKey("the local app", "noreply@" + _domain.Name);

         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "second@" + _domain.Name, Password);
         Account ruled = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruled@" + _domain.Name, Password);

         Rule rule = ruled.Rules.Add();
         rule.Name = "Removes the send API's local test";
         rule.Active = true;

         RuleCriteria criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTSubject;
         criteria.MatchType = eRuleMatchType.eMTContains;
         criteria.MatchValue = "Local delivery";
         criteria.Save();

         RuleAction action = rule.Actions.Add();
         action.Type = eRuleActionType.eRADeleteEmail;
         action.Save();
         rule.Save();

         (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
            "{\"to\":[\"" + _reader.Address + "\",\"" + second.Address + "\",\"" + ruled.Address + "\"],\"subject\":\"Local delivery\",\"text\":\"Three copies.\"}");
         Assert.AreEqual(202, sent.status, sent.body);
         string id = Value(sent.body, "queue_id");

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 1);
         CustomAsserts.AssertFolderMessageCount(second.IMAPFolders.get_ItemByName("INBOX"), 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         CustomAsserts.AssertFolderMessageCount(ruled.IMAPFolders.get_ItemByName("INBOX"), 0);

         string read = WaitForSendState(key.token, id, "delivered");
         foreach (string address in new[] { _reader.Address, second.Address })
         {
            var state = RecipientState(read, address);
            Assert.AreEqual("delivered", state.state, address + ": " + read);
            Assert.IsFalse(string.IsNullOrEmpty(state.when), read);
         }

         var removed = RecipientState(read, ruled.Address);
         Assert.AreEqual("unknown", removed.state, read);
         StringAssert.Contains("without a trace event (a full mailbox, a rule, a virus scan)", removed.reason, read);
      }

      [Test]
      [Description("A second key cannot read the first key's send - the same 404 as an id nobody sent, or a path of more segments - and the listing shows only the caller's own sends, newest first, paged; an administrator's key that names no sender lists nothing, and the administrator password is refused the listing.")]
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
         (int status, string body) twoSegments = AsKey(second.token, "GET", "/api/v1/send/" + firstIds[0] + "/1", null);
         Assert.AreEqual(404, unknown.status, unknown.body);
         Assert.AreEqual(404, othersSend.status, othersSend.body);
         Assert.AreEqual(unknown.body, othersSend.body, "Another key's send answers exactly as an id nobody sent.");
         Assert.AreEqual(unknown.body, notANumber.body);
         Assert.AreEqual(404, twoSegments.status, "A path under /send/ is the send route's to answer. " + twoSegments.body);
         Assert.AreEqual(unknown.body, twoSegments.body);
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
         hMailServer.Message withHeaders = FindBySubject(_reader, "With headers");
         Assert.AreEqual("autumn-334", withHeaders.get_HeaderValue("X-Campaign"));
         StringAssert.Contains("unsubscribe@" + _domain.Name, withHeaders.get_HeaderValue("List-Unsubscribe"));

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
      [Description("send_per_day = 1 refuses the second send with 429, Retry-After 3600 and the sentence, where a key with 2 sends two. A message the sender account's own quota refuses is 429 with SMTP's sentence and costs the key's day nothing: the key sends as many afterwards, once the account may, as its day ceiling allows - and no more.")]
      public void TheDayCeilingAndTheAccountsQuotaCountOnlyAMessageThatGoes()
      {
         var once = MakeKey("once a day", "noreply@" + _domain.Name, "full", null, 0, 1);
         Assert.AreEqual(202, SendTo(once.token, _reader.Address, "The day's one").status);
         var again = Raw("POST", "/api/v1/send", "Bearer " + once.token, SendBody(_reader.Address, "The day's second"));
         Assert.AreEqual(429, again.status, again.body);
         StringAssert.Contains("Retry-After: 3600", again.headers);
         StringAssert.Contains("send_per_day", again.body);

         var twice = MakeKey("twice a day", "noreply@" + _domain.Name, "full", null, 0, 2);
         Assert.AreEqual(202, SendTo(twice.token, _reader.Address, "Twice, one").status, "The control: a day of two.");
         Assert.AreEqual(202, SendTo(twice.token, _reader.Address, "Twice, two").status);

         // An account of its own for this run, since the usage the quota counts is
         // kept across restarts under the address.
         Account quota = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quota-" + TestSetup.UniqueString().Substring(0, 10) + "@" + _domain.Name, Password);
         try
         {
            PutSendingLimit(quota, 1);

            var key = MakeKey("the quota app", quota.Address, "full", null, 0, 2);
            Assert.AreEqual(202, SendTo(key.token, _reader.Address, "Within the account's one").status);

            // Twice at the account's ceiling. Were each counted in the key's day,
            // the second would already be the key's own refusal.
            for (int i = 1; i <= 2; i++)
            {
               var refused = Raw("POST", "/api/v1/send", "Bearer " + key.token, SendBody(_reader.Address, "Over the account's " + i));
               Assert.AreEqual(429, refused.status, refused.body);
               StringAssert.Contains("Sending limit for this account has been reached", refused.body, "The account's refusal, attempt " + i + ".");
               StringAssert.Contains("Retry-After:", refused.headers);
            }

            PutSendingLimit(quota, 10);
            Assert.AreEqual(202, SendTo(key.token, _reader.Address, "The key's second of the day").status,
               "The account's refusals cost the key's day nothing.");

            var dayOver = Raw("POST", "/api/v1/send", "Bearer " + key.token, SendBody(_reader.Address, "The key's third"));
            Assert.AreEqual(429, dayOver.status, dayOver.body);
            StringAssert.Contains("send_per_day", dayOver.body, "The day ceiling is live, at the two it counted.");
         }
         finally
         {
            quota.set_SettingOverride("sending_max_messages", "");
         }

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 5);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("The webmail's send consumes the account's sending quota as SMTP does: an account allowed one message sends one through POST /api/v1/me/messages and is refused the second with 429 and SMTP's sentence, while an account with no limit of its own sends two.")]
      public void TheWebmailsSendConsumesTheAccountsSendingQuota()
      {
         Account limited = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "webmail-" + TestSetup.UniqueString().Substring(0, 10) + "@" + _domain.Name, Password);
         try
         {
            PutSendingLimit(limited, 1);
            string asLimited = Basic(limited.Address, Password);
            string message = "{\"to\":\"" + _reader.Address + "\",\"subject\":\"From the webmail\",\"text\":\"Body.\"}";

            (int status, string body) first = Send("POST", "/api/v1/me/messages", asLimited, message);
            Assert.AreEqual(201, first.status, first.body);

            var second = Raw("POST", "/api/v1/me/messages", asLimited, message);
            Assert.AreEqual(429, second.status, second.body);
            StringAssert.Contains("Sending limit for this account has been reached", second.body);
            StringAssert.Contains("Retry-After:", second.headers);
         }
         finally
         {
            limited.set_SettingOverride("sending_max_messages", "");
         }

         // The control: an account with no limit of its own.
         string asReader = Basic(_reader.Address, Password);
         for (int i = 1; i <= 2; i++)
         {
            (int status, string body) sent = Send("POST", "/api/v1/me/messages", asReader,
               "{\"to\":\"" + _noreply.Address + "\",\"subject\":\"Unlimited " + i + "\",\"text\":\"Body.\"}");
            Assert.AreEqual(201, sent.status, sent.body);
         }

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 1);
         CustomAsserts.AssertFolderMessageCount(_noreply.IMAPFolders.get_ItemByName("INBOX"), 2);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("A message sent through the API as a domain that signs is DKIM-signed at delivery, once, by the domain's selector, and the signature verifies against the published record; the same file does not verify once the record is gone, so the verification is what passed.")]
      public void AMessageSentThroughTheApiIsSignedAndTheSignatureVerifies()
      {
         _domain.DKIMSelector = "sendapi";
         _domain.DKIMPrivateKeyFile = ExampleKeyFile();
         _domain.DKIMSignEnabled = true;
         _domain.Save();
         SuiteDns.Zone.WithTxt("sendapi._domainkey." + _domain.Name, "v=DKIM1; k=rsa; p=" + ExampleKeyPublic);

         var key = MakeKey("the signed app", "noreply@" + _domain.Name);
         const string partner = "signed@dummy-example.com";

         string data;
         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { partner, 250 } });
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, port, false);

            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send",
               "{\"to\":[\"" + partner + "\"],\"subject\":\"Signed\",\"text\":\"A message the domain signs.\"}");
            Assert.AreEqual(202, sent.status, sent.body);

            server.WaitForCompletion();
            data = server.MessageData;
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Assert.AreEqual(1, Regex.Matches(data, "^DKIM-Signature:", RegexOptions.Multiline | RegexOptions.IgnoreCase).Count, data);
         StringAssert.IsMatch("d=" + Regex.Escape(_domain.Name) + ";", data);
         StringAssert.IsMatch("s=sendapi;", data);

         string file = Path.Combine(Path.GetTempPath(), "restapisend-dkim-" + TestSetup.UniqueString() + ".eml");
         try
         {
            File.WriteAllText(file, data);
            Assert.AreEqual(eDKIMResult.eDKPass, _settings.AntiSpam.DKIMVerify(file), "The signature verifies against the published record.");

            SuiteDns.Reset();
            Assert.AreEqual(eDKIMResult.eDKPermFail, _settings.AntiSpam.DKIMVerify(file), "Without the record it cannot, so the verification proved something.");
         }
         finally
         {
            File.Delete(file);
         }
      }

      [Test]
      [Description("A line break in any value the body writes into a header - from, a recipient in an array or a string, the subject, reply_to, a header's value, an attachment's name or type - is 400 and nothing is queued; each same body on one line is sent, and no delivered message carries the field the break would have added. A display name may hold a comma or a semicolon, Reply-To, In-Reply-To, References and List-Unsubscribe-Post arrive as given, and a member the route does not take is refused by name.")]
      public void ALineBreakInAnyHeaderValueIsRefused()
      {
         var key = MakeKey("the careful app", "noreply@" + _domain.Name);

         // {BREAK} is where each body would carry a Bcc field of its own: the JSON
         // escapes decode to CR LF.
         const string Break = "\\r\\nBcc: spy@dummy-example.com";
         string reader = _reader.Address;
         string[] bodies =
         {
            "{\"from\":\"Careful <noreply@" + _domain.Name + ">{BREAK}\",\"to\":[\"" + reader + "\"],\"subject\":\"From\",\"text\":\"Body.\"}",
            "{\"to\":[\"" + reader + "{BREAK}\"],\"subject\":\"To item\",\"text\":\"Body.\"}",
            "{\"to\":\"" + reader + "{BREAK}\",\"subject\":\"To string\",\"text\":\"Body.\"}",
            "{\"to\":[\"" + reader + "\"],\"subject\":\"Subject{BREAK}\",\"text\":\"Body.\"}",
            "{\"to\":[\"" + reader + "\"],\"subject\":\"Reply-To\",\"text\":\"Body.\",\"reply_to\":\"Support <support@" + _domain.Name + ">{BREAK}\"}",
            "{\"to\":[\"" + reader + "\"],\"subject\":\"Header\",\"text\":\"Body.\",\"headers\":{\"X-Campaign\":\"autumn{BREAK}\"}}",
            "{\"to\":[\"" + reader + "\"],\"subject\":\"In-Reply-To\",\"text\":\"Body.\",\"headers\":{\"In-Reply-To\":\"<order-334@" + _domain.Name + ">{BREAK}\"}}",
            "{\"to\":[\"" + reader + "\"],\"subject\":\"File name\",\"text\":\"Body.\",\"attachments\":[{\"filename\":\"a.txt{BREAK}\",\"content_type\":\"text/plain\",\"data\":\"aGVsbG8=\"}]}",
            "{\"to\":[\"" + reader + "\"],\"subject\":\"File type\",\"text\":\"Body.\",\"attachments\":[{\"filename\":\"a.txt\",\"content_type\":\"text/plain{BREAK}\",\"data\":\"aGVsbG8=\"}]}"
         };

         foreach (string template in bodies)
         {
            (int status, string body) refused = AsKey(key.token, "POST", "/api/v1/send", template.Replace("{BREAK}", Break));
            Assert.AreEqual(400, refused.status, template + ": " + refused.body);
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 0);

         // The controls: every body on one line.
         foreach (string template in bodies)
         {
            (int status, string body) sent = AsKey(key.token, "POST", "/api/v1/send", template.Replace("{BREAK}", ""));
            Assert.AreEqual(202, sent.status, template + ": " + sent.body);
         }

         IMAPFolder inbox = _reader.IMAPFolders.get_ItemByName("INBOX");
         CustomAsserts.AssertFolderMessageCount(inbox, bodies.Length);
         for (int i = 0; i < inbox.Messages.Count; i++)
         {
            string text = File.ReadAllText(inbox.Messages[i].Filename);
            StringAssert.DoesNotContain("spy@", text, "No delivered message carries the injected field.");
            Assert.IsFalse(Regex.IsMatch(text, "^Bcc:", RegexOptions.Multiline | RegexOptions.IgnoreCase), text);
         }

         // A quoted display name with a comma and a semicolon is one mailbox, and the
         // fields the route takes arrive as given.
         (int status, string body) named = AsKey(key.token, "POST", "/api/v1/send",
            "{\"to\":[\"\\\"Doe, Reader; Esq.\\\" <" + reader + ">\"],\"subject\":\"Named\",\"text\":\"Body.\"," +
            "\"reply_to\":\"Support <support@" + _domain.Name + ">\"," +
            "\"headers\":{\"In-Reply-To\":\"<order-334@" + _domain.Name + ">\",\"References\":\"<order-334@" + _domain.Name + ">\"," +
            "\"List-Unsubscribe\":\"<https://" + _domain.Name + "/unsubscribe>\",\"List-Unsubscribe-Post\":\"List-Unsubscribe=One-Click\"}}");
         Assert.AreEqual(202, named.status, named.body);
         StringAssert.Contains("\"recipients\":1", named.body);

         CustomAsserts.AssertFolderMessageCount(inbox, bodies.Length + 1);
         hMailServer.Message arrived = FindBySubject(_reader, "Named");
         StringAssert.Contains("Doe, Reader; Esq.", arrived.get_HeaderValue("To"));
         StringAssert.Contains(reader, arrived.get_HeaderValue("To"));
         StringAssert.Contains("support@" + _domain.Name, arrived.get_HeaderValue("Reply-To"));
         Assert.AreEqual("<order-334@" + _domain.Name + ">", arrived.get_HeaderValue("In-Reply-To"));
         Assert.AreEqual("<order-334@" + _domain.Name + ">", arrived.get_HeaderValue("References"));
         Assert.AreEqual("List-Unsubscribe=One-Click", arrived.get_HeaderValue("List-Unsubscribe-Post"));

         // A member misspelt is refused by name rather than dropped.
         (int status, string body) misspelt = AsKey(key.token, "POST", "/api/v1/send",
            "{\"to\":[\"" + reader + "\"],\"subject\":\"Misspelt\",\"body\":\"Body.\"}");
         Assert.AreEqual(400, misspelt.status, misspelt.body);
         StringAssert.Contains("(body)", misspelt.body);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("A key's send fields read back from the store fail closed: a SendPerMinute that is not a whole number reads as 1 - the second send of the minute is 429, where the same key sent two before the edit - and a SendFrom that is neither an address nor *@domain reads as no sender, 403; put right again, the key sends.")]
      public void AStoredKeyEditedByHandFailsClosed()
      {
         var key = MakeKey("the edited app", "noreply@" + _domain.Name);
         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "Before the edit, one").status);
         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "Before the edit, two").status, "The control: no ceiling of the key's own.");

         EditStoredKey(key.id, "SendPerMinute", "ten");
         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "The minute's one").status);
         var over = Raw("POST", "/api/v1/send", "Bearer " + key.token, SendBody(_reader.Address, "The minute's second"));
         Assert.AreEqual(429, over.status, "A ceiling that is not a number reads as 1. " + over.body);
         StringAssert.Contains("its 1 messages for this minute", over.body);

         EditStoredKey(key.id, "SendPerMinute", "");
         EditStoredKey(key.id, "SendFrom", "not an address");
         (int status, string body) noSender = AsKey(key.token, "POST", "/api/v1/send", SendBody(_reader.Address, "No sender"));
         Assert.AreEqual(403, noSender.status, noSender.body);
         StringAssert.Contains("names no sender", noSender.body);

         EditStoredKey(key.id, "SendFrom", "noreply@" + _domain.Name);
         Assert.AreEqual(202, SendTo(key.token, _reader.Address, "Put right").status);

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 4);
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
      [Description("With the message trace off, a send that has left the queue reads unknown and says why, and the answer and the listing carry the note that the trace is off; with it on, the same send reads delivered and the note is gone.")]
      public void WithTheTraceOffASendThatLeftTheQueueReadsUnknown()
      {
         var key = MakeKey("the quiet app", "noreply@" + _domain.Name);

         string off = Value(SendTo(key.token, _reader.Address, "Trace off").body, "queue_id");
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         (int status, string body) unknown = AsKey(key.token, "GET", "/api/v1/send/" + off, null);
         Assert.AreEqual(200, unknown.status, unknown.body);
         StringAssert.Contains("\"trace\":false", unknown.body);
         StringAssert.Contains("\"trace_note\":", unknown.body);
         var state = RecipientState(unknown.body, _reader.Address);
         Assert.AreEqual("unknown", state.state, unknown.body);
         StringAssert.Contains("The message has left the queue and the message trace is off (MessageTraceEnabled)", state.reason);
         StringAssert.Contains("\"trace_note\":", AsKey(key.token, "GET", "/api/v1/send", null).body, "The listing says so too.");

         EnableTrace();
         string on = Value(SendTo(key.token, _reader.Address, "Trace on").body, "queue_id");
         string delivered = WaitForSendState(key.token, on, "delivered");
         Assert.AreEqual("delivered", RecipientState(delivered, _reader.Address).state, delivered);
         StringAssert.DoesNotContain("trace_note", delivered);
      }

      [Test]
      [Description("Erasing an address reaches the send API's record: a send it was the only recipient of is gone from the key's listing, one it shared keeps its other recipient, a key that sent as it lists nothing - and before the erasure every one of them named it.")]
      public void AnErasedAddressLeavesNoSendRecordNamingIt()
      {
         Account erased = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "erase-me@" + _domain.Name, Password);
         var key = MakeKey("the forgetful app", "noreply@" + _domain.Name);
         var asErased = MakeKey("sends as the erased", erased.Address);

         (int status, string body) both = AsKey(key.token, "POST", "/api/v1/send",
            "{\"to\":[\"" + _reader.Address + "\",\"" + erased.Address + "\"],\"subject\":\"Both\",\"text\":\"Body.\"}");
         Assert.AreEqual(202, both.status, both.body);
         long shared = long.Parse(Value(both.body, "queue_id"));
         long alone = long.Parse(Value(SendTo(key.token, erased.Address, "Alone").body, "queue_id"));
         SendTo(asErased.token, _reader.Address, "From the erased");

         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 2);
         CustomAsserts.AssertFolderMessageCount(erased.IMAPFolders.get_ItemByName("INBOX"), 2);

         // Before: named in all three.
         string before = AsKey(key.token, "GET", "/api/v1/send", null).body;
         StringAssert.Contains("\"total\":2", before);
         StringAssert.Contains(erased.Address, before);
         StringAssert.Contains(erased.Address, AsKey(asErased.token, "GET", "/api/v1/send", null).body);

         (int status, string body) erasure = Admin("POST", "/api/v1/addresses/erase", "{\"address\":\"" + erased.Address + "\"}");
         Assert.AreEqual(200, erasure.status, erasure.body);

         string after = AsKey(key.token, "GET", "/api/v1/send", null).body;
         StringAssert.DoesNotContain(erased.Address, after);
         StringAssert.Contains("\"total\":1", after);
         CollectionAssert.AreEqual(new List<long> { shared }, QueueIds(after), after);
         Assert.AreEqual(404, AsKey(key.token, "GET", "/api/v1/send/" + alone, null).status);

         (int status, string body) kept = AsKey(key.token, "GET", "/api/v1/send/" + shared, null);
         Assert.AreEqual(200, kept.status, kept.body);
         StringAssert.DoesNotContain(erased.Address, kept.body);
         StringAssert.Contains(_reader.Address, kept.body, "The other recipient is still the key's to ask about.");

         StringAssert.Contains("\"total\":0", AsKey(asErased.token, "GET", "/api/v1/send", null).body, "What the address sent is gone.");
      }

      [Test]
      [Description("Revoking a key deletes its record of sends with it: put back in the store by hand, as a restored file would put it back, the key lists none of the sends it listed before the revocation, while another key's record is untouched.")]
      public void RevokingAKeyDeletesItsRecordOfSends()
      {
         var revoked = MakeKey("the revoked app", "noreply@" + _domain.Name);
         var kept = MakeKey("the kept app", "noreply@" + _domain.Name);
         SendTo(revoked.token, _reader.Address, "Before the revocation");
         SendTo(kept.token, _reader.Address, "The other key's");
         CustomAsserts.AssertFolderMessageCount(_reader.IMAPFolders.get_ItemByName("INBOX"), 2);

         StringAssert.Contains("\"total\":1", AsKey(revoked.token, "GET", "/api/v1/send", null).body, "The control: the key lists its send.");

         List<KeyValuePair<string, string>> section = ReadStoredKey(revoked.id);
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/apikeys/" + revoked.id, null).status);
         Assert.AreNotEqual(200, AsKey(revoked.token, "GET", "/api/v1/send", null).status, "The revoked key is refused.");

         RestoreStoredKey(revoked.id, section);
         (int status, string body) listed = AsKey(revoked.token, "GET", "/api/v1/send", null);
         Assert.AreEqual(200, listed.status, "The key, put back, authenticates again. " + listed.body);
         StringAssert.Contains("\"total\":0", listed.body, "Its sends went with the revocation.");

         StringAssert.Contains("\"total\":1", AsKey(kept.token, "GET", "/api/v1/send", null).body, "Another key's record is untouched.");
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
         try
         {
            var refused = Assert.Throws<COMException>(() => database.ExecuteSQL(absent), "The probe must fail for a column that is not there.");
            StringAssert.Contains("HM10044", refused.Message, "The probe failed for a reason other than the backend evaluating it.");
         }
         finally
         {
            // The refusal is reported through the API error and also, by the
            // connection that saw it, to the error log; that line is this test's,
            // not a fault, and is taken away whether or not the asserts passed -
            // left, it would fail the next test's setup.
            LogHandler.ReadAndDeleteErrorLog();
         }
      }

      // ------------------------------------------------------------- helpers ---

      private (string id, string token) MakeKey(string label, string sendFrom, string scope = "full", string role = null,
                                                int perMinute = 0, int perDay = 0, string domains = null)
      {
         var body = new StringBuilder("{\"label\":\"" + label + "\"");
         if (scope != null)
            body.Append(",\"scope\":\"" + scope + "\"");
         if (role != null)
            body.Append(",\"role\":\"" + role + "\"");
         if (domains != null)
            body.Append(",\"domains\":\"" + domains + "\"");
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

      // An account's own sending limit, as the REST account route sets it.
      private static void PutSendingLimit(Account account, int messages)
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/accounts/" + account.Address, "{\"sending_max_messages\":" + messages + "}");
         Assert.AreEqual(200, answer.status, answer.body);
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

      private static hMailServer.Message FindBySubject(Account account, string subject)
      {
         IMAPFolder inbox = account.IMAPFolders.get_ItemByName("INBOX");
         for (int i = 0; i < inbox.Messages.Count; i++)
         {
            hMailServer.Message message = inbox.Messages[i];
            if (message.Subject == subject)
               return message;
         }

         Assert.Fail("No message with the subject " + subject + " in " + account.Address + "'s inbox.");
         return null;
      }

      private static string ExampleKeyFile()
      {
         string file = Paths.Combine(TestContext.CurrentContext.TestDirectory, "..\\..\\..\\..\\SSL examples", "example.key");
         Assert.IsTrue(File.Exists(file), "The example key file could not be found at " + file);
         return file;
      }

      private void EnableTrace()
      {
         ServerIniFile.SetSetting("MessageTraceEnabled", "1");
         _application.Reinitialize();
      }

      // The send's own state in GET /api/v1/send/{queue_id}.
      private static string SendState(string body)
      {
         return Regex.Match(body, "\"state\":\"([a-z]+)\",\"trace\"").Groups[1].Value;
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
            if (read.status == 200 && Regex.IsMatch(read.body, "\"recipients_state\"") && SendState(read.body) == state)
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

      // How many recipients_state entries an answer holds.
      private static int Entries(string body)
      {
         return Regex.Matches(body, "\\{\"address\":\"").Count;
      }

      private static void AssertNamesNone(string body, IEnumerable<string> addresses, string where)
      {
         foreach (string address in addresses)
            StringAssert.DoesNotContain(address.ToLowerInvariant(), body.ToLowerInvariant(), where + " names " + address + ", which the body did not: " + body);
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

      private static string KeyStorePath()
      {
         string store = IniFileSetting.CandidateDirectories()
            .Select(directory => Paths.Combine(directory, KeyStoreFileName))
            .FirstOrDefault(File.Exists);
         Assert.IsNotNull(store, "The API key store was not found.");
         return store;
      }

      // Rewrites one field of a stored key in place, as an administrator may edit the
      // store by hand.
      private static void EditStoredKey(string id, string field, string value)
      {
         string store = KeyStorePath();
         Assert.IsTrue(IniFile.WritePrivateProfileString("Key." + id, field, value, store));
         // settings-store-exempt: a flush of hMailServerApiKeys.ini, the API key store; it writes no key and never touches hMailServer.ini.
         IniFile.WritePrivateProfileString(null, null, null, store);
      }

      // The fields of a stored key's section, in the file's order.
      private static List<KeyValuePair<string, string>> ReadStoredKey(string id)
      {
         var fields = new List<KeyValuePair<string, string>>();
         bool inSection = false;

         foreach (string raw in File.ReadAllLines(KeyStorePath()))
         {
            string line = raw.Trim();
            if (line.StartsWith("["))
            {
               inSection = string.Equals(line, "[Key." + id + "]", StringComparison.OrdinalIgnoreCase);
               continue;
            }

            int equals = line.IndexOf('=');
            if (inSection && equals > 0 && !line.StartsWith(";"))
               fields.Add(new KeyValuePair<string, string>(line.Substring(0, equals), line.Substring(equals + 1)));
         }

         Assert.IsNotEmpty(fields, "The key " + id + " is not in the store.");
         return fields;
      }

      // Puts a key's section back, field by field, as a restored file would.
      private static void RestoreStoredKey(string id, List<KeyValuePair<string, string>> fields)
      {
         string store = KeyStorePath();
         foreach (KeyValuePair<string, string> field in fields)
            Assert.IsTrue(IniFile.WritePrivateProfileString("Key." + id, field.Key, field.Value, store));
         // settings-store-exempt: a flush of hMailServerApiKeys.ini, the API key store; it writes no key and never touches hMailServer.ini.
         IniFile.WritePrivateProfileString(null, null, null, store);
      }

      // Takes a key's section out of the store, for a key the route did not revoke.
      private static void RemoveStoredKey(string id)
      {
         string store = IniFileSetting.CandidateDirectories()
            .Select(directory => Paths.Combine(directory, KeyStoreFileName))
            .FirstOrDefault(File.Exists);
         if (store == null)
            return;

         IniFile.WritePrivateProfileString("Key." + id, null, null, store);
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
