// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The other accounts a reader adds in the webmail: /api/v1/me/fetch-accounts,
   ///    the administrator's fetch accounts narrowed to what a reader may set. A
   ///    host on a private address is refused until an administrator allows it; a
   ///    field that would switch a filter off or read recipients out of the
   ///    collected mail is refused by name, and the account saved has the filters
   ///    on whatever was sent; an interval under 15 minutes and a fourth account
   ///    are refused; an account another reader owns is not found; an account an
   ///    administrator set up with what a reader cannot set is seen and not
   ///    changed; and a collection started from the page brings the mail in.
   /// </summary>
   [TestFixture]
   public class RestApiReaderFetchAccounts : TestFixtureBase
   {
      private static int RestPort = 9614;
      private const string AdminPassword = "testar";
      private const string ReaderPassword = "Reader-Fetch-Passw0rd!";

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
            Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":3,\"reader_fetch_accounts_private_addresses\":false}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A reader's other account: a private host refused until allowed, an administrator's field refused by name and the filters forced on, the interval floor, the ceiling, another reader's and an administrator's account kept out of reach, and a collection from the page that brings the mail in.")]
      public void AReaderCollectsFromAnotherAccountWithinTheServersLimits()
      {
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-fetch@" + _domain.Name, ReaderPassword);
         Account neighbour = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "neighbour-fetch@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);

         var messages = new List<string>
         {
            "From: old-friend@dummy-example.com\r\nTo: reader@old-provider.test\r\nSubject: Collected from the old provider\r\n\r\nHello from before.\r\n"
         };
         int port = TestSetup.GetNextFreePort();
         string body = "{\"name\":\"Old provider\",\"server_address\":\"localhost\",\"port\":" + port +
                       ",\"server_type\":\"pop3\",\"username\":\"old\",\"password\":\"old-password\",\"connection_security\":\"none\",\"days_to_keep_messages\":-1}";

         // A private host, before an administrator allows one.
         (int status, string body) refused = Send("POST", "/api/v1/me/fetch-accounts", me, body);
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("not a public address", refused.body);
         // Which address the host leads to is not the reader's to learn: the
         // refusal names the host as they gave it (until 6.3.6 it named the
         // address).
         StringAssert.Contains("localhost", refused.body);
         StringAssert.DoesNotContain("127.0.0.1", refused.body, "The refusal named the address the host led to.");
         StringAssert.DoesNotContain("::1", refused.body, "The refusal named the address the host led to.");
         Assert.AreEqual(0, reader.FetchAccounts.Count, "Nothing is saved when the host is refused.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);

         // What only an administrator sets.
         (int status, string body) routed = Send("POST", "/api/v1/me/fetch-accounts", me,
            body.Substring(0, body.Length - 1) + ",\"process_mime_recipients\":true}");
         Assert.AreEqual(400, routed.status, routed.body);
         StringAssert.Contains("process_mime_recipients", routed.body);
         (int status, string body) filterOff = Send("POST", "/api/v1/me/fetch-accounts", me,
            body.Substring(0, body.Length - 1) + ",\"use_antispam\":false}");
         Assert.AreEqual(400, filterOff.status, filterOff.body);

         // Too often.
         (int status, string body) tooOften = Send("POST", "/api/v1/me/fetch-accounts", me,
            body.Substring(0, body.Length - 1) + ",\"minutes_between_fetch\":5}");
         Assert.AreEqual(400, tooOften.status, tooOften.body);
         StringAssert.Contains("at least 15", tooOften.body);
         Assert.AreEqual(0, reader.FetchAccounts.Count);

         using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
         {
            pop3Server.StartListen();

            (int status, string body) created = Send("POST", "/api/v1/me/fetch-accounts", me, body);
            Assert.AreEqual(201, created.status, created.body);
            StringAssert.Contains("\"use_antispam\":true", created.body);
            StringAssert.Contains("\"use_antivirus\":true", created.body);
            StringAssert.Contains("\"process_mime_recipients\":false", created.body);
            StringAssert.Contains("\"enable_route_recipients\":false", created.body);
            string id = NumberAfter(created.body, "\"id\":");

            FetchAccount viaCom = reader.FetchAccounts.get_Item(0);
            Assert.IsTrue(viaCom.UseAntiSpam, "The filters are on as COM reads the account.");
            Assert.IsTrue(viaCom.UseAntiVirus);
            Assert.IsFalse(viaCom.ProcessMIMERecipients);

            // Another reader cannot see or collect it.
            string neighbourAuth = Basic(neighbour.Address, ReaderPassword);
            Assert.AreEqual(404, Send("GET", "/api/v1/me/fetch-accounts/" + id, neighbourAuth, null).status);
            Assert.AreEqual(404, Send("POST", "/api/v1/me/fetch-accounts/" + id + "/download", neighbourAuth, null).status);
            StringAssert.DoesNotContain("Old provider", Send("GET", "/api/v1/me/fetch-accounts", neighbourAuth, null).body);

            // Collected now, from the page's button.
            (int status, string body) queued = Send("POST", "/api/v1/me/fetch-accounts/" + id + "/download", me, null);
            Assert.AreEqual(202, queued.status, queued.body);
            pop3Server.WaitForCompletion();
            Pop3ClientSimulator.AssertMessageCount(reader.Address, ReaderPassword, 1);
            StringAssert.Contains("Collected from the old provider", Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, ReaderPassword));

            DateTime deadline = DateTime.UtcNow.AddSeconds(30);
            while (DateTime.UtcNow < deadline && Send("GET", "/api/v1/me/fetch-accounts/" + id, me, null).body.Contains("\"locked\":true"))
               Thread.Sleep(100);

            // Paused, and back.
            (int status, string body) paused = Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"enabled\":false}");
            Assert.AreEqual(200, paused.status, paused.body);
            StringAssert.Contains("\"enabled\":false", paused.body);
            StringAssert.Contains("\"use_antispam\":true", paused.body, "A change keeps the filters on.");

            // The ceiling: one more is refused once the reader has as many as allowed.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":1}").status);
            (int status, string body) second = Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace("Old provider", "A second one"));
            Assert.AreEqual(403, second.status, second.body);
            StringAssert.Contains("at most 1", second.body);

            // Not offered at all.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":0}").status);
            Assert.AreEqual(403, Send("GET", "/api/v1/me/fetch-accounts", me, null).status);
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":3}").status);

            Assert.AreEqual(200, Send("DELETE", "/api/v1/me/fetch-accounts/" + id, me, null).status);
            Assert.AreEqual(0, reader.FetchAccounts.Count);
         }

         // An administrator's account, set up to read recipients from the message: seen, not changed or removed.
         FetchAccount administrators = reader.FetchAccounts.Add();
         administrators.Name = "Catch-all";
         administrators.ServerAddress = "localhost";
         administrators.Port = 9;
         administrators.MinutesBetweenFetch = 60;
         administrators.ProcessMIMERecipients = true;
         administrators.Enabled = false;
         administrators.Save();

         (int status, string body) listed = Send("GET", "/api/v1/me/fetch-accounts", me, null);
         StringAssert.Contains("Catch-all", listed.body);
         Assert.AreEqual(403, Send("PUT", "/api/v1/me/fetch-accounts/" + administrators.ID, me, "{\"name\":\"Mine now\"}").status);
         Assert.AreEqual(403, Send("DELETE", "/api/v1/me/fetch-accounts/" + administrators.ID, me, null).status);
         Assert.AreEqual("Catch-all", reader.FetchAccounts.get_ItemByDBID(administrators.ID).Name);
      }

      /// <summary>
      ///    OUT-1. A fetch account for a host on FetchOAuth2Hosts logs in with this
      ///    server's own client_credentials token, as whatever username it names -
      ///    every mailbox the app registration was granted. Before the fix a reader
      ///    made one for outlook.office365.com with the CEO's address, or renamed the
      ///    user on one an administrator had set up, and collected (and, with
      ///    days_to_keep_messages -1, deleted) that mailbox; a domain-restricted key
      ///    and a domain administrator over COM could do the same. Each of the
      ///    refusals below was a 201, a 200 or a successful Save against the old
      ///    code, which compared server_address with nothing; the server
      ///    administrator's own create and change are the control.
      /// </summary>
      [Test]
      [Description("Only a server administrator makes or changes a fetch account for a host on FetchOAuth2Hosts: a reader, a domain-restricted key and a domain administrator over COM are refused, the server administrator is not.")]
      public void OnlyAServerAdministratorGivesOutTheServersOAuthToken()
      {
         const string TokenHost = "m365-fetch.example.test";
         IniFileSetting.Write("FetchOAuth2Hosts", TokenHost);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status);
         // No name lookup: the host is judged by name, not by where it resolves.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);

         string keyId = null;
         try
         {
            Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-token@" + _domain.Name, ReaderPassword);
            string me = Basic(reader.Address, ReaderPassword);
            string body = "{\"name\":\"Theirs\",\"server_address\":\"" + TokenHost + "\",\"port\":993,\"server_type\":\"imap\"," +
                          "\"username\":\"ceo@contoso.example\",\"connection_security\":\"tls\",\"days_to_keep_messages\":0,\"enabled\":false}";

            (int status, string body) byReader = Send("POST", "/api/v1/me/fetch-accounts", me, body);
            Assert.AreEqual(403, byReader.status, byReader.body);
            StringAssert.Contains("FetchOAuth2Hosts", byReader.body);
            Assert.AreEqual(403, Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace(TokenHost, TokenHost.ToUpperInvariant())).status,
               "The host is compared without regard to case, as the fetcher compares it.");
            Assert.AreEqual(0, reader.FetchAccounts.Count, "Nothing is saved when the host is refused.");

            // A reader's own account elsewhere, moved onto the host.
            string elsewhere = body.Replace(TokenHost, "imap.elsewhere.example");
            (int status, string body) own = Send("POST", "/api/v1/me/fetch-accounts", me, elsewhere);
            Assert.AreEqual(201, own.status, own.body);
            string ownId = NumberAfter(own.body, "\"id\":");
            (int status, string body) moved = Send("PUT", "/api/v1/me/fetch-accounts/" + ownId, me, "{\"server_address\":\"" + TokenHost + "\"}");
            Assert.AreEqual(403, moved.status, moved.body);
            StringAssert.Contains("FetchOAuth2Hosts", moved.body);

            // The server administrator's, with the reader's defaults - both filters
            // on, as a reader's own account has them, so that AdministratorsOwn does
            // not refuse the reader first and the refusal below is the token rule's:
            // the reader cannot rename its user, the thing the token is presented for.
            (int status, string body) byAdmin = Admin("POST", "/api/v1/accounts/" + reader.Address + "/fetch-accounts",
               body.Replace("ceo@contoso.example", reader.Address).Replace("Theirs", "Microsoft 365")
                   .Replace("\"enabled\":false}", "\"enabled\":false,\"use_antispam\":true,\"use_antivirus\":true}"));
            Assert.AreEqual(201, byAdmin.status, byAdmin.body);
            string adminsId = NumberAfter(byAdmin.body, "\"id\":");
            (int status, string body) renamed = Send("PUT", "/api/v1/me/fetch-accounts/" + adminsId, me, "{\"username\":\"ceo@contoso.example\"}");
            Assert.AreEqual(403, renamed.status, renamed.body);
            StringAssert.Contains("FetchOAuth2Hosts", renamed.body, "Refused by the token rule, not because an administrator set it up with a filter off.");
            Assert.AreEqual(reader.Address, reader.FetchAccounts.get_ItemByDBID(int.Parse(adminsId)).Username, "The refused change changed nothing.");
            Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + reader.Address + "/fetch-accounts/" + adminsId, "{\"minutes_between_fetch\":45}").status,
               "The server administrator still changes it.");

            // A key restricted to the reader's domain.
            (int keyStatus, string keyBody) = Admin("POST", "/api/v1/apikeys", "{\"label\":\"fetch token - own domain\",\"scope\":\"full\",\"domains\":\"" + _domain.Name + "\"}");
            Assert.AreEqual(201, keyStatus, keyBody);
            keyId = System.Text.RegularExpressions.Regex.Match(keyBody, "\"id\":\"([^\"]*)\"").Groups[1].Value;
            string domainKey = "Bearer " + System.Text.RegularExpressions.Regex.Match(keyBody, "\"key\":\"([^\"]*)\"").Groups[1].Value;
            Assert.AreEqual(403, Send("POST", "/api/v1/accounts/" + reader.Address + "/fetch-accounts", domainKey, body).status);
            Assert.AreEqual(403, Send("PUT", "/api/v1/accounts/" + reader.Address + "/fetch-accounts/" + adminsId, domainKey, "{\"username\":\"ceo@contoso.example\"}").status);
            Assert.AreEqual(200, Send("PUT", "/api/v1/accounts/" + reader.Address + "/fetch-accounts/" + ownId, domainKey, "{\"minutes_between_fetch\":60}").status,
               "The same key still changes a fetch account for any other host.");

            // A domain administrator over COM.
            Account domainAdmin = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "domain-admin-token@" + _domain.Name, "test");
            domainAdmin.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
            domainAdmin.Save();
            var asDomainAdmin = new hMailServer.Application();
            Assert.IsNotNull(asDomainAdmin.Authenticate(domainAdmin.Address, "test"));
            FetchAccount viaDomainAdmin = asDomainAdmin.Domains[0].Accounts.get_ItemByAddress(reader.Address).FetchAccounts.Add();
            viaDomainAdmin.Name = "Theirs over COM";
            viaDomainAdmin.ServerAddress = TokenHost;
            viaDomainAdmin.Port = 993;
            viaDomainAdmin.Username = "ceo@contoso.example";
            viaDomainAdmin.Enabled = false;
            Assert.Throws<System.Runtime.InteropServices.COMException>(() => viaDomainAdmin.Save());
            viaDomainAdmin.ServerAddress = "imap.elsewhere.example";
            viaDomainAdmin.Save();

            Assert.AreEqual(3, reader.FetchAccounts.Count, "The reader's own, the administrator's and the domain administrator's for another host.");
         }
         finally
         {
            if (keyId != null)
               Admin("DELETE", "/api/v1/apikeys/" + keyId, null);
            IniFileSetting.Write("FetchOAuth2Hosts", "outlook.office365.com");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    OUT-4. The username and password go into command lines to the remote
      ///    server - POP3 "USER %s" and "PASS %s", IMAP LOGIN in a quoted string that
      ///    escaped only the backslash and the quote - so a CR LF in either ended the
      ///    command and sent what followed as another, to whatever host and port the
      ///    reader chose. Against the old code the reader's create was a 201, the
      ///    administrator's too, and the COM Save succeeded; the account with a plain
      ///    username is the control.
      /// </summary>
      [Test]
      [Description("A username or password with a CR, LF or NUL is refused by the reader's route, the administrator's route and COM.")]
      public void AFetchAccountsCredentialsAreOneLineEach()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);

         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-lines@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);
         string body = "{\"name\":\"Lines\",\"server_address\":\"localhost\",\"port\":110,\"server_type\":\"pop3\"," +
                       "\"username\":\"old\",\"password\":\"old-password\",\"enabled\":false}";

         foreach (string injected in new[] { "old\\r\\nDELE 1", "old\\nDELE 1", "old\\rDELE 1" })
         {
            (int status, string body) user = Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace("\"username\":\"old\"", "\"username\":\"" + injected + "\""));
            Assert.AreEqual(400, user.status, injected + ": " + user.body);
            StringAssert.Contains("one line", user.body);
            Assert.AreEqual(400, Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace("old-password", injected)).status, injected);
            Assert.AreEqual(400, Admin("POST", "/api/v1/accounts/" + reader.Address + "/fetch-accounts", body.Replace("\"username\":\"old\"", "\"username\":\"" + injected + "\"")).status, injected);
         }
         Assert.AreEqual(0, reader.FetchAccounts.Count, "Nothing is saved when a credential is refused.");

         (int status, string body) plain = Send("POST", "/api/v1/me/fetch-accounts", me, body);
         Assert.AreEqual(201, plain.status, plain.body);
         string id = NumberAfter(plain.body, "\"id\":");
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"password\":\"new\\r\\nDELE 1\"}").status, "A change is judged as a new one is.");

         FetchAccount viaCom = reader.FetchAccounts.Add();
         viaCom.Name = "Lines over COM";
         viaCom.ServerAddress = "localhost";
         viaCom.Port = 110;
         viaCom.Enabled = false;
         viaCom.Username = "old\r\nDELE 1";
         Assert.Throws<System.Runtime.InteropServices.COMException>(() => viaCom.Save());
         viaCom.Username = "old";
         viaCom.Password = "old\nDELE 1";
         Assert.Throws<System.Runtime.InteropServices.COMException>(() => viaCom.Save());
         viaCom.Password = "old-password";
         viaCom.Save();

         Assert.AreEqual(2, reader.FetchAccounts.Count);
      }

      /// <summary>
      ///    OUT-4, the rest. A reader's host was judged public once, when it was
      ///    saved, by the system resolver; the fetcher later resolved it again with
      ///    the server's own DNS and connected to whatever came back. A name whose
      ///    owner answers a public address at the save and 127.0.0.1 afterwards (DNS
      ///    rebinding), or one the server's DNSServer answers differently (split
      ///    horizon), reached the inside of the network every interval. The account a
      ///    reader makes is now marked reader_created and the fetcher keeps only the
      ///    public addresses it resolved, unless the administrator allows private
      ///    hosts. The rebinding is played here by the setting: saved while private
      ///    hosts were allowed, collected after they no longer are. Against the old
      ///    code the collection went ahead and the message arrived; the collection
      ///    with the setting on again, and the administrator's account over COM that
      ///    is not marked, are the controls.
      /// </summary>
      [Test]
      [Description("A reader's account is judged where the fetcher connects: a private address it resolves to is not connected to unless private hosts are allowed.")]
      public void AReadersAccountIsJudgedWhereItConnects()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);

         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-rebind@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);

         var messages = new List<string>
         {
            "From: old-friend@dummy-example.com\r\nTo: reader@old-provider.test\r\nSubject: Collected once allowed\r\n\r\nHello.\r\n"
         };
         int port = TestSetup.GetNextFreePort();

         using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
         {
            pop3Server.StartListen();

            (int status, string body) created = Send("POST", "/api/v1/me/fetch-accounts", me,
               "{\"name\":\"Rebinding\",\"server_address\":\"localhost\",\"port\":" + port +
               ",\"server_type\":\"pop3\",\"username\":\"old\",\"password\":\"old-password\",\"connection_security\":\"none\",\"days_to_keep_messages\":-1,\"enabled\":false}");
            Assert.AreEqual(201, created.status, created.body);
            StringAssert.Contains("\"reader_created\":true", created.body, "An account a reader adds is marked as a reader's.");
            string id = NumberAfter(created.body, "\"id\":");

            // The name now leads inside the network, and private hosts are not
            // allowed. Switched on only now, so that no collection ran before.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":false}").status);
            Assert.AreEqual(200, Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"enabled\":true}").status);
            Assert.AreEqual(202, Send("POST", "/api/v1/me/fetch-accounts/" + id + "/download", me, null).status);

            CustomAsserts.AssertReportedError("which a reader added in the webmail", "not a public address");
            WaitUntilUnlocked(me, id);
            Assert.AreEqual("", pop3Server.Conversation, "The fetcher connected to a private address for a reader's account.");
            Pop3ClientSimulator.AssertMessageCount(reader.Address, ReaderPassword, 0);

            // Allowed again: the same account is collected.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);
            Assert.AreEqual(202, Send("POST", "/api/v1/me/fetch-accounts/" + id + "/download", me, null).status);
            pop3Server.WaitForCompletion();
            Pop3ClientSimulator.AssertMessageCount(reader.Address, ReaderPassword, 1);
            WaitUntilUnlocked(me, id);
         }

         // An administrator's account is not marked, and not judged.
         FetchAccount administrators = reader.FetchAccounts.Add();
         administrators.Name = "Inside, by the administrator";
         administrators.ServerAddress = "localhost";
         administrators.Port = 9;
         administrators.MinutesBetweenFetch = 60;
         administrators.UseAntiSpam = true;
         administrators.UseAntiVirus = true;
         administrators.Enabled = false;
         administrators.Save();
         (int status, string body) listed = Admin("GET", "/api/v1/accounts/" + reader.Address + "/fetch-accounts/" + administrators.ID, null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"reader_created\":false", listed.body);

         // A reader's rename of it marks nothing; moving it to another port does.
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/fetch-accounts/" + administrators.ID, me, "{\"name\":\"Renamed by the reader\"}").status);
         StringAssert.Contains("\"reader_created\":false", Admin("GET", "/api/v1/accounts/" + reader.Address + "/fetch-accounts/" + administrators.ID, null).body);
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/fetch-accounts/" + administrators.ID, me, "{\"port\":110}").status);
         StringAssert.Contains("\"reader_created\":true", Admin("GET", "/api/v1/accounts/" + reader.Address + "/fetch-accounts/" + administrators.ID, null).body);
      }

      /// <summary>
      ///    OUT-4: a reader's account named any port from 1 to 65535, so the server
      ///    knocked, from inside its network, on whatever service a reader chose and
      ///    sent it a username and a password. A reader now names 110, 995, 143 or
      ///    993, unless the administrator allows private hosts (an old mail server
      ///    inside the network, on a port of its own). Against the old code the first
      ///    create below was a 201.
      /// </summary>
      [Test]
      [Description("A reader's account is collected from the POP3 and IMAP ports only, unless private hosts are allowed.")]
      public void AReadersAccountUsesThePop3AndImapPorts()
      {
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-ports@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);
         // A public address written as one: judged without a name lookup, and never
         // connected to, since the account is off.
         string body = "{\"name\":\"Ports\",\"server_address\":\"8.8.8.8\",\"port\":25,\"server_type\":\"pop3\"," +
                       "\"username\":\"old\",\"password\":\"old-password\",\"enabled\":false}";

         foreach (int refused in new[] { 25, 6379, 8080, 1 })
         {
            (int status, string body) made = Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace("\"port\":25", "\"port\":" + refused));
            Assert.AreEqual(400, made.status, refused + ": " + made.body);
            StringAssert.Contains("port", made.body);
         }
         Assert.AreEqual(0, reader.FetchAccounts.Count, "Nothing is saved when the port is refused.");

         (int status, string body) pop3s = Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace("\"port\":25", "\"port\":995"));
         Assert.AreEqual(201, pop3s.status, pop3s.body);
         string id = NumberAfter(pop3s.body, "\"id\":");
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"port\":8080}").status, "A change is judged as a new one is.");
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"port\":993,\"server_type\":\"imap\"}").status);

         // The administrator's own route is not limited.
         Assert.AreEqual(201, Admin("POST", "/api/v1/accounts/" + reader.Address + "/fetch-accounts", body.Replace("Ports", "Administrator's")).status);

         // Private hosts allowed: any port with them.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);
         Assert.AreEqual(201, Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace("\"port\":25", "\"port\":1110").Replace("Ports", "Inside")).status);
      }

      /// <summary>
      ///    The fetchers' side of OUT-1, which the check of the first fix left open:
      ///    the save refuses a reader a host on FetchOAuth2Hosts, but a host the
      ///    administrator adds to the list afterwards - or a row saved before the
      ///    save refused it - still got this server's token, as whatever username the
      ///    reader had named. An account a reader made now never gets it, and logs in
      ///    with its own USER and PASS. Against the old code the POP3 server below
      ///    received the XOAUTH2 blob.
      /// </summary>
      [Test]
      [Description("A reader's account on a host added to FetchOAuth2Hosts later logs in with its own password, never this server's token.")]
      public void AReadersAccountNeverGetsTheServersToken()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);

         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-later-host@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);
         int port = TestSetup.GetNextFreePort();

         (int status, string body) created = Send("POST", "/api/v1/me/fetch-accounts", me,
            "{\"name\":\"Later on the list\",\"server_address\":\"localhost\",\"port\":" + port +
            ",\"server_type\":\"pop3\",\"username\":\"ceo@contoso.example\",\"password\":\"old-password\",\"connection_security\":\"tls\",\"days_to_keep_messages\":-1,\"enabled\":false}");
         Assert.AreEqual(201, created.status, created.body);
         string id = NumberAfter(created.body, "\"id\":");

         IniFileSetting.Write("FetchOAuth2Hosts", "localhost");
         IniFileSetting.Write("OutboundOAuth2FixedToken", "fetch-bearer-for-admins");
         _application.Reinitialize();
         try
         {
            Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status);
            var messages = new List<string> { "Subject: Collected with the reader's password\r\n\r\nBody.\r\n" };
            using (var pop3Server = new Pop3ServerSimulator(1, port, messages, eConnectionSecurity.eCSTLS))
            {
               pop3Server.SetCertificate(RegressionTests.SSL.SslSetup.GetCertificate());
               pop3Server.StartListen();

               // Switched on by the server administrator over COM - the reader's own
               // route now refuses any change to an account on the list - which keeps
               // the mark: the account was still made on a reader's word.
               FetchAccount viaCom = reader.FetchAccounts.get_ItemByDBID(int.Parse(id));
               viaCom.Enabled = true;
               viaCom.Save();
               viaCom.DownloadNow();
               pop3Server.WaitForCompletion();

               Assert.IsEmpty(pop3Server.XOAuth2Blob, "This server's token went to a reader's account.");
               Assert.IsTrue(pop3Server.UserCommandReceived, "A reader's account logs in with its own username and password.");
            }
         }
         finally
         {
            IniFileSetting.Write("FetchOAuth2Hosts", "outlook.office365.com");
            IniFileSetting.Write("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    The rest of the fetchers' side of OUT-1. Marking only what a reader made
      ///    left the other callers without the server's authority: an account a key
      ///    restricted to named domains or a domain administrator over COM set up -
      ///    or one whose user name a reader changed on an administrator's account -
      ///    for a host the administrator put on FetchOAuth2Hosts afterwards logged in
      ///    with this server's own token, as whatever user name they had given. Each
      ///    is now server_token_withheld; a rename or a pause marks nothing, and a
      ///    server administrator's account is the control. Against the previous code
      ///    no listing carried server_token_withheld, and the POP3 server below
      ///    received the XOAUTH2 blob for the domain key's account.
      /// </summary>
      [Test]
      [Description("An account whose server or user name somebody without the server's authority named never gets this server's token, even for a host added to FetchOAuth2Hosts afterwards.")]
      public void AnAccountOthersNamedNeverGetsTheServersToken()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);

         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-others-named@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);
         string collection = "/api/v1/accounts/" + reader.Address + "/fetch-accounts";
         int port = TestSetup.GetNextFreePort();
         string body = "{\"name\":\"NAME\",\"server_address\":\"localhost\",\"port\":" + port + ",\"server_type\":\"pop3\"," +
                       "\"username\":\"" + reader.Address + "\",\"password\":\"old-password\",\"connection_security\":\"tls\"," +
                       "\"days_to_keep_messages\":-1,\"enabled\":false,\"use_antispam\":true,\"use_antivirus\":true}";
         Func<string, string> withheld = id =>
         {
            (int status, string body) got = Admin("GET", collection + "/" + id, null);
            Assert.AreEqual(200, got.status, got.body);
            return got.body.Contains("\"server_token_withheld\":true") ? "withheld" :
                   got.body.Contains("\"server_token_withheld\":false") ? "given" : "not reported: " + got.body;
         };

         // The server administrator's: given, and a reader's rename leaves it so;
         // a reader's change of the user name does not.
         string admins = NumberAfter(Admin("POST", collection, body.Replace("NAME", "The administrator's")).body, "\"id\":");
         Assert.AreEqual("given", withheld(admins));
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/fetch-accounts/" + admins, me, "{\"name\":\"Renamed by the reader\"}").status);
         Assert.AreEqual("given", withheld(admins), "A rename names neither the server nor the user.");
         string renamedUser = NumberAfter(Admin("POST", collection, body.Replace("NAME", "User renamed")).body, "\"id\":");
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/fetch-accounts/" + renamedUser, me, "{\"username\":\"ceo@contoso.example\"}").status);
         Assert.AreEqual("withheld", withheld(renamedUser), "A reader named the user the token would be presented as.");
         StringAssert.Contains("\"reader_created\":false", Admin("GET", collection + "/" + renamedUser, null).body,
            "Only the user changed: where it connects is still the administrator's word.");

         // A key restricted to the reader's domain.
         string keyId = null;
         try
         {
            (int keyStatus, string keyBody) = Admin("POST", "/api/v1/apikeys", "{\"label\":\"fetch origin - own domain\",\"scope\":\"full\",\"domains\":\"" + _domain.Name + "\"}");
            Assert.AreEqual(201, keyStatus, keyBody);
            keyId = System.Text.RegularExpressions.Regex.Match(keyBody, "\"id\":\"([^\"]*)\"").Groups[1].Value;
            string domainKey = "Bearer " + System.Text.RegularExpressions.Regex.Match(keyBody, "\"key\":\"([^\"]*)\"").Groups[1].Value;

            (int status, string body) byKey = Send("POST", collection, domainKey, body.Replace("NAME", "The domain key's").Replace(reader.Address, "ceo@contoso.example"));
            Assert.AreEqual(201, byKey.status, byKey.body);
            string keys = NumberAfter(byKey.body, "\"id\":");
            Assert.AreEqual("withheld", withheld(keys));
            Assert.AreEqual(200, Send("PUT", collection + "/" + admins, domainKey, "{\"minutes_between_fetch\":60}").status);
            Assert.AreEqual("given", withheld(admins), "A change of the interval names neither the server nor the user.");

            // A domain administrator over COM: one made, and one whose user it changes.
            Account domainAdmin = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "domain-admin-origin@" + _domain.Name, "test");
            domainAdmin.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
            domainAdmin.Save();
            var asDomainAdmin = new hMailServer.Application();
            Assert.IsNotNull(asDomainAdmin.Authenticate(domainAdmin.Address, "test"));
            FetchAccounts theirs = asDomainAdmin.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(reader.Address).FetchAccounts;
            FetchAccount made = theirs.Add();
            made.Name = "The domain administrator's";
            made.ServerAddress = "localhost";
            made.Port = port;
            made.Username = "ceo@contoso.example";
            made.UseAntiSpam = true;
            made.UseAntiVirus = true;
            made.Enabled = false;
            made.Save();
            Assert.AreEqual("withheld", withheld(made.ID.ToString()));
            FetchAccount paused = theirs.get_ItemByDBID(int.Parse(admins));
            paused.Enabled = false;
            paused.Save();
            Assert.AreEqual("given", withheld(admins), "A domain administrator's pause names neither the server nor the user.");
            paused.Username = "ceo@contoso.example";
            paused.Save();
            Assert.AreEqual("withheld", withheld(admins));

            // A save of a copy read before the mark does not take it away.
            FetchAccount stale = reader.FetchAccounts.get_ItemByDBID(int.Parse(renamedUser));
            Assert.AreEqual(200, Send("PUT", "/api/v1/me/fetch-accounts/" + renamedUser, me, "{\"port\":995}").status);
            stale.MinutesBetweenFetch = 90;
            stale.Save();
            StringAssert.Contains("\"reader_created\":true", Admin("GET", collection + "/" + renamedUser, null).body,
               "A server administrator's save of a copy read before a reader moved the account took the mark away.");

            // The host goes on the list afterwards: the domain key's account still logs
            // in with its own USER and PASS.
            IniFileSetting.Write("FetchOAuth2Hosts", "localhost");
            IniFileSetting.Write("OutboundOAuth2FixedToken", "fetch-bearer-for-admins");
            _application.Reinitialize();
            try
            {
               Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status);
               var messages = new List<string> { "Subject: Collected with the account's own password\r\n\r\nBody.\r\n" };
               using (var pop3Server = new Pop3ServerSimulator(1, port, messages, eConnectionSecurity.eCSTLS))
               {
                  pop3Server.SetCertificate(RegressionTests.SSL.SslSetup.GetCertificate());
                  pop3Server.StartListen();

                  FetchAccount viaCom = reader.FetchAccounts.get_ItemByDBID(int.Parse(keys));
                  viaCom.Enabled = true;
                  viaCom.Save();
                  Assert.AreEqual("withheld", withheld(keys), "The server administrator's own save keeps the mark.");
                  viaCom.DownloadNow();
                  pop3Server.WaitForCompletion();

                  Assert.IsEmpty(pop3Server.XOAuth2Blob, "This server's token went to an account a domain key set up.");
                  Assert.IsTrue(pop3Server.UserCommandReceived, "The account logs in with its own username and password.");
               }
            }
            finally
            {
               IniFileSetting.Write("FetchOAuth2Hosts", "outlook.office365.com");
               IniFileSetting.Write("OutboundOAuth2FixedToken", "");
               _application.Reinitialize();
            }
         }
         finally
         {
            if (keyId != null)
               Admin("DELETE", "/api/v1/apikeys/" + keyId, null);
         }
      }

      /// <summary>
      ///    The second security review of 6.3.6 (WEB636B-1): a webmail or Control Deck
      ///    page loaded before the upgrade goes on running after it - the restart
      ///    ends its session and it signs in again in place - and it sends -1 for what
      ///    its reader meant as "Leave the mail there" (the webmail) or "keep every
      ///    message" (the Deck's hint), which 6.3.6 reads as delete each message on the
      ///    other server once it is collected. The 6.3.6 pages say days_to_keep_meaning
      ///    6106 beside the value; a negative value from a browser session without it
      ///    is refused with 409, on the reader's routes, the import's start and the
      ///    administrator's routes, and nothing is saved. A password and an API key,
      ///    which send the documented meaning, are not asked for it. Against
      ///    8f2dec98b every 409 below was a 201 or a 200 that stored -1.
      /// </summary>
      [Test]
      [Description("A negative days_to_keep_messages from a browser session without days_to_keep_meaning - a page loaded before 6.3.6 - is refused with 409 and stores nothing; with it, -1 is stored, and a password's or an API key's -1 is stored without it.")]
      public void APageLoadedBeforeTheUpgradeCannotHaveTheMailDeleted()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true,\"reader_fetch_accounts_max\":10}").status);
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-stale@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);
         string page = SessionHeaders(me);

         int port = TestSetup.GetNextFreePort();
         string stale = "{\"name\":\"From a stale page\",\"server_address\":\"localhost\",\"port\":" + port +
                        ",\"server_type\":\"imap\",\"username\":\"old\",\"password\":\"old-password\",\"connection_security\":\"none\",\"days_to_keep_messages\":-1,\"enabled\":false}";
         string marked = stale.Substring(0, stale.Length - 1) + ",\"days_to_keep_meaning\":6106}";

         // The reader's page.
         (int status, string body) refused = FromPage("POST", "/api/v1/me/fetch-accounts", page, stale);
         Assert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains("page_out_of_date", refused.body);
         StringAssert.Contains("Reload the page", refused.body);
         Assert.AreEqual(0, reader.FetchAccounts.Count, "A refused page's account was saved.");

         (int status, string body) made = FromPage("POST", "/api/v1/me/fetch-accounts", page, marked);
         Assert.AreEqual(201, made.status, made.body);
         StringAssert.Contains("\"days_to_keep_messages\":-1", made.body);
         string id = NumberAfter(made.body, "\"id\":");
         Assert.AreEqual(1, reader.FetchAccounts.Count);

         Assert.AreEqual(200, FromPage("PUT", "/api/v1/me/fetch-accounts/" + id, page, "{\"days_to_keep_messages\":0}").status,
            "Leaving the mail there needs no meaning said: no page meant 0 as delete everything.");
         (int status, string body) staleChange = FromPage("PUT", "/api/v1/me/fetch-accounts/" + id, page, "{\"days_to_keep_messages\":-1}");
         Assert.AreEqual(409, staleChange.status, staleChange.body);
         StringAssert.Contains("\"days_to_keep_messages\":0", Send("GET", "/api/v1/me/fetch-accounts/" + id, me, null).body, "A refused change was saved.");
         Assert.AreEqual(200, FromPage("PUT", "/api/v1/me/fetch-accounts/" + id, page, "{\"days_to_keep_messages\":-1,\"days_to_keep_meaning\":6106}").status);
         StringAssert.Contains("\"days_to_keep_messages\":-1", Send("GET", "/api/v1/me/fetch-accounts/" + id, me, null).body);
         Assert.AreEqual(200, FromPage("PUT", "/api/v1/me/fetch-accounts/" + id, page, "{\"enabled\":false}").status,
            "A change that names no number of days is not asked what it means.");

         (int status, string body) wrongMeaning = FromPage("PUT", "/api/v1/me/fetch-accounts/" + id, page, "{\"days_to_keep_messages\":-1,\"days_to_keep_meaning\":6105}");
         Assert.AreEqual(400, wrongMeaning.status, wrongMeaning.body);
         StringAssert.Contains("days_to_keep_meaning", wrongMeaning.body);

         // The import's start: the account its callback makes keeps the value. The
         // provider is not set up here, so a start that passes is refused after.
         (int status, string body) staleImport = FromPage("POST", "/api/v1/me/fetch-accounts/import", page, "{\"provider\":\"google\",\"days_to_keep_messages\":-1}");
         Assert.AreEqual(409, staleImport.status, staleImport.body);
         StringAssert.Contains("page_out_of_date", staleImport.body);
         Assert.AreEqual(403, FromPage("POST", "/api/v1/me/fetch-accounts/import", page, "{\"provider\":\"google\",\"days_to_keep_messages\":-1,\"days_to_keep_meaning\":6106}").status,
            "With the meaning said, the start goes on to the provider's own check.");
         Assert.AreEqual(403, Send("POST", "/api/v1/me/fetch-accounts/import", me, "{\"provider\":\"google\",\"days_to_keep_messages\":-1}").status,
            "A password's start is not asked what it means.");

         // The reader's password, as a script or a program sends it: the documented meaning.
         (int status, string body) byPassword = Send("POST", "/api/v1/me/fetch-accounts", me, stale.Replace("From a stale page", "By password"));
         Assert.AreEqual(201, byPassword.status, byPassword.body);
         StringAssert.Contains("\"days_to_keep_messages\":-1", byPassword.body);

         // The administrator's routes: the Deck's session, the password and a key.
         string deck = SessionHeaders(Basic("Administrator", AdminPassword));
         string adminRoute = "/api/v1/accounts/" + reader.Address + "/fetch-accounts";
         int before = reader.FetchAccounts.Count;
         (int status, string body) staleDeck = FromPage("POST", adminRoute, deck, stale.Replace("From a stale page", "Deck"));
         Assert.AreEqual(409, staleDeck.status, staleDeck.body);
         Assert.AreEqual(before, reader.FetchAccounts.Count, "A refused Deck's account was saved.");
         (int status, string body) deckMade = FromPage("POST", adminRoute, deck, marked.Replace("From a stale page", "Deck"));
         Assert.AreEqual(201, deckMade.status, deckMade.body);
         string deckId = NumberAfter(deckMade.body, "\"id\":");
         Assert.AreEqual(409, FromPage("PUT", adminRoute + "/" + deckId, deck, "{\"days_to_keep_messages\":-1}").status);
         Assert.AreEqual(200, FromPage("PUT", adminRoute + "/" + deckId, deck, "{\"days_to_keep_messages\":3}").status);
         StringAssert.Contains("\"days_to_keep_messages\":3", Admin("GET", adminRoute + "/" + deckId, null).body, "A refused change was saved.");

         (int status, string body) byAdministrator = Admin("POST", adminRoute, stale.Replace("From a stale page", "Administrator password"));
         Assert.AreEqual(201, byAdministrator.status, byAdministrator.body);
         StringAssert.Contains("\"days_to_keep_messages\":-1", byAdministrator.body);

         string keyId = null;
         try
         {
            (int keyStatus, string keyBody) = Admin("POST", "/api/v1/apikeys", "{\"label\":\"fetch keep meaning - full\",\"scope\":\"full\"}");
            Assert.AreEqual(201, keyStatus, keyBody);
            keyId = System.Text.RegularExpressions.Regex.Match(keyBody, "\"id\":\"([^\"]*)\"").Groups[1].Value;
            string key = "Bearer " + System.Text.RegularExpressions.Regex.Match(keyBody, "\"key\":\"([^\"]*)\"").Groups[1].Value;
            (int status, string body) byKey = Send("POST", adminRoute, key, stale.Replace("From a stale page", "API key"));
            Assert.AreEqual(201, byKey.status, byKey.body);
            StringAssert.Contains("\"days_to_keep_messages\":-1", byKey.body);
            string keysId = NumberAfter(byKey.body, "\"id\":");
            Assert.AreEqual(200, Send("PUT", adminRoute + "/" + keysId, key, "{\"days_to_keep_messages\":-1}").status, "A key's change is not asked what it means.");
         }
         finally
         {
            if (keyId != null)
               Admin("DELETE", "/api/v1/apikeys/" + keyId, null);
         }
      }

      // A browser session started with the credential, as the webmail and the Deck
      // start one: the cookie, and the header every change on a session carries.
      private static string SessionHeaders(string authorization)
      {
         (int status, string headers, string body) started = RestRequest.Exchange("127.0.0.1", RestPort, "POST", "/api/v1/session", authorization, null, null);
         Assert.AreEqual(201, started.status, started.body);
         var cookie = System.Text.RegularExpressions.Regex.Match(started.headers, "hmailsession=([^;\r\n]+)");
         Assert.IsTrue(cookie.Success, started.headers);
         return "Cookie: hmailsession=" + cookie.Groups[1].Value + "\r\nX-Requested-With: hMailServer";
      }

      private static (int status, string body) FromPage(string method, string path, string sessionHeaders, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, null, requestBody, sessionHeaders);
      }

      private static void WaitUntilUnlocked(string authorization, string id)
      {
         DateTime deadline = DateTime.UtcNow.AddSeconds(30);
         while (DateTime.UtcNow < deadline && Send("GET", "/api/v1/me/fetch-accounts/" + id, authorization, null).body.Contains("\"locked\":true"))
            Thread.Sleep(100);
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

      private static string NumberAfter(string body, string marker)
      {
         int at = body.IndexOf(marker, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "Missing " + marker + " in: " + body);
         int start = at + marker.Length, end = start;
         while (end < body.Length && char.IsDigit(body[end]))
            end++;
         return body.Substring(start, end - start);
      }
   }
}
