// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    Accounts from a spreadsheet, held at the server: hmctl accounts import and
   ///    Import-HmAccount send each row of a CSV as a POST /api/v1/domains/{domain}/accounts,
   ///    and hmctl accounts export and Export-HmAccount write the columns
   ///    GET /api/v1/domains/{domain}/accounts answers. build/check-cli.py holds the two
   ///    clients to the exact bodies; this holds the server to storing them and to
   ///    answering them back.
   ///
   ///    Before 15 September 2026 neither half held: the create searched its body for six
   ///    keys and dropped the rest (a name column, admin_level) with a 201, its string
   ///    reader answered empty for a password the clients had sent as a JSON number, and
   ///    the listing answered address and active only, so every export had empty name,
   ///    size and level columns. Each test here fails against that server: the stored
   ///    names and levels, the listed fields, the refusals by name and the 403.
   ///
   ///    Every stored value is read back through COM, as the Control Panel reads it, and
   ///    a password is proved by the server accepting it. The accounts live in
   ///    example.test, which PerformBasicSetup recreates before every test; the API keys
   ///    made here are revoked, and the password policy one test sets is removed, in
   ///    TearDown.
   /// </summary>
   [TestFixture]
   public class RestApiAccountImport : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9137;
      private const string AdminPassword = "testar";
      private const string AccountsPath = "/api/v1/domains/example.test/accounts";

      // Names outside ASCII, written as escapes so that this file stays ASCII.
      private const string Una = "\u00DAna";
      private const string ONeill = "\u00D3 N\u00E9ill, Jr";
      private const string Zoe = "Zo\u00EB";

      // The password policy's minimum length, set by the one test that needs a
      // policy and returned to its default in TearDown.
      private const string PolicyMinimumLength = "PasswordPolicyMinimumLength";

      private readonly List<string> _keyIds = new List<string>();
      private bool _policySet;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            foreach (string id in _keyIds)
               Http("DELETE", "/api/v1/apikeys/" + id);
            _keyIds.Clear();
         }
         finally
         {
            if (_policySet)
            {
               IniFileSetting.Delete(PolicyMinimumLength);
               _policySet = false;
            }

            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("Three rows as the import sends them - a password of digits alone, a password that reads as a boolean, names outside ASCII with a comma, every typed column - are stored as sent, the passwords are the ones the server accepts, and the listing answers every column the export writes.")]
      public void RowsAsTheImportSendsThemAreStoredAndListedWithEveryExportedField()
      {
         // The bodies build/check-cli.py holds hmctl and Import-HmAccount to.
         AssertCreated(AccountsPath, "{\"address\":\"csv-carol@example.test\",\"password\":\"20260915\",\"first_name\":\"Carol\",\"last_name\":\"Clark\",\"active\":true,\"max_size_mb\":50,\"admin_level\":\"domain\"}");
         AssertCreated(AccountsPath, "{\"address\":\"csv-dave@example.test\",\"password\":\"true\",\"active\":false}");
         AssertCreated(AccountsPath, "{\"address\":\"csv-una@example.test\",\"password\":\"pass word\",\"first_name\":\"" + Una + "\",\"last_name\":\"" + ONeill + "\",\"active\":true,\"max_size_mb\":0,\"admin_level\":\"user\"}");

         Account carol = AccountOverCom("csv-carol@example.test");
         Assert.IsNotNull(carol, "The first row was not stored.");
         Assert.AreEqual("Carol", carol.PersonFirstName);
         Assert.AreEqual("Clark", carol.PersonLastName);
         Assert.AreEqual(50, carol.MaxSize);
         Assert.IsTrue(carol.Active);
         Assert.AreEqual(eAdminLevel.hAdminLevelDomainAdmin, carol.AdminLevel, "admin_level was dropped: the create must store it.");
         Assert.IsTrue(carol.ValidatePassword("20260915"), "A password of digits alone is the password.");
         Assert.IsTrue(ImapClientSimulator.ValidatePassword("csv-carol@example.test", "20260915"), "IMAP accepts the password of digits.");

         Account dave = AccountOverCom("csv-dave@example.test");
         Assert.IsNotNull(dave, "The second row was not stored.");
         Assert.IsFalse(dave.Active);
         Assert.AreEqual("", dave.PersonFirstName);
         Assert.AreEqual(eAdminLevel.hAdminLevelNormal, dave.AdminLevel, "admin_level defaults to user.");
         Assert.AreEqual(0, dave.MaxSize);
         Assert.IsTrue(dave.ValidatePassword("true"), "A password that reads as a boolean is the password.");

         Account una = AccountOverCom("csv-una@example.test");
         Assert.IsNotNull(una, "The third row was not stored.");
         Assert.AreEqual(Una, una.PersonFirstName, "The name is the UTF-8 the body carried.");
         Assert.AreEqual(ONeill, una.PersonLastName);
         Assert.IsTrue(una.ValidatePassword("pass word"));

         // The listing: address and active first, as it has always begun, then every
         // column the export writes, each of its type.
         (int status, string body) listed = Http("GET", AccountsPath);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("{\"address\":\"csv-carol@example.test\",\"active\":true,\"max_size_mb\":50,\"first_name\":\"Carol\",\"last_name\":\"Clark\",\"admin_level\":\"domain\"}", listed.body);
         StringAssert.Contains("{\"address\":\"csv-dave@example.test\",\"active\":false,\"max_size_mb\":0,\"first_name\":\"\",\"last_name\":\"\",\"admin_level\":\"user\"}", listed.body);
         StringAssert.Contains("{\"address\":\"csv-una@example.test\",\"active\":true,\"max_size_mb\":0,\"first_name\":\"" + Una + "\",\"last_name\":\"" + ONeill + "\",\"admin_level\":\"user\"}", listed.body);
      }

      [Test]
      [Description("What an import must not lose is refused by name and creates nothing: a name column, a password sent as a number, active or max_size_mb or admin_level of the wrong type or value, a name past the 60 characters its column holds, and a body that is not JSON. The negative controls of the test above - against the server before this change each of these made an account or said the wrong thing.")]
      public void WhatTheImportMustNotLoseIsRefusedByNameAndCreatesNothing()
      {
         AssertRefused(400, "unknown field: name", "csv-named@example.test",
            "{\"address\":\"csv-named@example.test\",\"password\":\"Named-Pass-1\",\"name\":\"Ivy Iverson\"}");
         AssertRefused(400, "password must be a string", "csv-number@example.test",
            "{\"address\":\"csv-number@example.test\",\"password\":20260915}");
         AssertRefused(400, "address and password are required", "csv-nopassword@example.test",
            "{\"address\":\"csv-nopassword@example.test\",\"first_name\":\"Frank\"}");
         AssertRefused(400, "active must be true or false", "csv-active@example.test",
            "{\"address\":\"csv-active@example.test\",\"password\":\"Active-Pass-1\",\"active\":\"yes\"}");
         AssertRefused(400, "max_size_mb must be a whole number", "csv-size@example.test",
            "{\"address\":\"csv-size@example.test\",\"password\":\"Size-Pass-1\",\"max_size_mb\":\"50\"}");
         AssertRefused(400, "max_size_mb must be a whole number", "csv-fraction@example.test",
            "{\"address\":\"csv-fraction@example.test\",\"password\":\"Size-Pass-1\",\"max_size_mb\":5.5}");
         AssertRefused(400, "max_size_mb must be a whole number", "csv-negative@example.test",
            "{\"address\":\"csv-negative@example.test\",\"password\":\"Size-Pass-1\",\"max_size_mb\":-1}");
         AssertRefused(400, "admin_level must be user, domain or server", "csv-root@example.test",
            "{\"address\":\"csv-root@example.test\",\"password\":\"Level-Pass-1\",\"admin_level\":\"root\"}");
         AssertRefused(400, "admin_level must be user, domain or server", "csv-two@example.test",
            "{\"address\":\"csv-two@example.test\",\"password\":\"Level-Pass-1\",\"admin_level\":2}");
         AssertRefused(400, "at most 60 characters", "csv-long@example.test",
            "{\"address\":\"csv-long@example.test\",\"password\":\"Long-Pass-1\",\"last_name\":\"" + new string('x', 61) + "\"}");
         AssertRefused(400, "the body must be a JSON object", "csv-form@example.test",
            "address=csv-form@example.test&password=Form-Pass-1");

         // The boundary the refusal above sits on: sixty characters are stored whole.
         AssertCreated(AccountsPath, "{\"address\":\"csv-sixty@example.test\",\"password\":\"Sixty-Pass-1\",\"last_name\":\"" + new string('y', 60) + "\"}");
         Assert.AreEqual(new string('y', 60), AccountOverCom("csv-sixty@example.test").PersonLastName);
      }

      [Test]
      [Description("admin_level on the create follows the update's rule, which is the COM rule: a key restricted to named domains may import users and domain administrators and is refused a server administrator with a 403 that creates nothing; a key issued for every domain, or the administrator password, may import one.")]
      public void AKeyForNamedDomainsMayImportDomainAdministratorsButNotServerAdministrators()
      {
         (string ownId, string ownKey) = CreateKey("csv import - own domain", "full", "example.test");
         (string fullId, string fullKey) = CreateKey("csv import - every domain", "full", null);

         (int status, string body) refused = Bearer("POST", AccountsPath, ownKey,
            "{\"address\":\"csv-mint@example.test\",\"password\":\"Mint-Pass-1\",\"admin_level\":\"server\"}");
         Assert.AreEqual(403, refused.status, "A domain-restricted key must not import a server administrator. Body: " + refused.body);
         StringAssert.Contains("restricted to named domains", refused.body);
         Assert.IsNull(AccountOverCom("csv-mint@example.test"), "The refused row must not have made an account at any level.");

         AssertCreated(AccountsPath, "{\"address\":\"csv-deputy@example.test\",\"password\":\"Deputy-Pass-1\",\"admin_level\":\"domain\"}", ownKey);
         Assert.AreEqual(eAdminLevel.hAdminLevelDomainAdmin, AccountOverCom("csv-deputy@example.test").AdminLevel);

         AssertCreated(AccountsPath, "{\"address\":\"csv-member@example.test\",\"password\":\"Member-Pass-1\",\"admin_level\":\"user\"}", ownKey);
         Assert.AreEqual(eAdminLevel.hAdminLevelNormal, AccountOverCom("csv-member@example.test").AdminLevel);

         AssertCreated(AccountsPath, "{\"address\":\"csv-chief@example.test\",\"password\":\"Chief-Pass-1\",\"admin_level\":\"server\"}", fullKey);
         Assert.AreEqual(eAdminLevel.hAdminLevelServerAdmin, AccountOverCom("csv-chief@example.test").AdminLevel);

         AssertCreated(AccountsPath, "{\"address\":\"csv-boss@example.test\",\"password\":\"Boss-Pass-1\",\"admin_level\":\"server\"}");
         Assert.AreEqual(eAdminLevel.hAdminLevelServerAdmin, AccountOverCom("csv-boss@example.test").AdminLevel);
      }

      [Test]
      [Description("The password policy applies to the create as it does to Account.Password over COM and to the update: with a minimum length set, a row whose password is shorter is refused with the policy's own sentence and makes no account - not even the server administrator the row asked for - and a row whose password meets it is made. Against the create before this change the short password made the account.")]
      public void APasswordThePolicyRefusesMakesNoAccount()
      {
         // The negative control first, with no policy: the same short password is
         // taken, so the refusal below is the policy's and nothing else's.
         AssertCreated(AccountsPath, "{\"address\":\"csv-before@example.test\",\"password\":\"20260915\"}");

         IniFileSetting.Write(PolicyMinimumLength, "12");
         _policySet = true;
         _application.Reinitialize();

         AssertRefused(400, "at least 12 characters", "csv-short@example.test",
            "{\"address\":\"csv-short@example.test\",\"password\":\"20260915\",\"admin_level\":\"server\"}");

         // A duplicate is still a duplicate, whatever its password: an import run
         // a second time hears 409 for the rows it made the first time.
         (int status, string body) again = Http("POST", AccountsPath, "{\"address\":\"csv-before@example.test\",\"password\":\"20260915\"}");
         Assert.AreEqual(409, again.status, "An existing address is reported as existing before its password is judged. Answer: " + again.body);

         AssertCreated(AccountsPath, "{\"address\":\"csv-policy@example.test\",\"password\":\"Long-Enough-Pass-1\"}");
         Assert.IsTrue(AccountOverCom("csv-policy@example.test").ValidatePassword("Long-Enough-Pass-1"));
      }

      [Test]
      [Description("The round trip at the server: an entry of the listing, with a new address and a password added, is a create body the route takes whole - no field of it unknown - and the account it makes lists as the same entry.")]
      public void AListedEntryWithAPasswordAddedMakesTheSameAccount()
      {
         AssertCreated(AccountsPath, "{\"address\":\"csv-source@example.test\",\"password\":\"Source-Pass-1\",\"first_name\":\"" + Zoe + "\",\"last_name\":\"O'Neil, Jr\",\"active\":false,\"max_size_mb\":75,\"admin_level\":\"domain\"}");

         string entry = ListedEntry("csv-source@example.test");
         string copy = entry.Replace("\"address\":\"csv-source@example.test\"", "\"address\":\"csv-copy@example.test\",\"password\":\"Copy-Pass-1\"");
         AssertCreated(AccountsPath, copy);

         Assert.AreEqual(entry.Replace("csv-source@example.test", "csv-copy@example.test"), ListedEntry("csv-copy@example.test"),
            "The copy must list exactly as the entry it was made from.");

         Account copied = AccountOverCom("csv-copy@example.test");
         Assert.AreEqual(Zoe, copied.PersonFirstName);
         Assert.AreEqual("O'Neil, Jr", copied.PersonLastName);
         Assert.IsFalse(copied.Active);
         Assert.AreEqual(75, copied.MaxSize);
         Assert.AreEqual(eAdminLevel.hAdminLevelDomainAdmin, copied.AdminLevel);
         Assert.IsTrue(copied.ValidatePassword("Copy-Pass-1"));
      }

      [Test]
      [Description("hmctl itself, run against this server: a UTF-8 spreadsheet with a password of digits, a name outside ASCII and a comma in quotes is imported, and the export writes the same columns back. Ignored where there is no Python on the PATH.")]
      public void HmctlImportsAndExportsASpreadsheetAgainstThisServer()
      {
         string python = FindPython();
         if (python == null)
            Assert.Ignore("No python.exe on the PATH (the Microsoft Store's stand-in does not count), so hmctl cannot run here; build/check-cli.py holds it to the same bodies against a recorded server.");

         string hmctl = RepositoryFile(@"hmailserver\source\Tools\Cli\hmctl");
         string directory = ScratchDirectory();
         try
         {
            string people = Path.Combine(directory, "people.csv");
            File.WriteAllText(people, Spreadsheet("csv-cli-py@example.test"), new UTF8Encoding(false));
            string connection = Quote(hmctl) + " --url http://127.0.0.1:" + RestPort + " --password " + AdminPassword;

            (int exitCode, string output) imported = Run(python, connection + " accounts import " + Quote(people));
            Assert.AreEqual(0, imported.exitCode, imported.output);
            StringAssert.Contains("1 made, 0 already there, 0 refused.", imported.output);
            AssertSpreadsheetAccount("csv-cli-py@example.test");

            string exported = Path.Combine(directory, "accounts.csv");
            (int exitCode, string output) export = Run(python, connection + " accounts export example.test " + Quote(exported));
            Assert.AreEqual(0, export.exitCode, export.output);

            string text = File.ReadAllText(exported, Encoding.UTF8);
            StringAssert.StartsWith("address,first_name,last_name,active,max_size_mb,admin_level\r\n", text);
            StringAssert.Contains("csv-cli-py@example.test," + Zoe + ",\"O'Neil, Jr\",true,250,domain\r\n", text);
         }
         finally
         {
            Directory.Delete(directory, true);
         }
      }

      [Test]
      [Description("The PowerShell module itself, under Windows PowerShell 5.1, run against this server: the same spreadsheet imported by Import-HmAccount and written back by Export-HmAccount, the name outside ASCII surviving both directions.")]
      public void TheModuleImportsAndExportsASpreadsheetAgainstThisServer()
      {
         string powershell = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
         if (!File.Exists(powershell))
            Assert.Ignore("No Windows PowerShell at " + powershell + ".");

         string module = RepositoryFile(@"hmailserver\source\Tools\Cli\HmailServer.psm1");
         string directory = ScratchDirectory();
         try
         {
            string people = Path.Combine(directory, "people.csv");
            string exported = Path.Combine(directory, "accounts.csv");
            File.WriteAllText(people, Spreadsheet("csv-cli-ps@example.test"), new UTF8Encoding(false));

            string script = string.Join("\n",
               "$ErrorActionPreference = 'Stop'",
               "Import-Module " + PowerShellQuote(module) + " -Force",
               "Connect-HmServer -Url 'http://127.0.0.1:" + RestPort + "' -Password (ConvertTo-SecureString " + PowerShellQuote(AdminPassword) + " -AsPlainText -Force) | Out-Null",
               "$result = Import-HmAccount -Path " + PowerShellQuote(people) + " -Confirm:$false",
               "if (@($result.Failed).Count -gt 0) { throw ('refused: ' + (@($result.Failed) | ConvertTo-Json -Compress)) }",
               "'made: ' + (@($result.Made) -join ',')",
               "Export-HmAccount -Domain example.test -Path " + PowerShellQuote(exported));

            (int exitCode, string output) run = Run(powershell,
               "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + Convert.ToBase64String(Encoding.Unicode.GetBytes(script)));
            Assert.AreEqual(0, run.exitCode, run.output);
            StringAssert.Contains("made: csv-cli-ps@example.test", run.output);
            AssertSpreadsheetAccount("csv-cli-ps@example.test");

            // Export-Csv quotes every field, and Windows PowerShell writes a byte-order mark.
            string text = File.ReadAllText(exported, Encoding.UTF8);
            StringAssert.StartsWith("\"address\",\"first_name\",\"last_name\",\"active\",\"max_size_mb\",\"admin_level\"", text);
            StringAssert.Contains("\"csv-cli-ps@example.test\",\"" + Zoe + "\",\"O'Neil, Jr\",\"true\",\"250\",\"domain\"", text);
         }
         finally
         {
            Directory.Delete(directory, true);
         }
      }

      // One row as a spreadsheet writes it: a password of digits, a name outside
      // ASCII, a comma in quotes, yes for active and a level in capitals.
      private static string Spreadsheet(string address)
      {
         return "address,password,first_name,last_name,active,max_size_mb,admin_level\r\n" +
                address + ",20260915," + Zoe + ",\"O'Neil, Jr\",yes,250,Domain\r\n";
      }

      private void AssertSpreadsheetAccount(string address)
      {
         Account account = AccountOverCom(address);
         Assert.IsNotNull(account, address + " was not made.");
         Assert.AreEqual(Zoe, account.PersonFirstName);
         Assert.AreEqual("O'Neil, Jr", account.PersonLastName);
         Assert.IsTrue(account.Active);
         Assert.AreEqual(250, account.MaxSize);
         Assert.AreEqual(eAdminLevel.hAdminLevelDomainAdmin, account.AdminLevel);
         Assert.IsTrue(account.ValidatePassword("20260915"), "The password of digits is the password.");
      }

      private void AssertCreated(string path, string body, string bearerToken = null)
      {
         (int status, string body) created = bearerToken == null ? Http("POST", path, body) : Bearer("POST", path, bearerToken, body);
         Assert.AreEqual(201, created.status, "POST " + path + " " + body + " must create the account. Answer: " + created.body);
      }

      private void AssertRefused(int expectedStatus, string reason, string address, string body)
      {
         (int status, string body) answer = Http("POST", AccountsPath, body);
         Assert.AreEqual(expectedStatus, answer.status, body + " must be refused. Answer: " + answer.body);
         StringAssert.Contains(reason, answer.body, "The refusal of " + body + " must say why.");
         Assert.IsNull(AccountOverCom(address), "The refused " + body + " must not have made an account.");
      }

      // The listing's entry for one address, whole.
      private static string ListedEntry(string address)
      {
         (int status, string body) listed = Http("GET", AccountsPath);
         Assert.AreEqual(200, listed.status, listed.body);
         Match match = Regex.Match(listed.body, "\\{\"address\":\"" + Regex.Escape(address) + "\"[^{}]*\\}");
         Assert.IsTrue(match.Success, "No entry for " + address + " in: " + listed.body);
         return match.Value;
      }

      private Account AccountOverCom(string address)
      {
         try
         {
            return _domain.Accounts.ItemByAddress[address];
         }
         catch (COMException)
         {
            return null;
         }
      }

      private (string id, string key) CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";
         (int status, string created) = Http("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + created);
         string id = Extract(created, "id");
         _keyIds.Add(id);
         return (id, Extract(created, "key"));
      }

      // The value of a top-level JSON property, string or number, enough for
      // the bodies this API returns.
      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"?([^\",}]*)\"?");
         Assert.IsTrue(match.Success, "No '" + key + "' in: " + json);
         return match.Groups[1].Value;
      }

      // python.exe from the PATH, skipping the Microsoft Store's stand-in under
      // WindowsApps, which opens the Store rather than running anything.
      private static string FindPython()
      {
         foreach (string entry in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
         {
            string directory = entry.Trim().Trim('"');
            if (directory.Length == 0 || directory.IndexOf("WindowsApps", StringComparison.OrdinalIgnoreCase) >= 0)
               continue;

            try
            {
               string candidate = Path.Combine(directory, "python.exe");
               if (File.Exists(candidate))
                  return candidate;
            }
            catch (ArgumentException)
            {
               // A PATH entry with a character a path cannot hold.
            }
         }

         return null;
      }

      // A file of the checkout, found by walking up from the test binaries, so the
      // test does not depend on how deep the output directory is.
      private static string RepositoryFile(string relativePath)
      {
         for (DirectoryInfo directory = new DirectoryInfo(TestContext.CurrentContext.TestDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Combine(directory.FullName, relativePath);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("These binaries are not inside a checkout, so " + relativePath + " cannot be found.");
         return null;
      }

      private static string ScratchDirectory()
      {
         string directory = Path.Combine(Path.GetTempPath(), "hm-account-import-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(directory);
         return directory;
      }

      private static string Quote(string path)
      {
         return "\"" + path + "\"";
      }

      private static string PowerShellQuote(string value)
      {
         return "'" + value.Replace("'", "''") + "'";
      }

      // Runs a client to completion and answers its exit code and everything it
      // printed. The credential environment variables are removed so that only the
      // arguments decide who is asking, and the loopback is kept off any proxy.
      private static (int exitCode, string output) Run(string fileName, string arguments)
      {
         var start = new ProcessStartInfo(fileName, arguments)
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };
         foreach (string name in new[] { "HMAIL_URL", "HMAIL_API_KEY", "HMAIL_PASSWORD", "HMAIL_PASSWORD_FILE" })
            start.EnvironmentVariables.Remove(name);
         start.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
         start.EnvironmentVariables["NO_PROXY"] = "127.0.0.1,localhost";

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();

            if (!process.WaitForExit(120 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
                  // It finished between the wait and the kill.
               }

               Assert.Fail(fileName + " " + arguments + " did not finish within two minutes. Printed: " + output.Result + error.Result);
            }

            process.WaitForExit();
            return (process.ExitCode, output.Result + error.Result);
         }
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return Http(method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) Bearer(string method, string path, string token, string requestBody = null)
      {
         return Http(method, path, "Bearer " + token, requestBody);
      }

      // Issues one HTTP/1.0 request against the REST listener and returns the
      // parsed status code and body. The connect is retried briefly to absorb the
      // listener bind race right after a reinitialize.
      private static (int status, string body) Http(string method, string path, string authorization, string requestBody)
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
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int statusCode = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               return (statusCode, body);
            }
         }
      }
   }
}
