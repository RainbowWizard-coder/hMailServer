// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.SSL;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The first-hour checklist: GET /api/v1/setup/checklist and
   ///    Utilities.GetSetupChecklist, the same document over REST and over COM. Eight
   ///    items - a domain, an account, TLS on the submission port, SPF, DKIM, DMARC, a
   ///    backup destination and a second factor on the administrator password - each
   ///    done, action_needed, cannot_tell or not_needed, with findings that carry a
   ///    code, its arguments and an English sentence.
   ///
   ///    What is tested is that the judgement is the server's and is made from what
   ///    the server can see: the records are looked up in the suite's own zone, the
   ///    submission port is judged from the port table with a certificate that
   ///    exists, and the backup destination is judged on the disk the server runs on.
   ///    A lookup that is skipped says so rather than failing the item.
   /// </summary>
   [TestFixture]
   public class RestApiSetupChecklist : TestFixtureBase
   {
      private static int RestPort = 9593;
      private const string AdminPassword = "testar";
      private const string Path = "/api/v1/setup/checklist";
      private const string CertificateName = "Checklist certificate";
      private const int TlsPort = 5593;

      private string _previousBackupDestination;
      private Domain _secondDomain;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         _previousBackupDestination = _settings.Backup.Destination;

         // The fixture's domain has an account, so the first two items are done
         // and the rest of the list is what is under test.
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "checklist@example.test", "test");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            SuiteDns.Reset();
            _settings.Backup.Destination = _previousBackupDestination;

            if (_secondDomain != null)
            {
               _secondDomain.Delete();
               _secondDomain = null;
            }

            RemoveWhatWasAdded();
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("Eight items in the order a new server meets them, each in one of the four states, each finding with a code, its arguments and a sentence; the fixture's domain and account are done.")]
      public void EightItemsEachWithAState()
      {
         JsonDocument document = Get(Path);
         JsonElement root = document.RootElement;

         ClassicAssert.IsTrue(root.GetProperty("dns_looked_up").GetBoolean(), "The lookups are made unless the caller says not to.");

         var keys = new List<string>();
         var states = new HashSet<string> { "done", "action_needed", "cannot_tell", "not_needed" };
         int counted = 0;
         foreach (JsonElement item in root.GetProperty("items").EnumerateArray())
         {
            keys.Add(item.GetProperty("key").GetString());
            string state = item.GetProperty("state").GetString();
            ClassicAssert.IsTrue(states.Contains(state), "Unknown state " + state + " on " + keys[keys.Count - 1]);

            int findings = 0;
            foreach (JsonElement finding in item.GetProperty("findings").EnumerateArray())
            {
               findings++;
               ClassicAssert.IsTrue(states.Contains(finding.GetProperty("state").GetString()));
               ClassicAssert.IsNotEmpty(finding.GetProperty("code").GetString(), "Every finding carries a code.");
               ClassicAssert.AreEqual(JsonValueKind.Array, finding.GetProperty("args").ValueKind);
               ClassicAssert.IsNotEmpty(finding.GetProperty("text").GetString(), "Every finding carries its sentence for a client with no catalogue.");
            }
            ClassicAssert.Greater(findings, 0, "An item without a finding cannot be judged: " + keys[keys.Count - 1]);
            counted++;
         }

         CollectionAssert.AreEqual(new[] { "domain", "account", "submission", "spf", "dkim", "dmarc", "backup", "second_factor" }, keys);

         int summed = root.GetProperty("done").GetInt32() + root.GetProperty("action_needed").GetInt32() +
                      root.GetProperty("cannot_tell").GetInt32() + root.GetProperty("not_needed").GetInt32();
         ClassicAssert.AreEqual(counted, summed, "The four counts are the items.");

         ClassicAssert.AreEqual("done", StateOf(root, "domain"));
         ClassicAssert.AreEqual("done", StateOf(root, "account"));
         ClassicAssert.AreEqual("domain_count", FirstCode(root, "domain"));
         StringAssert.Contains("Active domains:", FirstText(root, "domain"));
      }

      [Test]
      [Description("SPF and DMARC are looked up in DNS for every active domain: a domain whose records the suite's zone serves is done, one with no records needs action, and the two are told apart by name.")]
      public void RecordsAreLookedUpInDnsForEveryActiveDomain()
      {
         SuiteDns.Zone.WithTxt("example.test", "v=spf1 -all");
         SuiteDns.Zone.WithTxt("_dmarc.example.test", "v=DMARC1; p=none");

         JsonDocument published = Get(Path);
         ClassicAssert.AreEqual("done", StateOf(published.RootElement, "spf"), published.RootElement.ToString());
         ClassicAssert.AreEqual("done", StateOf(published.RootElement, "dmarc"));
         ClassicAssert.AreEqual("record_published", FirstCode(published.RootElement, "spf"));
         CollectionAssert.AreEqual(new[] { "example.test", "SPF", "example.test" }, FirstArgs(published.RootElement, "spf"));
         CollectionAssert.AreEqual(new[] { "example.test", "DMARC", "_dmarc.example.test" }, FirstArgs(published.RootElement, "dmarc"));

         // The fixture's domain does not sign, which is the first thing the DKIM
         // item says - before any record is looked up.
         ClassicAssert.AreEqual("action_needed", StateOf(published.RootElement, "dkim"));
         ClassicAssert.AreEqual("dkim_off", FirstCode(published.RootElement, "dkim"));

         _secondDomain = SingletonProvider<TestSetup>.Instance.AddDomain("checklist-second.test");

         JsonDocument mixed = Get(Path);
         ClassicAssert.AreEqual("action_needed", StateOf(mixed.RootElement, "spf"), "One domain without a record makes the item need action.");
         var codesByDomain = new Dictionary<string, string>();
         foreach (JsonElement finding in Item(mixed.RootElement, "spf").GetProperty("findings").EnumerateArray())
            codesByDomain[finding.GetProperty("args")[0].GetString()] = finding.GetProperty("code").GetString();
         ClassicAssert.AreEqual("record_published", codesByDomain["example.test"]);
         ClassicAssert.AreEqual("record_missing", codesByDomain["checklist-second.test"]);
      }

      [Test]
      [Description("With dns=0 the record items say which host to look up instead of looking it up, and the item cannot be told rather than failed; COM answers the same document.")]
      public void SkippedLookupsSaySoRatherThanFailing()
      {
         JsonDocument skipped = Get(Path + "?dns=0");
         ClassicAssert.IsFalse(skipped.RootElement.GetProperty("dns_looked_up").GetBoolean());
         ClassicAssert.AreEqual("cannot_tell", StateOf(skipped.RootElement, "spf"));
         ClassicAssert.AreEqual("record_not_looked_up", FirstCode(skipped.RootElement, "spf"));
         CollectionAssert.AreEqual(new[] { "example.test", "DMARC", "_dmarc.example.test" }, FirstArgs(skipped.RootElement, "dmarc"));

         string overCom = _application.Utilities.GetSetupChecklist(false);
         using (JsonDocument com = JsonDocument.Parse(overCom))
         {
            ClassicAssert.IsFalse(com.RootElement.GetProperty("dns_looked_up").GetBoolean());
            ClassicAssert.AreEqual("record_not_looked_up", FirstCode(com.RootElement, "spf"));
            ClassicAssert.AreEqual(8, com.RootElement.GetProperty("items").GetArrayLength());
         }

         using (JsonDocument com = JsonDocument.Parse(_application.Utilities.GetSetupChecklist(true)))
         {
            ClassicAssert.IsTrue(com.RootElement.GetProperty("dns_looked_up").GetBoolean());
            ClassicAssert.AreEqual("record_missing", FirstCode(com.RootElement, "spf"), "Nothing is published for the fixture's domain in this test.");
         }
      }

      [Test]
      [Description("The submission port is judged from the port table: the stock port 587 without TLS needs action, and a port with STARTTLS and a certificate that exists is done, named with its certificate.")]
      public void TheSubmissionPortNeedsTlsWithACertificate()
      {
         JsonDocument stock = Get(Path);
         ClassicAssert.AreEqual("action_needed", StateOf(stock.RootElement, "submission"));
         ClassicAssert.IsTrue(HasFinding(stock.RootElement, "submission", "submission_plain", "587"), Item(stock.RootElement, "submission").ToString());

         string sslPath = SslSetup.GetSslCertPath();
         SSLCertificate certificate = _settings.SSLCertificates.Add();
         certificate.Name = CertificateName;
         certificate.CertificateFile = Paths.Combine(sslPath, "example.crt");
         certificate.PrivateKeyFile = Paths.Combine(sslPath, "example.key");
         certificate.Save();

         TCPIPPort port = _settings.TCPIPPorts.Add();
         port.Address = "0.0.0.0";
         port.PortNumber = TlsPort;
         port.Protocol = eSessionType.eSTSMTP;
         port.ConnectionSecurity = eConnectionSecurity.eCSSTARTTLSOptional;
         port.SSLCertificateID = certificate.ID;
         port.Save();

         JsonDocument withTls = Get(Path);
         ClassicAssert.IsTrue(HasFinding(withTls.RootElement, "submission", "submission_tls", TlsPort.ToString(), CertificateName), Item(withTls.RootElement, "submission").ToString());
         ClassicAssert.AreEqual("action_needed", StateOf(withTls.RootElement, "submission"), "The plain port 587 still needs action beside the secured one.");

         port.SSLCertificateID = 0;
         port.Save();

         JsonDocument noCertificate = Get(Path);
         ClassicAssert.IsTrue(HasFinding(noCertificate.RootElement, "submission", "submission_no_certificate", TlsPort.ToString()), Item(noCertificate.RootElement, "submission").ToString());
      }

      [Test]
      [Description("The backup destination is judged on the server's disk: none set, a folder that does not exist, and a folder that does.")]
      public void TheBackupDestinationIsCheckedOnDisk()
      {
         _settings.Backup.Destination = "";
         ClassicAssert.AreEqual("backup_none", FirstCode(Get(Path).RootElement, "backup"));

         string missing = System.IO.Path.Combine(TestContext.CurrentContext.TestDirectory, "no-such-folder-" + Guid.NewGuid().ToString("N"));
         _settings.Backup.Destination = missing;
         JsonDocument absent = Get(Path);
         ClassicAssert.AreEqual("action_needed", StateOf(absent.RootElement, "backup"));
         ClassicAssert.AreEqual("backup_folder_missing", FirstCode(absent.RootElement, "backup"));
         ClassicAssert.AreEqual(missing, FirstArgs(absent.RootElement, "backup")[0]);

         _settings.Backup.Destination = TestContext.CurrentContext.TestDirectory;
         JsonDocument present = Get(Path);
         ClassicAssert.AreEqual("done", StateOf(present.RootElement, "backup"), Item(present.RootElement, "backup").ToString());
         ClassicAssert.AreEqual("backup_set", FirstCode(present.RootElement, "backup"));
      }

      [Test]
      [Description("The administrator's second factor is reported as it is, with the sentence a client without a catalogue shows.")]
      public void TheSecondFactorIsReportedAsItIs()
      {
         JsonDocument document = Get(Path);
         string code = FirstCode(document.RootElement, "second_factor");
         ClassicAssert.IsTrue(code == "second_factor_on" || code == "second_factor_off", code);
         ClassicAssert.AreEqual(code == "second_factor_on" ? "done" : "action_needed", StateOf(document.RootElement, "second_factor"));
         StringAssert.Contains("administrator", FirstText(document.RootElement, "second_factor"));
      }

      // ----------------------------------------------------------------- helpers

      private static JsonDocument Get(string path)
      {
         (int status, string body) answer = Http("GET", path);
         ClassicAssert.AreEqual(200, answer.status, "GET " + path + " answered " + answer.status + ": " + answer.body);
         return JsonDocument.Parse(answer.body);
      }

      private static JsonElement Item(JsonElement root, string key)
      {
         foreach (JsonElement item in root.GetProperty("items").EnumerateArray())
         {
            if (item.GetProperty("key").GetString() == key)
               return item;
         }

         Assert.Fail("No item " + key + " in " + root);
         return default;
      }

      private static string StateOf(JsonElement root, string key)
      {
         return Item(root, key).GetProperty("state").GetString();
      }

      private static JsonElement FirstFinding(JsonElement root, string key)
      {
         foreach (JsonElement finding in Item(root, key).GetProperty("findings").EnumerateArray())
            return finding;

         Assert.Fail("No finding on " + key);
         return default;
      }

      private static string FirstCode(JsonElement root, string key)
      {
         return FirstFinding(root, key).GetProperty("code").GetString();
      }

      private static string FirstText(JsonElement root, string key)
      {
         return FirstFinding(root, key).GetProperty("text").GetString();
      }

      private static List<string> FirstArgs(JsonElement root, string key)
      {
         var args = new List<string>();
         foreach (JsonElement arg in FirstFinding(root, key).GetProperty("args").EnumerateArray())
            args.Add(arg.GetString());
         return args;
      }

      private static bool HasFinding(JsonElement root, string key, string code, params string[] leadingArgs)
      {
         foreach (JsonElement finding in Item(root, key).GetProperty("findings").EnumerateArray())
         {
            if (finding.GetProperty("code").GetString() != code)
               continue;

            JsonElement args = finding.GetProperty("args");
            bool matches = args.GetArrayLength() >= leadingArgs.Length;
            for (int i = 0; matches && i < leadingArgs.Length; i++)
               matches = args[i].GetString() == leadingArgs[i];

            if (matches)
               return true;
         }

         return false;
      }

      private void RemoveWhatWasAdded()
      {
         TCPIPPorts ports = _settings.TCPIPPorts;
         for (int i = ports.Count - 1; i >= 0; i--)
         {
            if (ports[i].PortNumber == TlsPort)
               ports.DeleteByDBID(ports[i].ID);
         }

         SSLCertificates certificates = _settings.SSLCertificates;
         for (int i = certificates.Count - 1; i >= 0; i--)
         {
            if (certificates[i].Name == CertificateName)
               certificates.DeleteByDBID(certificates[i].ID);
         }
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
