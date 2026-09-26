// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Security
{
   /// <summary>
   ///    Country blocking, end to end: a geographic database this test writes, a
   ///    policy per listener and per domain, and the connections and sign-ins
   ///    those policies refuse.
   ///
   ///    THE ONE THING THAT MAKES THESE TESTS POSSIBLE. The suite connects from
   ///    127.0.0.1, and the shipped CountryBlockingExemptRanges names the loopback
   ///    address and every private network - so with the setting as it ships,
   ///    nothing the suite does could ever be judged. That is deliberate on both
   ///    sides: the exemption is a SETTING and not a hidden rule compiled into the
   ///    server, precisely so that an administrator can read it and a test can
   ///    take it away. Each test below that expects a refusal empties it, points
   ///    the server at a database that gives 127.0.0.0/8 a country, and puts both
   ///    back in TearDown.
   ///
   ///    The MMDB format itself is not tested here. It is a self-contained unit
   ///    with its own tests in the server (GeoIpDatabaseTester, which the suite
   ///    runs through Utilities.RunTestSuite): three record widths, the pointer
   ///    forms, the separator arithmetic and every refusal, against databases
   ///    written byte by byte. What is tested here is the wiring: that the server
   ///    reads the file an administrator supplies, and that the policy reaches
   ///    every listener and the sign-in path.
   /// </summary>
   [TestFixture]
   public class CountryBlocking : TestFixtureBase
   {
      // The countries the fixture's database gives. 127.0.0.0/8 is where the
      // suite connects from, so it is the one every refusal test turns on.
      private const string LoopbackCountry = "SE";
      private const string OtherCountry = "US";

      // CountryBlocking::DefaultExemptRanges, as docs/CountryBlocking.md prints it.
      private const string ShippedExemptRanges =
         "127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10,fe80::/10,fc00::/7";

      private string _databasePath;
      private string _exemptRangesAsShipped;
      private Account _account;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "geo@example.test", "test");

         _databasePath = Paths.Combine(_settings.Directories.DataDirectory, "GeoIP", "RegressionTests-Country.mmdb");

         var networks = new Dictionary<string, string>
         {
            { "127.0.0.0/8", LoopbackCountry },
            { "8.8.8.0/24", OtherCountry },
            { "203.0.113.0/24", "CN" }
         };

         GeoIpFixture.WriteCountryDatabase(_databasePath, networks);

         // What TearDown puts back. A run that was killed between a test's
         // JudgeTheLoopbackAddress and its TearDown leaves the bench with no
         // exemptions at all, and the next run would then "restore" that and
         // read it as the shipped value; the shipped value is what the server
         // seeds the settings row with (CountryBlocking::DefaultExemptRanges).
         _exemptRangesAsShipped = _settings.CountryBlockingExemptRanges;
         if (string.IsNullOrWhiteSpace(_exemptRangesAsShipped))
            _exemptRangesAsShipped = ShippedExemptRanges;

         _settings.GeoIpDatabaseFile = _databasePath;
      }

      [TearDown]
      public void PutTheServerBack()
      {
         // Every one of these matters to every test that runs after this fixture:
         // a policy left on with the exemption left off would refuse the whole
         // suite. TestSetup.PerformBasicSetup clears GeoIpDatabaseFile as well, as
         // a second line of defence, but this is the one that is supposed to work.
         try
         {
            _settings.set_CountryPolicy("smtp", "");
            _settings.set_CountryPolicy("imap", "");
            _settings.set_CountryPolicy("pop3", "");
            _settings.set_CountryPolicy("http", "");
            _settings.CountryBlockingExemptRanges = _exemptRangesAsShipped;
            _settings.GeoIpDatabaseFile = "";

            _domain.CountryPolicy = "";
            _domain.Save();
         }
         finally
         {
            if (!string.IsNullOrEmpty(_databasePath) && File.Exists(_databasePath))
               File.Delete(_databasePath);
         }
      }

      /// <summary>Empties the exemption, so that the loopback address is judged like anybody else's.</summary>
      private void JudgeTheLoopbackAddress()
      {
         _settings.CountryBlockingExemptRanges = "";
      }

      private static string Banner(int port)
      {
         using (var connection = new TcpConnection())
         {
            ClassicAssert.IsTrue(connection.Connect(port), "The connection to port " + port + " was not made at all.");
            return connection.Receive();
         }
      }

      private static string PopLogon(string user, string password)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(110));
            socket.ReadUntil("+OK");
            socket.Send("USER " + user + "\r\n");
            socket.ReadUntil("+OK");
            socket.Send("PASS " + password + "\r\n");
            string response = socket.ReadUntil("\r\n");
            socket.Send("QUIT\r\n");
            socket.Disconnect();
            return response;
         }
      }

      [Test]
      [Description("The server reads the database the administrator supplies: a known range answers its country, an unknown address answers nothing, and the status says what was loaded")]
      public void TheDatabaseTheAdministratorSuppliesIsRead()
      {
         ClassicAssert.AreEqual(LoopbackCountry, _application.Utilities.LookupCountry("127.0.0.1"),
            "The address the suite connects from is in the fixture's database and must answer its country.");
         ClassicAssert.AreEqual(OtherCountry, _application.Utilities.LookupCountry("8.8.8.8"));
         ClassicAssert.AreEqual("CN", _application.Utilities.LookupCountry("203.0.113.9"));

         // An address in no network at all. Not an error - the database simply has
         // nothing for it, which is what an allow list refuses and a block list
         // admits.
         ClassicAssert.AreEqual("", _application.Utilities.LookupCountry("9.9.9.9"));

         string status = _application.Utilities.GeoIpDatabaseStatus();
         StringAssert.Contains("\"loaded\":true", status);
         StringAssert.Contains("\"type\":\"GeoLite2-Country\"", status);
         StringAssert.Contains("\"ip_version\":6", status);
         StringAssert.Contains("\"record_size\":28", status);
      }

      [Test]
      [Description("A connection from a blocked country is refused on SMTP, IMAP and POP3 - closed with nothing said to the client - and the application log records each refusal with the country")]
      public void AConnectionFromABlockedCountryIsRefusedOnEveryProtocol()
      {
         JudgeTheLoopbackAddress();

         _settings.set_CountryPolicy("smtp", "block:" + LoopbackCountry);
         _settings.set_CountryPolicy("imap", "block:" + LoopbackCountry);
         _settings.set_CountryPolicy("pop3", "block:" + LoopbackCountry);

         // Nothing at all comes back: no banner, no code and no text. A client
         // that learned why it was refused would have learned the policy.
         ClassicAssert.IsEmpty(Banner(25), "An SMTP connection from a blocked country must be closed without a greeting.");
         ClassicAssert.IsEmpty(Banner(143), "An IMAP connection from a blocked country must be closed without a greeting.");
         ClassicAssert.IsEmpty(Banner(110), "A POP3 connection from a blocked country must be closed without a greeting.");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Country blocking: refused SMTP from 127.0.0.1 (" + LoopbackCountry + ")", log);
         StringAssert.Contains("Country blocking: refused IMAP from 127.0.0.1 (" + LoopbackCountry + ")", log);
         StringAssert.Contains("Country blocking: refused POP3 from 127.0.0.1 (" + LoopbackCountry + ")", log);
      }

      [Test]
      [Description("A connection from an allowed country is accepted, on an allow list that names it and on a block list that does not")]
      public void AConnectionFromAnAllowedCountryIsAccepted()
      {
         JudgeTheLoopbackAddress();

         _settings.set_CountryPolicy("smtp", "allow:" + LoopbackCountry);
         StringAssert.Contains("220", Banner(25), "An allow list that names this country must admit it.");

         _settings.set_CountryPolicy("smtp", "block:" + OtherCountry);
         StringAssert.Contains("220", Banner(25), "A block list that does not name this country must admit it.");
      }

      [Test]
      [Description("An address in the exempt ranges is never judged: the same block list that refuses it with the ranges empty admits it with the loopback range back")]
      public void AnAddressInTheExemptRangesIsNeverJudged()
      {
         JudgeTheLoopbackAddress();
         _settings.set_CountryPolicy("smtp", "block:" + LoopbackCountry);

         ClassicAssert.IsEmpty(Banner(25), "The negative control: with no exemption the policy refuses this address.");

         _settings.CountryBlockingExemptRanges = "127.0.0.0/8";
         StringAssert.Contains("220", Banner(25), "An address in the exempt ranges must not be judged by any policy.");

         // And the shipped value does the same thing, which is why a stock server
         // cannot lock its own administrator out.
         _settings.CountryBlockingExemptRanges = _exemptRangesAsShipped;
         StringAssert.Contains("220", Banner(25));
      }

      [Test]
      [Description("An open, authenticated session is not disturbed by a policy that starts refusing its country half way through")]
      public void AnAuthenticatedSessionIsNotDisturbed()
      {
         JudgeTheLoopbackAddress();

         var client = new ImapClientSimulator();

         try
         {
            ClassicAssert.IsTrue(client.ConnectAndLogon("geo@example.test", "test"),
               "The session must be established while nothing is being judged.");

            // Now refuse this country on IMAP. Nothing consults the policy again
            // for a connection that has already been accepted, so this session
            // must go on working - which is what makes it safe to change a policy
            // on a running server.
            _settings.set_CountryPolicy("imap", "block:" + LoopbackCountry);

            ClassicAssert.IsTrue(client.SelectFolder("INBOX"),
               "A session that was already authenticated must not be affected by a policy change.");
            StringAssert.Contains("OK", client.SendSingleCommand("A99 NOOP"));

            // The negative control: a NEW connection is refused, so the policy is
            // certainly in force.
            ClassicAssert.IsEmpty(Banner(143), "A new connection must be refused while the old one still works.");
         }
         finally
         {
            client.Disconnect();
         }
      }

      [Test]
      [Description("A missing database allows everything and says so once in the application log, rather than locking the administrator out")]
      public void AMissingDatabaseAllowsEverything()
      {
         JudgeTheLoopbackAddress();

         // The strictest policy there is - an allow list naming a country this
         // address is not in - against a file that is not there.
         _settings.set_CountryPolicy("smtp", "allow:" + OtherCountry);
         _settings.GeoIpDatabaseFile = Paths.Combine(_settings.Directories.DataDirectory, "GeoIP", "no-such-database.mmdb");

         StringAssert.Contains("220", Banner(25),
            "With no database that can be read, nothing is judged by country: a mail server that refused every " +
            "connection because a file was renamed would be a worse failure than one that stops applying a policy.");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("the geographic database could not be read", log);

         string status = _application.Utilities.GeoIpDatabaseStatus();
         StringAssert.Contains("\"loaded\":false", status);
         StringAssert.Contains("\"configured\":true", status);
      }

      [Test]
      [Description("A sign-in from a country the domain refuses gets the ordinary invalid-credentials reply, and is counted against neither the account nor the address")]
      public void ASignInFromARefusedCountryIsTheOrdinaryRefusal()
      {
         JudgeTheLoopbackAddress();

         // The listener policies stay off: this is the per-domain policy, which
         // is about signing in rather than about connecting, because a domain is
         // not known when a connection is accepted.
         string ordinaryRefusal = PopLogon("geo@example.test", "wrong");
         StringAssert.Contains("-ERR", ordinaryRefusal);

         _domain.CountryPolicy = "block:" + LoopbackCountry;
         _domain.Save();

         string refused = PopLogon("geo@example.test", "test");
         ClassicAssert.AreEqual(ordinaryRefusal, refused,
            "A sign-in refused on its country must be byte-identical to a wrong password. Anything else tells the " +
            "person on the other end what the policy is.");

         // And with the country allowed, the same credentials work - so the
         // refusal above was the policy and not a broken password.
         _domain.CountryPolicy = "allow:" + LoopbackCountry;
         _domain.Save();

         StringAssert.Contains("+OK", PopLogon("geo@example.test", "test"));
      }

      private static string PopScram(string user, string password)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(110));
            socket.ReadUntil("+OK");
            string answer = RegressionTests.POP3.Pop3SaslTestClient.AuthenticateScram(socket, user, password);

            // QUIT, and its answer read, so that the maildrop this session may hold
            // is released before the next sign-in: a refusal there must be the
            // policy's, never [IN-USE].
            socket.Send("QUIT\r\n");
            socket.Receive();
            return answer;
         }
      }

      [Test]
      [Description("SCRAM-SHA-256 meets the domain's country policy as a password sign-in does: it proves the password without passing through AccountLogon::Logon, where the policy is asked")]
      public void AScramSignInFromARefusedCountryIsRefused()
      {
         JudgeTheLoopbackAddress();

         // The negative control: with no policy the account signs in over SCRAM.
         StringAssert.StartsWith("+OK", PopScram("geo@example.test", "test"));

         _domain.CountryPolicy = "block:" + LoopbackCountry;
         _domain.Save();

         // Until 26 September 2026 this signed in: the policy was asked only on
         // the paths that hand AccountLogon::Logon a password.
         StringAssert.StartsWith("-ERR", PopScram("geo@example.test", "test"),
            "A SCRAM sign-in from a country the domain refuses was accepted.");

         _domain.CountryPolicy = "allow:" + LoopbackCountry;
         _domain.Save();

         StringAssert.StartsWith("+OK", PopScram("geo@example.test", "test"));
      }

      [Test]
      [Description("The policy of the domain the account really belongs to is asked however the name is spelled: a sign-in under one of its domain aliases meets it too")]
      public void ASignInUnderADomainAliasMeetsTheDomainsPolicy()
      {
         JudgeTheLoopbackAddress();

         DomainAlias alias = _domain.DomainAliases.Add();
         alias.DomainID = _domain.ID;
         alias.AliasName = "geo-alias.test";
         alias.Save();

         // The negative control: the alias spelling signs the account in, so a
         // refusal below is the policy and not the spelling.
         StringAssert.Contains("+OK", PopLogon("geo@geo-alias.test", "test"),
            "The account must sign in under its domain's alias while no policy is set.");

         _domain.CountryPolicy = "block:" + LoopbackCountry;
         _domain.Save();

         StringAssert.Contains("-ERR", PopLogon("geo@example.test", "test"),
            "The domain's own spelling is refused, as ASignInFromARefusedCountryIsTheOrdinaryRefusal proves.");

         // Until 26 September 2026 this signed in: the policy was looked up under
         // geo-alias.test, which has no hm_domains row and so no policy, and the
         // password was then checked against geo@example.test.
         StringAssert.Contains("-ERR", PopLogon("geo@geo-alias.test", "test"),
            "A sign-in under a domain alias escaped the domain's country policy.");
      }

      [Test]
      [Description("The IMAP master user signing in as another account meets the policy of the account signed into, not only its own")]
      public void AMasterUserSignInMeetsTheTargetsPolicy()
      {
         JudgeTheLoopbackAddress();

         // The master is geo@example.test, whose domain has no policy; the
         // mailbox it acts as is in a domain of its own.
         Domain targetDomain = SingletonProvider<TestSetup>.Instance.AddDomain("geo-target.test");
         SingletonProvider<TestSetup>.Instance.AddAccount(targetDomain, "mailbox@geo-target.test", "other");

         _settings.IMAPSASLPlainEnabled = true;
         _settings.IMAPMasterUser = "geo@example.test";

         // The negative control: with no policy anywhere the master signs in as
         // the mailbox, so a refusal below is the policy and nothing else.
         StringAssert.Contains("A01 OK", ImapAuthenticatePlain("mailbox@geo-target.test", "geo@example.test", "test"),
            "The master user must sign in as the mailbox while no policy is set.");

         targetDomain.CountryPolicy = "block:" + LoopbackCountry;
         targetDomain.Save();

         // The master's own sign-in is unaffected: its domain has no policy.
         StringAssert.Contains("+OK", PopLogon("geo@example.test", "test"));

         // Until 26 September 2026 this signed in: the policy was asked only for
         // the name that authenticated, geo@example.test.
         StringAssert.Contains("A01 NO", ImapAuthenticatePlain("mailbox@geo-target.test", "geo@example.test", "test"),
            "A master user opened a mailbox from a country the mailbox's domain refuses.");
      }

      private static string ImapAuthenticatePlain(string authzid, string authcid, string password)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(143));
            socket.ReadUntil("* OK");
            socket.Send("A01 AUTHENTICATE PLAIN\r\n");
            string continuation = socket.Receive();
            StringAssert.StartsWith("+", continuation, "The server did not ask for the SASL response.");
            socket.Send(Convert.ToBase64String(Encoding.UTF8.GetBytes(authzid + "\0" + authcid + "\0" + password)) + "\r\n");
            string result = socket.ReadUntil("A01 ");
            socket.Send("A02 LOGOUT\r\n");
            socket.ReadUntil("A02 ");
            socket.Disconnect();
            return result;
         }
      }

      [Test]
      [Description("A domain with no policy of its own judges no sign-in, whatever the listener policies say")]
      public void ADomainWithNoPolicyJudgesNothing()
      {
         JudgeTheLoopbackAddress();

         ClassicAssert.AreEqual("", _domain.CountryPolicy);
         StringAssert.Contains("+OK", PopLogon("geo@example.test", "test"));
      }

      [Test]
      [Description("Every refusal is counted, in total and by country, and the status shows both")]
      public void RefusalsAreCountedByCountry()
      {
         JudgeTheLoopbackAddress();

         string before = _application.Utilities.GeoIpDatabaseStatus();
         StringAssert.Contains("\"refusals_total\":", before);

         _settings.set_CountryPolicy("pop3", "block:" + LoopbackCountry);
         ClassicAssert.IsEmpty(Banner(110));

         string after = _application.Utilities.GeoIpDatabaseStatus();
         StringAssert.Contains("\"" + LoopbackCountry + "\":", after);
      }

      [Test]
      [Description("The policy grammar is one grammar: the same value is refused in the same words on the settings and on a domain, and a policy is stored in one spelling however it was typed")]
      public void ThePolicyGrammarIsOneGrammar()
      {
         // Stored canonically: lower-case keyword, upper-case codes, sorted, no
         // spaces - so that two spellings of one policy are one stored value.
         _settings.set_CountryPolicy("smtp", "BLOCK: ru , cn");
         ClassicAssert.AreEqual("block:CN,RU", _settings.get_CountryPolicy("smtp"));

         _domain.CountryPolicy = "ALLOW: ie,gb ";
         _domain.Save();
         ClassicAssert.AreEqual("allow:GB,IE", _domain.CountryPolicy);

         // And every refusal, each with a sentence rather than a code.
         var refused = new[] { "deny:CN", "allow", "block:", "block:GBR", "block:G1", "off:GB" };

         foreach (string value in refused)
         {
            string copy = value;

            var settingsError = Assert.Throws<COMException>(() => _settings.set_CountryPolicy("smtp", copy),
               "The settings must refuse " + copy);
            ClassicAssert.IsNotEmpty(settingsError.Message);

            var domainError = Assert.Throws<COMException>(() => _domain.CountryPolicy = copy,
               "A domain must refuse " + copy);
            ClassicAssert.AreEqual(settingsError.Message, domainError.Message,
               "One grammar means one refusal: the settings and a domain must refuse " + copy + " in the same words.");
         }

         // Nothing was stored by any of them.
         ClassicAssert.AreEqual("block:CN,RU", _settings.get_CountryPolicy("smtp"));

         // A protocol this server has no policy for is refused rather than
         // silently written somewhere.
         Assert.Throws<COMException>(() => _settings.set_CountryPolicy("managesieve", "block:CN"));
      }

      [Test]
      [Description("The staleness setting is checked, and a database older than it is used anyway with a line in the log saying so")]
      public void AStaleDatabaseIsUsedAndSaidToBeStale()
      {
         // Built two hundred days ago, against a limit of forty-five.
         long built = DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 200L * 24 * 60 * 60;

         var networks = new Dictionary<string, string> { { "127.0.0.0/8", LoopbackCountry } };
         GeoIpFixture.WriteCountryDatabase(_databasePath, networks, built);

         _settings.GeoIpDatabaseMaxAgeDays = 45;
         _settings.GeoIpDatabaseFile = _databasePath;

         // Still used: a stale answer beats no answer, because most of the
         // addresses in it have not moved.
         ClassicAssert.AreEqual(LoopbackCountry, _application.Utilities.LookupCountry("127.0.0.1"));

         string status = _application.Utilities.GeoIpDatabaseStatus();
         StringAssert.Contains("\"stale\":true", status);

         StringAssert.Contains("which is more than the 45 day(s) GeoIpDatabaseMaxAgeDays allows",
            LogHandler.ReadCurrentDefaultLog());

         _settings.GeoIpDatabaseMaxAgeDays = 45;
      }
   }

   /// <summary>
   ///    The same settings over the REST API, which is the surface the Control
   ///    Deck and hmctl reach them through. The listener's own policy is in this
   ///    group too, which is why every value here is written and read back rather
   ///    than only written: a policy that refused this listener would take the
   ///    Deck away from the administrator who set it.
   /// </summary>
   [TestFixture]
   public class CountryBlockingOverRest : TestFixtureBase
   {
      private static int RestPort = 9591;
      private const string AdminPassword = "testar";

      private string _exemptRangesAsShipped;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         _exemptRangesAsShipped = _settings.CountryBlockingExemptRanges;

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            _settings.set_CountryPolicy("smtp", "");
            _settings.set_CountryPolicy("imap", "");
            _settings.set_CountryPolicy("pop3", "");
            _settings.set_CountryPolicy("http", "");
            _settings.CountryBlockingExemptRanges = _exemptRangesAsShipped;
            _settings.GeoIpDatabaseFile = "";
            _settings.GeoIpDatabaseMaxAgeDays = 45;
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The settings group carries the database, the exempt ranges and the four listener policies, and a PUT of them is read back through COM as the Control Panel reads it")]
      public void TheSettingsGroupRoundTripsThroughCom()
      {
         (int status, string body) = Http("GET", "/api/v1/settings");
         ClassicAssert.AreEqual(200, status, body);

         StringAssert.Contains("\"geoip_database_file\":", body);
         StringAssert.Contains("\"geoip_database_max_age_days\":", body);
         StringAssert.Contains("\"country_blocking_exempt_ranges\":", body);
         StringAssert.Contains("\"country_policy_smtp\":", body);
         StringAssert.Contains("\"country_policy_imap\":", body);
         StringAssert.Contains("\"country_policy_pop3\":", body);
         StringAssert.Contains("\"country_policy_http\":", body);

         // The status is a fact about the server, shown and never sent.
         StringAssert.Contains("\"geoip_database_status\":", body);

         (int putStatus, string putBody) = Http("PUT", "/api/v1/settings",
            "{\"country_policy_smtp\":\"BLOCK: cn , ru\",\"country_policy_pop3\":\"allow:gb\",\"geoip_database_max_age_days\":60}");
         ClassicAssert.AreEqual(200, putStatus, putBody);

         ClassicAssert.AreEqual("block:CN,RU", _settings.get_CountryPolicy("smtp"),
            "A policy written over REST must be stored in the same canonical form COM stores.");
         ClassicAssert.AreEqual("allow:GB", _settings.get_CountryPolicy("pop3"));
         ClassicAssert.AreEqual(60, _settings.GeoIpDatabaseMaxAgeDays);
      }

      [Test]
      [Description("A policy the grammar does not accept is a 400 carrying the setting's own sentence, and nothing in the request is applied")]
      public void ARefusedPolicyChangesNothing()
      {
         _settings.set_CountryPolicy("smtp", "block:CN");

         (int status, string body) = Http("PUT", "/api/v1/settings",
            "{\"country_policy_imap\":\"allow:GB\",\"country_policy_smtp\":\"deny:CN\"}");

         ClassicAssert.AreEqual(400, status, body);
         StringAssert.Contains("not a country policy", body);

         ClassicAssert.AreEqual("block:CN", _settings.get_CountryPolicy("smtp"), "The refused value must not have been stored.");
         ClassicAssert.AreEqual("", _settings.get_CountryPolicy("imap"),
            "A settings PUT is all or nothing: the key that was acceptable must not have been applied either.");
      }

      [Test]
      [Description("The read-only status key is refused by a PUT rather than stored")]
      public void TheStatusIsReadOnly()
      {
         (int status, string body) = Http("PUT", "/api/v1/settings", "{\"geoip_database_status\":\"{}\"}");

         ClassicAssert.AreEqual(400, status, body);
         StringAssert.Contains("geoip_database_status", body);
      }

      [Test]
      [Description("A domain's country policy travels over the domain route, canonicalised and read back in the listing")]
      public void ADomainPolicyTravelsOverTheDomainRoute()
      {
         string path = "/api/v1/domains/" + Uri.EscapeDataString(_domain.Name);

         (int status, string body) = Http("PUT", path, "{\"active\":true,\"country_policy\":\"BLOCK: ru,cn \"}");
         ClassicAssert.AreEqual(200, status, body);
         StringAssert.Contains("\"country_policy\":\"block:CN,RU\"", body);

         (int getStatus, string getBody) = Http("GET", "/api/v1/domains");
         ClassicAssert.AreEqual(200, getStatus, getBody);
         StringAssert.Contains("\"country_policy\":\"block:CN,RU\"", getBody);

         (int refusedStatus, string refusedBody) = Http("PUT", path, "{\"active\":true,\"country_policy\":\"deny:CN\"}");
         ClassicAssert.AreEqual(400, refusedStatus, refusedBody);
         StringAssert.Contains("not a country policy", refusedBody);

         (int clearStatus, string clearBody) = Http("PUT", path, "{\"active\":true,\"country_policy\":\"\"}");
         ClassicAssert.AreEqual(200, clearStatus, clearBody);
         StringAssert.Contains("\"country_policy\":\"\"", clearBody);
      }

      [Test]
      [Description("The countries report section exists, is server-wide, and says in its own words which half of it is windowed")]
      public void TheCountriesReportSectionIsServed()
      {
         (int status, string body) = Http("GET", "/api/v1/reports/countries");
         ClassicAssert.AreEqual(200, status, body);

         StringAssert.Contains("\"section\":\"countries\"", body);
         StringAssert.Contains("\"by_country\":", body);
         StringAssert.Contains("\"refusals_since_start\":", body);
         StringAssert.Contains("is NOT windowed", body);

         (int indexStatus, string indexBody) = Http("GET", "/api/v1/reports");
         ClassicAssert.AreEqual(200, indexStatus, indexBody);
         StringAssert.Contains("countries", indexBody);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
