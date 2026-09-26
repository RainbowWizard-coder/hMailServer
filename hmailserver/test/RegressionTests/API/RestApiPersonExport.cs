// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Sockets;
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
   ///    A person's data as one archive, and every trace of them erased: the two
   ///    halves of a data-subject request. Account.ExportArchive and
   ///    GET /api/v1/accounts/{address}/export.zip write and download the same
   ///    .zip - the mail as mbox per folder, the address book as vCard, the
   ///    preferences, an account.xml in the backup's form, the log lines that
   ///    name the address and a manifest that counts them. The erasure reaches
   ///    what account deletion leaves in OTHER people's records - their address
   ///    books and first-contact memory - and, asked deliberately, the log files.
   ///    Windows only: the archive is read back with System.IO.Compression and
   ///    the log with LogHandler, neither of which the Linux bench has.
   /// </summary>
   [TestFixture]
   public class RestApiPersonExport : TestFixtureBase
   {
      private static int RestPort = 9597;
      private const string AdminPassword = "testar";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
      }

      private static void AppendTo(ImapClientSimulator imap, string tag, string folder, string message)
      {
         imap.SendSingleCommandWithLiteral(tag + " APPEND \"" + folder + "\" {" + message.Length + "}", message);
      }

      private Account APersonWithMailAndAContact(string address)
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, "test");

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "First", "The first exported body.");
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Second", "The second exported body.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(account.Address, "test"));
         ClassicAssert.IsTrue(imap.CreateFolder("Projects"));
         AppendTo(imap, "A10", "Projects", "Subject: Filed\r\n\r\nA body filed under Projects for export.\r\n");
         imap.Disconnect();

         // A contact in the person's own address book, over their own API.
         (int status, string body) created = Http("POST", "/api/v1/me/contacts", "{\"name\":\"Friend Example\",\"address\":\"friend@example.test\"}", UserHeader(account.Address));
         ClassicAssert.AreEqual(201, created.status, created.body);

         return account;
      }

      [Test]
      [Description("Account.ExportArchive writes one .zip holding the mail as mbox per folder, the address book as vCard, the preferences, an account.xml in the backup's form, the log lines naming the address and a manifest that counts them")]
      public void TheArchiveHoldsEverythingTheServerHolds()
      {
         Account account = APersonWithMailAndAContact("exported@example.test");
         string zipPath = Paths.Combine(Path.GetTempPath(), "hm-person-" + Guid.NewGuid().ToString("N") + ".zip");
         try
         {
            int exported = account.ExportArchive(zipPath);
            ClassicAssert.AreEqual(3, exported, "Two delivered messages and one filed by IMAP.");

            using (ZipArchive zip = ZipFile.OpenRead(zipPath))
            {
               string inbox = Read(zip, "mail/INBOX.mbox");
               StringAssert.Contains("The first exported body.", inbox);
               StringAssert.Contains("The second exported body.", inbox);
               StringAssert.StartsWith("From ", inbox, "An mbox starts with a From_ line.");
               StringAssert.Contains("A body filed under Projects for export.", Read(zip, "mail/Projects.mbox"));

               string contacts = Read(zip, "contacts.vcf");
               StringAssert.Contains("BEGIN:VCARD", contacts);
               StringAssert.Contains("friend@example.test", contacts);

               string accountXml = Read(zip, "account.xml");
               StringAssert.Contains("exported@example.test", accountXml);
               StringAssert.Contains("<Account", accountXml);

               ClassicAssert.IsNotNull(zip.GetEntry("preferences.json"), "The webmail's preferences are there, empty or not.");
               ClassicAssert.IsNotNull(zip.GetEntry("logs.txt"), "The log lines naming the address are there, even when there are none.");

               string manifest = Read(zip, "manifest.json");
               StringAssert.Contains("\"address\": \"exported@example.test\"", manifest);
               StringAssert.Contains("\"messages\": 3", manifest);
               StringAssert.Contains("\"folders\": 2", manifest);
               StringAssert.Contains("\"contacts\": 1", manifest);
            }
         }
         finally
         {
            if (File.Exists(zipPath))
               File.Delete(zipPath);
         }
      }

      [Test]
      [Description("GET /api/v1/accounts/{address}/export.zip downloads the same archive with the administrator password, and the person downloads their own at GET /api/v1/me/export.zip")]
      public void TheArchiveIsDownloadedByTheOperatorAndByThePerson()
      {
         Account account = APersonWithMailAndAContact("downloaded@example.test");

         byte[] operators = Download("/api/v1/accounts/" + Uri.EscapeDataString(account.Address) + "/export.zip", AdminHeader());
         ClassicAssert.Greater(operators.Length, 100, "An archive came back.");
         ClassicAssert.AreEqual('P', (char) operators[0]);
         ClassicAssert.AreEqual('K', (char) operators[1]);

         using (var zip = new ZipArchive(new MemoryStream(operators), ZipArchiveMode.Read))
         {
            StringAssert.Contains("The first exported body.", Read(zip, "mail/INBOX.mbox"));
            StringAssert.Contains("friend@example.test", Read(zip, "contacts.vcf"));
         }

         byte[] own = Download("/api/v1/me/export.zip", UserHeader(account.Address));
         ClassicAssert.Greater(own.Length, 100, "The person gets an archive too.");
         using (var zip = new ZipArchive(new MemoryStream(own), ZipArchiveMode.Read))
         {
            StringAssert.Contains("\"address\": \"downloaded@example.test\"", Read(zip, "manifest.json"));
         }

         // The operator's route is the administrator password's alone.
         (int created, string key) = Http("POST", "/api/v1/apikeys", "{\"label\":\"person export\",\"scope\":\"full\"}");
         ClassicAssert.AreEqual(201, created, key);
         // 401, as on every route that is the password's alone: to a key, "valid
         // but not permitted" is kept indistinguishable from "not a key".
         ClassicAssert.AreEqual(401, Http("GET", "/api/v1/accounts/" + Uri.EscapeDataString(account.Address) + "/export.zip", null, "Bearer " + Extract(key, "key")).status);
      }

      [Test]
      [Description("An app password cannot download the account's export (403), and the archive's account.xml leaves out the password hash, the authenticator's seed, the app passwords' hashes and the fetch accounts' passwords")]
      public void AnAppPasswordCannotDownloadTheExportAndTheArchiveCarriesNoCredentials()
      {
         Account account = APersonWithMailAndAContact("credentials@example.test");

         FetchAccount fetch = account.FetchAccounts.Add();
         fetch.Enabled = false;
         fetch.Name = "Elsewhere";
         fetch.Username = "someone@dummy-example.com";
         fetch.Password = "fetch-account-secret";
         fetch.ServerAddress = "pop.dummy-example.com";
         fetch.Port = 110;
         fetch.Save();

         (int status, string body) minted = Http("POST", "/api/v1/me/app-passwords", "{\"name\":\"Phone\",\"password\":\"test\"}", UserHeader(account.Address));
         ClassicAssert.AreEqual(201, minted.status, minted.body);
         string appPassword = Extract(minted.body, "password");
         string asProgram = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes(account.Address + ":" + appPassword));

         // The app password works - it reads the account's own details - and is
         // refused the whole account in one download.
         ClassicAssert.AreEqual(200, Http("GET", "/api/v1/me", null, asProgram).status, "The app password signs in.");
         (int status, string body) refused = Http("GET", "/api/v1/me/export.zip", null, asProgram);
         ClassicAssert.AreEqual(403, refused.status, refused.body);

         byte[] own = Download("/api/v1/me/export.zip", UserHeader(account.Address));
         string accountXml;
         using (var zip = new ZipArchive(new MemoryStream(own), ZipArchiveMode.Read))
            accountXml = Read(zip, "account.xml");

         // What the backup's form carries is there - the fetch account and the
         // app password by name - and the secrets in them are not.
         StringAssert.Contains("<FetchAccount", accountXml);
         StringAssert.Contains("Elsewhere", accountXml);
         StringAssert.Contains("<AppPassword", accountXml);
         StringAssert.Contains("Phone", accountXml);
         ClassicAssert.IsFalse(Regex.IsMatch(accountXml, "\\sPassword=\""), "No password, the account's or a fetch account's: " + accountXml);
         ClassicAssert.IsFalse(accountXml.Contains("PasswordEncryption="), "No password scheme.");
         ClassicAssert.IsFalse(accountXml.Contains("TotpSecret="), "No authenticator seed.");
         ClassicAssert.IsFalse(accountXml.Contains("OAuthToken="), "No fetch account token.");
         ClassicAssert.IsFalse(Regex.IsMatch(accountXml, "\\sHash=\""), "No app password hash.");

         // The operator's download is the same archive, and leaves them out too.
         byte[] operators = Download("/api/v1/accounts/" + Uri.EscapeDataString(account.Address) + "/export.zip", AdminHeader());
         using (var zip = new ZipArchive(new MemoryStream(operators), ZipArchiveMode.Read))
            ClassicAssert.IsFalse(Regex.IsMatch(Read(zip, "account.xml"), "\\s(Password|TotpSecret|Hash)=\""));
      }

      [Test]
      [Description("A mailbox whose mail alone is past 2 GB is refused a download (413) before any archive is built, by the person and by the operator alike")]
      public void AMailboxPastTwoGigabytesIsRefusedBeforeTheArchiveIsBuilt()
      {
         Account account = APersonWithMailAndAContact("large@example.test");

         // Three small messages on disk, recorded in the database as a gigabyte
         // each: what the refusal is decided on is the recorded size, before a
         // byte of mail is read. The old code built the whole archive - here a
         // small one - and answered 200.
         _application.Database.ExecuteSQL("update hm_messages set messagesize = 1073741824 where messageaccountid = " + account.ID);

         (int status, string body) own = Http("GET", "/api/v1/me/export.zip", null, UserHeader(account.Address));
         ClassicAssert.AreEqual(413, own.status, own.body);
         StringAssert.Contains("2 GB", own.body);

         (int status, string body) operators = Http("GET", "/api/v1/accounts/" + Uri.EscapeDataString(account.Address) + "/export.zip");
         ClassicAssert.AreEqual(413, operators.status, operators.body);

         // The refusal is the recorded size and nothing else: back under it, the
         // same mailbox downloads.
         _application.Database.ExecuteSQL("update hm_messages set messagesize = 100 where messageaccountid = " + account.ID);
         ClassicAssert.Greater(Download("/api/v1/me/export.zip", UserHeader(account.Address)).Length, 100, "Back under the ceiling, the download is built.");
      }

      [Test]
      [Description("logs.txt holds the lines that name the address as a whole address: ann@'s export does not carry the log lines of joann@ or of ann@ at a longer domain")]
      public void TheLogLinesAreThoseOfTheAddressAloneNotOfALongerOne()
      {
         Account ann = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ann@example.test", "test");
         Account joann = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "joann@example.test", "test");

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", ann.Address, "To Ann", "For ann.");
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", joann.Address, "To Joann", "For joann.");
         Pop3ClientSimulator.AssertMessageCount(ann.Address, "test", 1);
         Pop3ClientSimulator.AssertMessageCount(joann.Address, "test", 1);

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("joann@example.test", log, "The SMTP log names the other person, or this test proves nothing.");

         string zipPath = Paths.Combine(Path.GetTempPath(), "hm-person-" + Guid.NewGuid().ToString("N") + ".zip");
         try
         {
            ann.ExportArchive(zipPath);

            using (ZipArchive zip = ZipFile.OpenRead(zipPath))
            {
               string lines = Read(zip, "logs.txt");
               StringAssert.Contains("ann@example.test", lines, "Ann's own lines are there.");
               StringAssert.DoesNotContain("joann@example.test", lines, "Nobody else's lines are.");
            }
         }
         finally
         {
            if (File.Exists(zipPath))
               File.Delete(zipPath);
         }
      }

      [Test]
      [Description("An erasure reaches other people's address books and first-contact memory, and - asked for by name - the log files, where the address becomes [erased]")]
      public void ErasureReachesOtherPeoplesRecordsAndTheLogs()
      {
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "other@example.test", "test");
         Account erased = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "erased@example.test", "test");

         // The erased person is in the other's address book, by the other's
         // own hand, and in the other's first-contact memory, by writing.
         ClassicAssert.AreEqual(201, Http("POST", "/api/v1/me/contacts", "{\"name\":\"Erased Person\",\"address\":\"erased@example.test\"}", UserHeader(other.Address)).status);
         SmtpClientSimulator.StaticSend(erased.Address, other.Address, "Hello", "From the person about to be erased.");
         Pop3ClientSimulator.AssertMessageCount(other.Address, "test", 1);
         // First-contact memory is kept only where the domain has the tip on, and
         // outgoing it is of the people an account wrote to signed in: the other
         // writes back, and the erased address is remembered.
         _domain.FirstContactTip = true;
         _domain.Save();
         new SmtpClientSimulator().SendRaw(other.Address, "test", other.Address, erased.Address,
            "From: " + other.Address + "\r\nTo: " + erased.Address + "\r\nSubject: Re: Hello\r\n\r\nA reply.\r\n");
         Pop3ClientSimulator.AssertMessageCount(erased.Address, "test", 1);
         StringAssert.Contains("erased@example.test", Http("GET", "/api/v1/me/contacts", null, UserHeader(other.Address)).body);
         StringAssert.Contains("erased@example.test", LogHandler.ReadCurrentDefaultLog(), "The SMTP log names the sender.");

         erased.Delete();

         int removed = _application.Utilities.EraseAddressTraces("erased@example.test", true);
         ClassicAssert.GreaterOrEqual(removed, 2, "The contact row and the first-contact row at least.");
         StringAssert.DoesNotContain("erased@example.test", Http("GET", "/api/v1/me/contacts", null, UserHeader(other.Address)).body,
            "The other person's address book no longer names the erased address.");
         StringAssert.Contains("erased@example.test", LogHandler.ReadCurrentDefaultLog(), "EraseAddressTraces alone leaves the logs as they are.");

         int lines = _application.Utilities.EraseAddressLogLines("erased@example.test");
         ClassicAssert.Greater(lines, 0, "Occurrences were rewritten.");
         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.DoesNotContain("erased@example.test", log, "No log line names the address any more.");
         StringAssert.Contains("[erased]", log, "The lines say they were changed rather than pretending nothing was there.");

         ClassicAssert.AreEqual(0, _application.Utilities.EraseAddressLogLines("erased@example.test"), "A second pass finds nothing.");
      }

      [Test]
      [Description("Erasing an address from the logs rewrites it only where it is the whole address: joann@ and a longer domain keep their lines as they were")]
      public void ErasingTheLogsLeavesALongerAddressAlone()
      {
         Account ann = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ann@example.test", "test");
         Account joann = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "joann@example.test", "test");

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", ann.Address, "To Ann", "For ann.");
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", joann.Address, "To Joann", "For joann.");
         Pop3ClientSimulator.AssertMessageCount(ann.Address, "test", 1);
         Pop3ClientSimulator.AssertMessageCount(joann.Address, "test", 1);
         StringAssert.Contains("<joann@example.test>", LogHandler.ReadCurrentDefaultLog(), "The SMTP log names the other person, or this test proves nothing.");

         int changed = _application.Utilities.EraseAddressLogLines("ann@example.test");
         ClassicAssert.Greater(changed, 0, "Ann's occurrences were rewritten.");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("<joann@example.test>", log, "Joann's lines are hers and stay as they were.");
         StringAssert.DoesNotContain("jo[erased]", log, "No other address is cut into.");
         ClassicAssert.IsFalse(Regex.IsMatch(log, "(?<![A-Za-z0-9.+_-])ann@example\\.test"), "Ann's own occurrences are gone.");
         StringAssert.Contains("[erased]", log);
      }

      [Test]
      [Description("Erasing an address from the logs rewrites it where a log line quotes it, as the application log's 'address' lines do: a quotation mark before an address is not part of it")]
      public void ErasingTheLogsReachesTheAddressInQuotes()
      {
         // The application log writes "RestApi: Distribution list '<address>'
         // created ...". An apostrophe is a byte RFC 5322 lets a local part
         // hold, and a rule that read it as one left this occurrence behind.
         (int status, string body) created = Http("POST", "/api/v1/domains/example.test/lists", "{\"address\":\"quoted-list@example.test\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("'quoted-list@example.test'", LogHandler.ReadCurrentDefaultLog(), "The application log quotes the address, or this test proves nothing.");

         int changed = _application.Utilities.EraseAddressLogLines("quoted-list@example.test");
         ClassicAssert.Greater(changed, 0, "The quoted occurrence was rewritten.");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.DoesNotContain("quoted-list@example.test", log, "No log line names the address any more.");
         StringAssert.Contains("'[erased]'", log, "The quotation says it was changed.");
      }

      // ----------------------------------------------------------------- helpers

      private static string Read(ZipArchive zip, string name)
      {
         ZipArchiveEntry entry = zip.GetEntry(name);
         ClassicAssert.IsNotNull(entry, "The archive has no " + name + ". Entries: " + string.Join(", ", zip.Entries.Select(e => e.FullName)));
         using (var reader = new StreamReader(entry.Open(), Encoding.UTF8))
            return reader.ReadToEnd();
      }

      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\":\"([^\"]*)\"");
         ClassicAssert.IsTrue(match.Success, "No " + key + " in " + json);
         return match.Groups[1].Value;
      }

      private static string AdminHeader()
      {
         return "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
      }

      private static string UserHeader(string address)
      {
         return "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes(address + ":test"));
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null, string authorization = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization ?? AdminHeader(), requestBody);
      }

      // The archive is bytes, which RestRequest's string answer would mangle: a
      // raw HTTP/1.0 exchange that reads the body by its Content-Length.
      private static byte[] Download(string path, string authorization)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);
            using (NetworkStream stream = client.GetStream())
            {
               byte[] request = Encoding.ASCII.GetBytes(
                  "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\nAuthorization: " + authorization + "\r\nConnection: close\r\n\r\n");
               stream.Write(request, 0, request.Length);

               var all = new MemoryStream();
               byte[] buffer = new byte[65536];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  all.Write(buffer, 0, read);

               byte[] bytes = all.ToArray();
               int headerEnd = IndexOf(bytes, Encoding.ASCII.GetBytes("\r\n\r\n"));
               ClassicAssert.Greater(headerEnd, 0, "No header end in the answer.");
               string headers = Encoding.ASCII.GetString(bytes, 0, headerEnd);
               StringAssert.StartsWith("HTTP/1.", headers);
               ClassicAssert.IsTrue(headers.Contains(" 200 "), "Not 200: " + headers);
               StringAssert.Contains("application/zip", headers.ToLowerInvariant());

               byte[] body = new byte[bytes.Length - headerEnd - 4];
               Array.Copy(bytes, headerEnd + 4, body, 0, body.Length);
               return body;
            }
         }
      }

      private static int IndexOf(byte[] haystack, byte[] needle)
      {
         for (int i = 0; i + needle.Length <= haystack.Length; i++)
         {
            int j = 0;
            while (j < needle.Length && haystack[i + j] == needle[j])
               j++;
            if (j == needle.Length)
               return i;
         }
         return -1;
      }
   }
}
