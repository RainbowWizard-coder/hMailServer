// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Security
{
   /// <summary>
   ///    Which account a name given at sign-in names (Common/Util/LogonName.h): the one
   ///    whose stored address IS the name, in other case, under the default domain or a
   ///    domain alias - and no other spelling the database's own comparison takes for it.
   ///    Up to 6.3.7 the database chose: MySQL and MariaDB took an accented letter for a
   ///    plain one, and they, SQL Server and SQL Server Compact took the address with
   ///    spaces after it, and SQL Server Compact a soft hyphen inside it, while the
   ///    per-name lockout and the compromise hold counted such a spelling apart. A
   ///    spelling with the right password now fails as a name that does not exist
   ///    fails, with the same reply, and is counted as a failure; and a locked name is
   ///    not got round by one. The names go over IMAP LOGIN as bytes in the server's
   ///    narrow code page (Windows-1252 on Windows, Latin-1 on Linux; the same bytes
   ///    for every character here), as a client typing them sends them. On PostgreSQL,
   ///    which compares lower() of both sides, none of these spellings found the
   ///    account before either, so there those tests hold of the old build too. The
   ///    web services listener's HTTP Basic (CalDAV, CardDAV, Autodiscover) carries a
   ///    name as UTF-8, so a dotted capital I for an i - which PostgreSQL's lower()
   ///    took for the i, as MySQL and MariaDB did - and a NUL reach the sign-in there:
   ///    those tests hold the rule on every engine, and that a name holding a NUL is
   ///    sent to no database statement. SCRAM-SHA-256 finds its account by a lookup
   ///    of its own on each of SMTP, IMAP and POP3, and the address with a space after
   ///    it signed in there too. IMAP LOGIN on Linux keeps a NUL in a name, as HTTP
   ///    Basic does, and that is held too. The REST API's request for a password reset,
   ///    which asks for no credential, can be given an address holding a NUL; its
   ///    address is lowered before anything else is done with it, and the lowering
   ///    ends at the NUL, so it asks for the part before the NUL: that is held, and
   ///    that it sends no more than a request for that part would. The held account's
   ///    case is in API/RestApiCompromisedAccount, over IMAP LOGIN and over SMTP AUTH
   ///    PLAIN, which carries a name as UTF-8.
   /// </summary>
   [TestFixture]
   public class LogonNameVariants : TestFixtureBase
   {
      private const string Password = "Variant-Passw0rd!";
      private const int WebServicesPort = 9183;
      private const string AdminPassword = "testar";

      // The JSON escape of U+0000, made of its two halves so that no tool that
      // reads this file decodes it.
      private const string JsonNulEscape = "\\" + "u0000";

      private static int RestPort = 9185;

      private Account _account;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "variant@" + _domain.Name, Password);
      }

      /// <summary>
      ///    The spellings of address that the databases took for it: an accented letter
      ///    (MySQL, MariaDB), a soft hyphen inside it (SQL Server Compact, and SQL Server
      ///    under its _100 collations) and a space after it (MySQL, MariaDB, SQL Server,
      ///    SQL Server Compact). address must hold an "a" after its first character.
      /// </summary>
      public static List<string> Variants(string address)
      {
         int a = address.IndexOf('a', 1);
         return new List<string>
         {
            address.Substring(0, a) + "\u00e0" + address.Substring(a + 1),
            address.Substring(0, a) + "\u00ad" + address.Substring(a),
            address + " "
         };
      }

      [Test]
      [Description("Each spelling the databases took for the account fails with its right password exactly as a name that does not exist fails; its own spelling and other case sign in")]
      public void AVariantSpellingFailsAsAWrongNameDoes()
      {
         string wrongName = ImapLogin("nobody-at-all@" + _domain.Name, Password);
         StringAssert.StartsWith("NO", wrongName);

         foreach (string variant in Variants(_account.Address))
         {
            ClassicAssert.AreEqual(wrongName, ImapLogin(variant, Password),
               "The spelling '" + Escape(variant) + "' with the account's right password must fail as a name that does not exist fails.");
         }

         StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password), "The account's own address signs in.");
         StringAssert.StartsWith("OK", ImapLogin(_account.Address.ToUpperInvariant(), Password), "The address in other case signs in.");
      }

      [Test]
      [Description("The address under a domain alias and the local part alone under the default domain sign in, in other case too; an accented spelling under either fails as a name that does not exist fails")]
      public void AnAliasAndTheDefaultDomainStillSignIn()
      {
         string wrongName = ImapLogin("nobody-at-all@" + _domain.Name, Password);
         StringAssert.StartsWith("NO", wrongName);

         // DomainAliases.Add gives the alias the collection's domain, so the
         // domain's ID, which the Linux suite's stand-in for the COM API does not
         // have, is not read.
         DomainAlias alias = _domain.DomainAliases.Add();
         alias.AliasName = "logon-alias.example";
         alias.Save();

         string defaultDomain = _settings.DefaultDomain;
         try
         {
            StringAssert.StartsWith("OK", ImapLogin("variant@logon-alias.example", Password), "The address under a domain alias signs in.");
            StringAssert.StartsWith("OK", ImapLogin("VARIANT@Logon-Alias.Example", Password), "The address under a domain alias in other case signs in.");
            ClassicAssert.AreEqual(wrongName, ImapLogin("v\u00e0riant@logon-alias.example", Password),
               "An accented spelling under a domain alias fails as a name that does not exist fails.");

            _settings.DefaultDomain = _domain.Name;
            StringAssert.StartsWith("OK", ImapLogin("variant", Password), "The local part alone signs in under the default domain.");
            // A bare name that does not exist, for the reply a bare name gets.
            string wrongBareName = ImapLogin("nobody-at-all", Password);
            StringAssert.StartsWith("NO", wrongBareName);
            ClassicAssert.AreEqual(wrongBareName, ImapLogin("v\u00e0riant", Password),
               "An accented local part under the default domain fails as a name that does not exist fails.");
         }
         finally
         {
            _settings.DefaultDomain = defaultDomain;
            _domain.DomainAliases.DeleteByDBID(alias.ID);
         }
      }

      [Test]
      [Description("A spelling the sign-in refuses is counted as a failure: three of the address followed by a space, with the right password, lock the account's name")]
      public void AVariantSpellingIsCountedAsAFailure()
      {
         try
         {
            SetIniSetting("AccountLockoutThreshold", "3");
            _application.Reinitialize();

            for (int i = 0; i < 3; i++)
               StringAssert.StartsWith("NO", ImapLogin(_account.Address + " ", Password), "The address followed by a space does not sign in.");

            StringAssert.StartsWith("NO", ImapLogin(_account.Address, Password),
               "Three failures under a spelling the lockout counts as the account lock the account's name.");
         }
         finally
         {
            SetIniSetting("AccountLockoutThreshold", null);
            _application.Reinitialize();
            StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password));
         }
      }

      [Test]
      [Description("A locked name is not got round by another spelling of it: once three wrong passwords lock it, no spelling signs in with the right one")]
      public void ALockedNameIsNotGotRoundByAVariantSpelling()
      {
         try
         {
            SetIniSetting("AccountLockoutThreshold", "3");
            _application.Reinitialize();

            for (int i = 0; i < 3; i++)
               StringAssert.StartsWith("NO", ImapLogin(_account.Address, "wrong" + i));

            string locked = ImapLogin(_account.Address, Password);
            StringAssert.StartsWith("NO", locked, "The locked name refuses the right password.");

            foreach (string variant in Variants(_account.Address))
            {
               ClassicAssert.AreEqual(locked, ImapLogin(variant, Password),
                  "The spelling '" + Escape(variant) + "' must not sign in to a locked name with its right password.");
            }
         }
         finally
         {
            SetIniSetting("AccountLockoutThreshold", null);
            _application.Reinitialize();
            StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password));
         }
      }

      /// <summary>
      ///    The spellings of address that the databases took for it over HTTP Basic, which
      ///    carries a name as UTF-8: a dotted capital I for its first i (PostgreSQL, MySQL,
      ///    MariaDB), an i grave for it (MySQL, MariaDB) and a space after it (MySQL,
      ///    MariaDB, SQL Server, SQL Server Compact). address must hold an "i".
      /// </summary>
      public static List<string> Utf8Variants(string address)
      {
         int i = address.IndexOf('i');
         return new List<string>
         {
            address.Substring(0, i) + "İ" + address.Substring(i + 1),
            address.Substring(0, i) + "ì" + address.Substring(i + 1),
            address + " "
         };
      }

      [Test]
      [Description("Over the web services listener's HTTP Basic, a locked name is not got round by a dotted capital I or an i grave for its i, nor by a space after it, with the right password")]
      public void OverHttpBasicALockedNameIsNotGotRoundByAVariantSpelling()
      {
         StartWebServices("3");
         try
         {
            for (int i = 0; i < 3; i++)
               StringAssert.StartsWith("NO", ImapLogin(_account.Address, "wrong" + i));

            int locked = DavOptions(_account.Address, Password);
            ClassicAssert.AreEqual(401, locked, "The locked name refuses the right password over HTTP Basic.");

            foreach (string variant in Utf8Variants(_account.Address))
            {
               ClassicAssert.AreEqual(locked, DavOptions(variant, Password),
                  "The spelling '" + Escape(variant) + "' must not sign in to a locked name with its right password over HTTP Basic.");
            }
         }
         finally
         {
            StopWebServices();
            StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password));
         }
      }

      [Test]
      [Description("Over HTTP Basic, each spelling the databases took for the account fails with its right password as a name that does not exist fails, and is counted as a failure: three of the address followed by a space lock the account's name")]
      public void OverHttpBasicAVariantSpellingFailsAndIsCountedAsAFailure()
      {
         StartWebServices("3");
         try
         {
            ClassicAssert.AreEqual(200, DavOptions(_account.Address, Password), "The account's own name signs in over HTTP Basic.");

            int wrongName = DavOptions("nobody-at-all@" + _domain.Name, Password);
            ClassicAssert.AreEqual(401, wrongName);

            foreach (string variant in Utf8Variants(_account.Address))
            {
               ClassicAssert.AreEqual(wrongName, DavOptions(variant, Password),
                  "The spelling '" + Escape(variant) + "' with the account's right password must fail over HTTP Basic as a name that does not exist fails.");
            }

            // The space's spelling has failed once above; two more make three.
            for (int i = 0; i < 2; i++)
               ClassicAssert.AreEqual(wrongName, DavOptions(_account.Address + " ", Password));

            StringAssert.StartsWith("NO", ImapLogin(_account.Address, Password),
               "Three failures under a spelling the lockout counts as the account lock the account's name.");
         }
         finally
         {
            StopWebServices();
            StringAssert.StartsWith("OK", ImapLogin(_account.Address, Password));
         }
      }

      [Test]
      [Description("A name holding a NUL names no account: over HTTP Basic, with the account's right password, it fails as a name that does not exist fails, and no database statement is sent with it, so no error is reported")]
      public void ANameHoldingANulNamesNoAccountAndIsSentToNoStatement()
      {
         StartWebServices(null);
         try
         {
            ClassicAssert.AreEqual(200, DavOptions(_account.Address, Password), "The account's own name signs in over HTTP Basic.");

            int wrongName = DavOptions("nobody-at-all@" + _domain.Name, Password);
            ClassicAssert.AreEqual(401, wrongName);

            string address = _account.Address;
            var names = new List<string>
            {
               address + "\0",
               address + "\0junk",
               address.Substring(0, 3) + "\0" + address.Substring(3)
            };

            foreach (string name in names)
            {
               ClassicAssert.AreEqual(wrongName, DavOptions(name, Password),
                  "The name '" + Escape(name) + "' with the account's right password must fail as a name that does not exist fails.");
            }

            string errors = File.Exists(LogHandler.GetErrorLogFileName()) ? LogHandler.ReadAndDeleteErrorLog() : "";
            ClassicAssert.IsEmpty(errors.Trim(), "A name holding a NUL reached a database statement, and an error was reported.");
         }
         finally
         {
            StopWebServices();
         }
      }

      [Test]
      [Description("IMAP LOGIN with a name holding a NUL - at its end, before more text and inside it - and the account's right password signs nothing in, and no database statement is sent with it, so no error is reported. On Linux the command line reaches LOGIN whole, and the name fails as a name that does not exist fails; on Windows the line ends at the NUL, so the command is never completed and nobody is signed in")]
      public void OverImapLoginANameHoldingANulNamesNoAccountAndIsSentToNoStatement()
      {
         string wrongName = ImapLogin("nobody-at-all@" + _domain.Name, Password);
         StringAssert.StartsWith("NO", wrongName);

         // The suite reads the server's error log by its path, so it runs where the
         // server runs, and the platform of this process is the server's.
         bool lineKeepsTheNul = Environment.OSVersion.Platform == PlatformID.Unix;

         string address = _account.Address;
         var names = new List<string>
         {
            address + "\0",
            address + "\0junk",
            address.Substring(0, 3) + "\0" + address.Substring(3)
         };

         foreach (string name in names)
         {
            string reply = ImapLoginOrNoAnswer(name, Password, lineKeepsTheNul ? 60000 : 5000);
            if (lineKeepsTheNul)
            {
               ClassicAssert.AreEqual(wrongName, reply,
                  "Over IMAP LOGIN, the name '" + Escape(name) + "' with the account's right password must fail as a name that does not exist fails.");
            }
            else
            {
               ClassicAssert.IsFalse(reply != null && reply.StartsWith("OK", StringComparison.Ordinal),
                  "Over IMAP LOGIN, the name '" + Escape(name) + "' with the account's right password signed in: " + reply);
            }
         }

         string errors = File.Exists(LogHandler.GetErrorLogFileName()) ? LogHandler.ReadAndDeleteErrorLog() : "";
         ClassicAssert.IsEmpty(errors.Trim(), "A name holding a NUL reached a database statement over IMAP LOGIN, and an error was reported.");

         StringAssert.StartsWith("OK", ImapLogin(address, Password), "The account's own address signs in.");
      }

      [Test]
      [Description("POST /api/v1/password-reset, which asks for no credential, with an address holding a NUL - at its end, before more text and inside it - in a body the JSON parser refuses, which the route's lenient reader then reads, and then with the address itself: each is answered as every request is, no database statement is sent with a NUL, so no error is reported, and the four together send what the address's own request alone would - one link to its recovery address, under the address's own budget")]
      public void APasswordResetForAnAddressHoldingANulSendsNoMoreThanOneForTheAddress()
      {
         Account recovery = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "variant-recovery@" + _domain.Name, Password);

         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         try
         {
            ClassicAssert.AreEqual(200, Rest("GET", "/api/v1/status", Basic("Administrator", AdminPassword), null).status,
               "REST API did not answer /api/v1/status.");
            (int status, string body) configured = Rest("PUT", "/api/v1/settings", Basic("Administrator", AdminPassword),
               "{\"confidential_base_url\":\"https://mail.example.test\",\"account_recovery_enabled\":true}");
            ClassicAssert.AreEqual(200, configured.status, configured.body);

            // A proved recovery address, which is what a reset link is sent to.
            string address = _account.Address;
            (int status, string body) set = Rest("PUT", "/api/v1/me/recovery", Basic(address, Password),
               "{\"address\":\"" + recovery.Address + "\",\"password\":\"" + Password + "\"}");
            ClassicAssert.AreEqual(200, set.status, set.body);
            string confirmation = Pop3ClientSimulator.AssertGetFirstMessageText(recovery.Address, Password);
            ClassicAssert.AreEqual(200, Rest("POST", "/recovery/verify?token=" + TokenIn(confirmation, "/recovery/verify?token="), null, null).status,
               "The recovery address was not confirmed.");

            var names = new List<string>
            {
               address + JsonNulEscape,
               address + JsonNulEscape + "junk",
               address.Substring(0, 3) + JsonNulEscape + address.Substring(3)
            };

            string answer = null;
            foreach (string name in names)
            {
               // Text after the end of the value: the parser refuses the body.
               (int status, string body) asked = Rest("POST", "/api/v1/password-reset", null, "{\"address\":\"" + name + "\"} x");
               ClassicAssert.AreEqual(202, asked.status, asked.body);
               if (answer == null)
                  answer = asked.body;
               ClassicAssert.AreEqual(answer, asked.body, "Every request is answered alike.");
            }

            // The address itself, within the minute: one link in all, whichever of the
            // requests sent it, since a request for a name holding a NUL is at most a
            // request for the part before it, under that part's own budget.
            (int status, string body) own = Rest("POST", "/api/v1/password-reset", null, "{\"address\":\"" + address + "\"}");
            ClassicAssert.AreEqual(202, own.status, own.body);
            ClassicAssert.AreEqual(answer, own.body, "A request for the address itself is answered as the others were.");

            // Waited for as long as the queued work would take, then counted once the
            // delivery queue is empty, so that "one arrived" is not "the second has not
            // arrived yet".
            System.Threading.Thread.Sleep(3000);
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(recovery.Address, Password, 1);
            StringAssert.Contains("/portal/reset?token=", Pop3ClientSimulator.AssertGetFirstMessageText(recovery.Address, Password));

            string errors = File.Exists(LogHandler.GetErrorLogFileName()) ? LogHandler.ReadAndDeleteErrorLog() : "";
            ClassicAssert.IsEmpty(errors.Trim(), "An address holding a NUL reached a database statement from a password-reset request, and an error was reported.");
         }
         finally
         {
            Rest("PUT", "/api/v1/settings", Basic("Administrator", AdminPassword), "{\"confidential_base_url\":\"\",\"account_recovery_enabled\":true}");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      private static (int status, string body) Rest(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      /// <summary>The 64 hex digits after the marker, out of the message as it was sent.</summary>
      private static string TokenIn(string text, string marker)
      {
         int at = text.IndexOf(marker, StringComparison.Ordinal);
         ClassicAssert.Greater(at, -1, "No " + marker + " in: " + text);
         System.Text.RegularExpressions.Match token = System.Text.RegularExpressions.Regex.Match(text.Substring(at + marker.Length), "^[0-9a-f]{64}");
         ClassicAssert.IsTrue(token.Success, "No token after " + marker + " in: " + text);
         return token.Value;
      }

      [Test]
      [Description("Over SCRAM-SHA-256 on SMTP, IMAP and POP3, the address with a space after it, which MySQL, MariaDB and both SQL Servers took for the account, fails with the account's right password as a name that does not exist fails; the address itself signs in on each")]
      public void OverScramAVariantSpellingFailsAsAWrongNameDoes()
      {
         bool saslEnabled = _settings.IMAPSASLPlainEnabled;
         _settings.IMAPSASLPlainEnabled = true;
         try
         {
            string[] wrongName = Scram("nobody-at-all@" + _domain.Name, Password);
            StringAssert.StartsWith("535", wrongName[0], "A name that does not exist does not sign in over SMTP.");
            StringAssert.DoesNotContain("A01 OK", wrongName[1], "A name that does not exist does not sign in over IMAP.");
            StringAssert.StartsWith("-ERR", wrongName[2], "A name that does not exist does not sign in over POP3.");

            string[] variant = Scram(_account.Address + " ", Password);
            for (int i = 0; i < ScramProtocols.Length; i++)
            {
               ClassicAssert.AreEqual(wrongName[i], variant[i],
                  "Over " + ScramProtocols[i] + " SCRAM-SHA-256, the address with a space after it and the account's right password must fail as a name that does not exist fails.");
            }

            string[] own = Scram(_account.Address, Password);
            StringAssert.StartsWith("235", own[0], "The account's own address signs in over SMTP SCRAM-SHA-256.");
            StringAssert.Contains("A01 OK", own[1], "The account's own address signs in over IMAP SCRAM-SHA-256.");
            StringAssert.StartsWith("+OK", own[2], "The account's own address signs in over POP3 SCRAM-SHA-256.");
         }
         finally
         {
            _settings.IMAPSASLPlainEnabled = saslEnabled;
         }
      }

      private static readonly string[] ScramProtocols = { "SMTP", "IMAP", "POP3" };

      /// <summary>
      ///    One SCRAM-SHA-256 exchange on each of SMTP, IMAP and POP3, each on a
      ///    connection of its own; the final reply of each, in that order.
      /// </summary>
      private static string[] Scram(string user, string password)
      {
         var replies = new string[3];

         using (var smtp = new TcpConnection())
         {
            ClassicAssert.IsTrue(smtp.Connect(25), "Could not connect to SMTP.");
            smtp.ReadUntil("220");
            smtp.Send("EHLO example.test\r\n");
            smtp.ReadUntil("250 ");
            replies[0] = RegressionTests.SMTP.SmtpScramTestClient.Authenticate(smtp, user, password);
         }

         using (var imap = new TcpConnection())
         {
            ClassicAssert.IsTrue(imap.Connect(143), "Could not connect to IMAP.");
            imap.Receive();
            replies[1] = RegressionTests.IMAP.ScramTestClient.Authenticate(imap, "A01", user, password);
         }

         using (var pop3 = new TcpConnection())
         {
            ClassicAssert.IsTrue(pop3.Connect(110), "Could not connect to POP3.");
            pop3.ReadUntil("+OK");
            replies[2] = RegressionTests.POP3.Pop3SaslTestClient.AuthenticateScram(pop3, user, password);

            // Released, so that the maildrop is not held for the next sign-in.
            pop3.Send("QUIT\r\n");
            pop3.Receive();
         }

         return replies;
      }

      /// <summary>
      ///    SMTP AUTH PLAIN with an initial response, the name and the password as
      ///    UTF-8, on a connection of its own; the reply line.
      /// </summary>
      public static string SmtpAuthPlain(string user, string password)
      {
         using (var con = new TcpConnection())
         {
            ClassicAssert.IsTrue(con.Connect(25), "Could not connect to SMTP.");
            con.ReadUntil("220");
            con.Send("EHLO example.test\r\n");
            con.ReadUntil("250 ");
            con.Send("AUTH PLAIN " + Convert.ToBase64String(Encoding.UTF8.GetBytes("\0" + user + "\0" + password)) + "\r\n");
            return con.Receive();
         }
      }

      /// <summary>
      ///    On Linux, where SASLprep cannot normalise a name that is not ASCII, SMTP
      ///    AUTH PLAIN reports HM6450 for such a name to the error log and goes on with
      ///    the name as it was given. Those lines, and only those, are read and removed
      ///    here, so that the next test's SetUp does not take them for an error of its
      ///    own; any other error fails the test.
      /// </summary>
      public static void ConsumeSaslPrepRefusals()
      {
         if (!File.Exists(LogHandler.GetErrorLogFileName()))
            return;

         string reported = LogHandler.ReadAndDeleteErrorLog();
         foreach (string line in reported.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries))
         {
            if (line.Trim().Length > 0)
               StringAssert.Contains("HM6450", line, "An error other than SASLprep's refusal of a name that is not ASCII was reported: " + line);
         }
      }

      private void StartWebServices(string lockoutThreshold)
      {
         IniFileSetting.Write("CalDavRedirectUrl", "");
         IniFileSetting.Write("CardDavRedirectUrl", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebServicesPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", "0");
         SetIniSetting("AccountLockoutThreshold", lockoutThreshold);
         _application.Reinitialize();
      }

      private void StopWebServices()
      {
         IniFileSetting.Write("WebServicesHttpPort", "0");
         SetIniSetting("AccountLockoutThreshold", null);
         _application.Reinitialize();
      }

      /// <summary>
      ///    OPTIONS /dav/ on the web services listener with HTTP Basic, the name and the
      ///    password as UTF-8, on a connection of its own; the status code. The request
      ///    says it came over HTTPS, as a TLS-terminating proxy on this machine says it
      ///    (X-Forwarded-Proto), since DAV refuses Basic over plain HTTP, and the
      ///    listener is plain HTTP here.
      /// </summary>
      private static int DavOptions(string user, string password)
      {
         string authorization = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", WebServicesPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  System.Threading.Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            client.ReceiveTimeout = 60000;
            client.SendTimeout = 60000;
            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               string head = "OPTIONS /dav/ HTTP/1.0\r\nHost: 127.0.0.1:" + WebServicesPort + "\r\nAuthorization: " + authorization +
                             "\r\nX-Forwarded-Proto: https\r\nConnection: close\r\n\r\n";
               byte[] bytes = Encoding.ASCII.GetBytes(head);
               stream.Write(bytes, 0, bytes.Length);

               var buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int space = raw.IndexOf(' ');
               int status;
               if (!raw.StartsWith("HTTP/", StringComparison.Ordinal) || space < 0 || raw.Length < space + 4 ||
                   !int.TryParse(raw.Substring(space + 1, 3), out status))
                  throw new InvalidOperationException("The web services listener's answer was not an HTTP response: " + raw);
               return status;
            }
         }
      }

      /// <summary>
      ///    IMAP LOGIN with the name and the password as quoted strings, each character
      ///    sent as the one byte of its code (so below U+0100), on a connection of its
      ///    own; the tagged reply without its tag.
      /// </summary>
      public static string ImapLogin(string user, string password)
      {
         string reply = ImapLoginOrNoAnswer(user, password, 60000);
         if (reply == null)
            throw new InvalidOperationException("The connection closed before the LOGIN was answered.");
         return reply;
      }

      /// <summary>
      ///    ImapLogin, waiting at most timeout milliseconds for the tagged reply; null
      ///    when none came in that time or the connection closed first.
      /// </summary>
      private static string ImapLoginOrNoAnswer(string user, string password, int timeout)
      {
         using (var client = new TcpClient(TestPorts.HostAddress.AddressFamily))
         {
            client.ReceiveTimeout = 60000;
            client.SendTimeout = 60000;
            client.Connect(TestPorts.HostAddress, TestPorts.Actual(143));
            NetworkStream stream = client.GetStream();
            ReadLine(stream);

            Write(stream, "A1 LOGIN \"" + user + "\" \"" + password + "\"\r\n");
            client.ReceiveTimeout = timeout;

            string line;
            do
            {
               try
               {
                  line = ReadLine(stream);
               }
               catch (IOException)
               {
                  return null;
               }

               if (line == null)
                  return null;
            } while (!line.StartsWith("A1 ", StringComparison.Ordinal));

            try
            {
               Write(stream, "A2 LOGOUT\r\n");
            }
            catch (Exception)
            {
               // The server may have closed the connection after the reply.
            }

            return line.Substring(3);
         }
      }

      private static void Write(NetworkStream stream, string text)
      {
         var bytes = new byte[text.Length];
         for (int i = 0; i < text.Length; i++)
         {
            if (text[i] > 0xFF)
               throw new ArgumentException("A character past U+00FF has no one-byte form.");
            bytes[i] = (byte) text[i];
         }

         stream.Write(bytes, 0, bytes.Length);
      }

      private static string ReadLine(NetworkStream stream)
      {
         var chars = new System.Text.StringBuilder();
         int b;
         while ((b = stream.ReadByte()) >= 0)
         {
            if (b == '\n')
               return chars.ToString();
            if (b != '\r')
               chars.Append((char) b);
         }

         return chars.Length > 0 ? chars.ToString() : null;
      }

      private static string Escape(string text)
      {
         var escaped = new System.Text.StringBuilder();
         foreach (char c in text)
         {
            if (c >= 0x20 && c < 0x7F)
               escaped.Append(c);
            else
               escaped.AppendFormat("<U+{0:X4}>", (int) c);
         }

         return escaped.ToString();
      }

      // Stores the setting, or returns it to its default when value is null, as
      // AccountLockoutTests does: the settings store in the database, applied by
      // the Reinitialize after each call.
      private static void SetIniSetting(string key, string value)
      {
         if (value == null)
            IniFileSetting.Delete(key);
         else
            IniFileSetting.Write(key, value);
      }
   }
}
