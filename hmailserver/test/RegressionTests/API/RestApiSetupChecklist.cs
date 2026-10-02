// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.SSL;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The first-hour checklist: GET /api/v1/setup/checklist and
   ///    Utilities.GetSetupChecklist, the same document over REST and over COM. Twelve
   ///    items - a setting profile, a domain, an account, TLS on the submission port, passwords only
   ///    over TLS, SPF, DKIM, DMARC, a backup destination, a second factor on the administrator
   ///    password, the stored-secrets key made and exported, and the account the server runs as -
   ///    each done, action_needed, cannot_tell or not_needed, with findings that carry a code, its
   ///    arguments and an English sentence.
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
      // NewInstallDefaults::CertificateName(): the record a new installation's defaults make.
      private const string InstallationCertificateName = "Self-signed (made at installation)";
      private const int TlsPort = 5593;

      private string _previousBackupDestination;
      private Domain _secondDomain;
      // The Internet range's id while a test has it requiring TLS for authentication,
      // so that the teardown puts it back: the Linux suite's ranges are not reset
      // between fixtures (SecurityRanges.SetDefault is a Windows COM call).
      private long _internetRangeChanged;
      // A stock listener a test gave STARTTLS, each with the step that puts it back
      // as it was: the Linux suite's listeners are not reset between fixtures either.
      private readonly List<Action> _listenersToPutBack = new List<Action>();
      // The SMTP port 25 record the delivery-port test judged: written by it on the
      // Linux suite, whose listener is 2525 (and so deleted again), or the bench's
      // own, whose SMTP authentication it puts back.
      private long _port25Written;
      private long _port25Changed;
      private string _port25ModeBefore;
      private long _minutesToBanBefore = -1;

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

            if (_internetRangeChanged != 0)
            {
               Http("PUT", "/api/v1/ipranges/" + _internetRangeChanged, "{\"require_tls_for_auth\":false}");
               _internetRangeChanged = 0;
            }

            // The listeners before the certificate they were given is deleted.
            foreach (Action putBack in _listenersToPutBack)
               putBack();
            _listenersToPutBack.Clear();

            RemoveWhatWasAdded();
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("Thirteen items in the order a new server meets them - a setting profile first, the account the server runs as last - each in one of the four states, each finding with a code, its arguments and a sentence; the fixture's domain and account are done.")]
      public void TenItemsEachWithAState()
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

         CollectionAssert.AreEqual(new[] { "profile", "domain", "account", "submission", "sign_in_tls", "delivery_port_auth", "spf", "dkim", "dmarc", "backup", "second_factor", "stored_secrets_key", "service_account" }, keys);

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
            ClassicAssert.AreEqual(13, com.RootElement.GetProperty("items").GetArrayLength());
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
         // The submission listener's port as this bench has it: 587 on the Windows
         // bench and the CI job, and whatever HMTEST_SUBMISSION_PORT names on a bench
         // that cannot bind a privileged port. The checklist judges every SMTP port
         // but 25 as submission, so the finding is for that listener, whatever its
         // number.
         string submissionPort = TestPorts.Actual(587).ToString();

         JsonDocument stock = Get(Path);
         ClassicAssert.AreEqual("action_needed", StateOf(stock.RootElement, "submission"));
         ClassicAssert.IsTrue(HasFinding(stock.RootElement, "submission", "submission_plain", submissionPort), Item(stock.RootElement, "submission").ToString());

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
         ClassicAssert.AreEqual("action_needed", StateOf(withTls.RootElement, "submission"), "The plain port " + submissionPort + " still needs action beside the secured one.");

         // A port with no certificate at all cannot be saved over COM, so the
         // case is the one an administrator meets: the certificate it names is gone.
         port.SSLCertificateID = certificate.ID + 100000;
         port.Save();

         JsonDocument noCertificate = Get(Path);
         ClassicAssert.IsTrue(HasFinding(noCertificate.RootElement, "submission", "submission_no_certificate", TlsPort.ToString()), Item(noCertificate.RootElement, "submission").ToString());
      }

      [Test]
      [Description("A listener whose certificate is the self-signed one a new installation made needs action - its TLS works, and every mail client warns about it - and is named with the certificate, where one an administrator chose is done (docs/SecureDefaults.md).")]
      public void TheInstallationsSelfSignedCertificateIsToBeReplaced()
      {
         string sslPath = SslSetup.GetSslCertPath();
         SSLCertificate certificate = _settings.SSLCertificates.Add();
         certificate.Name = InstallationCertificateName;
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

         JsonDocument document = Get(Path + "?dns=0");
         ClassicAssert.IsTrue(HasFinding(document.RootElement, "submission", "submission_self_signed", TlsPort.ToString(), InstallationCertificateName), Item(document.RootElement, "submission").ToString());
         ClassicAssert.IsFalse(HasFinding(document.RootElement, "submission", "submission_tls", TlsPort.ToString()), "The installation's certificate is not called done.");
         ClassicAssert.AreEqual("action_needed", StateOf(document.RootElement, "submission"));
         StringAssert.Contains("ACME", FindingWithCode(document.RootElement, "submission", "submission_self_signed").GetProperty("text").GetString());
      }

      [Test]
      [Description("Passwords only over TLS. The stock Internet range takes a password before TLS on listeners that have no TLS to start, so the item needs action as sign_in_no_tls, naming them, with no fix: requiring TLS there would refuse every client there. Once the listeners offer STARTTLS with a certificate, the finding is sign_in_plain and carries the one step that puts it right - the IP range update - which, sent exactly as the finding gives it, makes the item done. The loopback range is not judged. Negative control: with the requirement taken off again, sign_in_plain and its fix come back.")]
      public void PasswordsOnlyOverTlsCarriesTheStepThatPutsItRight()
      {
         SecurityRange internet = _settings.SecurityRanges.get_ItemByName("Internet");
         ClassicAssert.IsNotNull(internet, "The bench has the Internet range a new database has.");
         ClassicAssert.IsFalse(internet.RequireSSLTLSForAuth, "The bench's Internet range is as the create scripts make it.");

         // The stock listeners have no TLS at all: the step that would require it
         // is not offered, and the finding says what comes first.
         JsonDocument stock = Get(Path + "?dns=0");
         ClassicAssert.AreEqual("action_needed", StateOf(stock.RootElement, "sign_in_tls"), Item(stock.RootElement, "sign_in_tls").ToString());
         JsonElement noTls = FindingWithCode(stock.RootElement, "sign_in_tls", "sign_in_no_tls");
         ClassicAssert.AreEqual("Internet", noTls.GetProperty("args")[0].GetString());
         StringAssert.Contains(TestPorts.Actual(587).ToString(), noTls.GetProperty("args")[1].GetString(), "The listeners with no TLS to start are named.");
         ClassicAssert.IsFalse(noTls.TryGetProperty("fix", out _), "Requiring TLS where no listener can start it would lock every client out: " + noTls);
         foreach (JsonElement finding in Item(stock.RootElement, "sign_in_tls").GetProperty("findings").EnumerateArray())
            ClassicAssert.AreNotEqual("My computer", finding.GetProperty("args").GetArrayLength() > 0 ? finding.GetProperty("args")[0].GetString() : "", "The loopback range is not judged: " + finding);

         // Every listener given STARTTLS with a certificate, as a new installation's
         // are - and put back as it was by the teardown.
         string sslPath = SslSetup.GetSslCertPath();
         SSLCertificate certificate = _settings.SSLCertificates.Add();
         certificate.Name = CertificateName;
         certificate.CertificateFile = Paths.Combine(sslPath, "example.crt");
         certificate.PrivateKeyFile = Paths.Combine(sslPath, "example.key");
         certificate.Save();

         TCPIPPorts ports = _settings.TCPIPPorts;
         for (int i = 0; i < ports.Count; i++)
         {
            TCPIPPort port = ports[i];
            var id = port.ID;
            var security = port.ConnectionSecurity;
            var certificateId = port.SSLCertificateID;
            _listenersToPutBack.Add(() =>
            {
               TCPIPPorts current = _settings.TCPIPPorts;
               for (int j = 0; j < current.Count; j++)
               {
                  if (current[j].ID != id)
                     continue;

                  TCPIPPort listener = current[j];
                  listener.ConnectionSecurity = security;
                  listener.SSLCertificateID = certificateId;
                  listener.Save();
                  return;
               }
            });

            port.ConnectionSecurity = eConnectionSecurity.eCSSTARTTLSOptional;
            port.SSLCertificateID = certificate.ID;
            port.Save();
         }

         JsonDocument tlsReady = Get(Path + "?dns=0");
         JsonElement plain = FindingWithCode(tlsReady.RootElement, "sign_in_tls", "sign_in_plain");
         ClassicAssert.AreEqual("Internet", plain.GetProperty("args")[0].GetString());
         StringAssert.Contains(TestPorts.Actual(587).ToString(), plain.GetProperty("args")[1].GetString(), "The listeners that would take a password in the clear are named.");

         JsonElement fix = plain.GetProperty("fix");
         ClassicAssert.AreEqual("PUT", fix.GetProperty("method").GetString());
         ClassicAssert.AreEqual("/api/v1/ipranges/" + internet.ID, fix.GetProperty("path").GetString());
         ClassicAssert.IsTrue(fix.GetProperty("body").GetProperty("require_tls_for_auth").GetBoolean());

         _internetRangeChanged = internet.ID;
         (int status, string body) applied = Http(fix.GetProperty("method").GetString(), fix.GetProperty("path").GetString(), fix.GetProperty("body").GetRawText());
         ClassicAssert.AreEqual(200, applied.status, applied.body);

         JsonDocument secured = Get(Path + "?dns=0");
         ClassicAssert.AreEqual("done", StateOf(secured.RootElement, "sign_in_tls"), Item(secured.RootElement, "sign_in_tls").ToString());
         ClassicAssert.IsTrue(HasFinding(secured.RootElement, "sign_in_tls", "sign_in_tls_required", "Internet"), Item(secured.RootElement, "sign_in_tls").ToString());
         foreach (JsonElement finding in Item(secured.RootElement, "sign_in_tls").GetProperty("findings").EnumerateArray())
            ClassicAssert.IsFalse(finding.TryGetProperty("fix", out _), "A finding that is done carries no fix: " + finding);

         (int status, string body) undone = Http("PUT", fix.GetProperty("path").GetString(), "{\"require_tls_for_auth\":false}");
         ClassicAssert.AreEqual(200, undone.status, undone.body);
         _internetRangeChanged = 0;

         JsonDocument again = Get(Path + "?dns=0");
         ClassicAssert.IsTrue(HasFinding(again.RootElement, "sign_in_tls", "sign_in_plain", "Internet"), Item(again.RootElement, "sign_in_tls").ToString());
         ClassicAssert.IsTrue(FindingWithCode(again.RootElement, "sign_in_tls", "sign_in_plain").TryGetProperty("fix", out _));
      }

      [Test]
      [Description("No password on port 25 (GitLab work item #54): an SMTP listener on 25 whose authentication is On - an upgraded server's - needs action, with no fix, since changing it is what arms the ban; a honeypot and an Off listener are done, and with the ban minutes at 0 the finding says nobody is banned. Negative control: the same listener On again needs action again.")]
      public void Port25ThatStillTakesAuthNeedsAction()
      {
         long id = Port25Record();

         JsonDocument on = Get(Path + "?dns=0");
         ClassicAssert.AreEqual("action_needed", StateOf(on.RootElement, "delivery_port_auth"), Item(on.RootElement, "delivery_port_auth").ToString());
         ClassicAssert.IsTrue(HasFinding(on.RootElement, "delivery_port_auth", "delivery_port_auth_on", "25"), Item(on.RootElement, "delivery_port_auth").ToString());
         JsonElement onFinding = FindingWithCode(on.RootElement, "delivery_port_auth", "delivery_port_auth_on");
         ClassicAssert.IsFalse(onFinding.TryGetProperty("fix", out _), "Changing the listener arms the ban; the checklist does not offer it as a click: " + onFinding);
         StringAssert.Contains("587", onFinding.GetProperty("text").GetString());

         SetSmtpAuthentication(id, "honeypot");
         JsonDocument honeypot = Get(Path + "?dns=0");
         ClassicAssert.AreEqual("done", StateOf(honeypot.RootElement, "delivery_port_auth"), Item(honeypot.RootElement, "delivery_port_auth").ToString());
         ClassicAssert.IsTrue(HasFinding(honeypot.RootElement, "delivery_port_auth", "delivery_port_auth_honeypot", "25"));

         SetSmtpAuthentication(id, "off");
         JsonDocument off = Get(Path + "?dns=0");
         ClassicAssert.AreEqual("done", StateOf(off.RootElement, "delivery_port_auth"));
         ClassicAssert.IsTrue(HasFinding(off.RootElement, "delivery_port_auth", "delivery_port_auth_off", "25"));

         _minutesToBanBefore = Get("/api/v1/settings").RootElement.GetProperty("minutes_to_ban").GetInt64();
         (int status, string body) noBan = Http("PUT", "/api/v1/settings", "{\"minutes_to_ban\":0}");
         ClassicAssert.AreEqual(200, noBan.status, noBan.body);
         JsonDocument unbanned = Get(Path + "?dns=0");
         ClassicAssert.IsTrue(HasFinding(unbanned.RootElement, "delivery_port_auth", "delivery_port_auth_no_ban", "25"), Item(unbanned.RootElement, "delivery_port_auth").ToString());
         PutMinutesToBanBack();

         SetSmtpAuthentication(id, "on");
         JsonDocument onAgain = Get(Path + "?dns=0");
         ClassicAssert.AreEqual("action_needed", StateOf(onAgain.RootElement, "delivery_port_auth"), "Negative control: On again is not done.");
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

      [Test]
      [Description("The account the server runs as is judged from its own token (docs/ServiceAccount.md): LocalSystem, or root on Linux, needs action and names no account; any other account is done and named, in the sentence as well. Where this process can ask the service manager - the Windows suite, beside the server - the finding agrees with the account the service is registered as, which on the bench is LocalSystem; and an administrator's account says nothing of what only a lesser account cannot do.")]
      public void TheServiceAccountIsJudgedFromTheServersOwnToken()
      {
         JsonDocument document = Get(Path + "?dns=0");
         JsonElement root = document.RootElement;
         JsonElement finding = FirstFinding(root, "service_account");
         string code = finding.GetProperty("code").GetString();
         List<string> args = FirstArgs(root, "service_account");

         switch (code)
         {
            case "service_account_local_system":
            case "service_account_root":
               ClassicAssert.AreEqual("action_needed", finding.GetProperty("state").GetString(), "The most privileged account is a thing to do, as the missing profile is.");
               ClassicAssert.AreEqual("action_needed", StateOf(root, "service_account"));
               ClassicAssert.IsEmpty(args, "The most privileged account is named by the code, not by an argument.");
               StringAssert.Contains(code == "service_account_root" ? "root" : "LocalSystem", finding.GetProperty("text").GetString());
               break;
            case "service_account_own":
               ClassicAssert.AreEqual("done", finding.GetProperty("state").GetString());
               ClassicAssert.AreEqual(1, args.Count, "The account is named.");
               ClassicAssert.IsNotEmpty(args[0], "The account is named.");
               StringAssert.Contains(args[0], finding.GetProperty("text").GetString());
               break;
            default:
               Assert.Fail("The service_account item opens with " + code + ", which is none of the three: " + Item(root, "service_account"));
               break;
         }

         // An administrator - LocalSystem - can write firewall rules and run an
         // installer, so neither refusal is said for it, whatever is switched on.
         if (code == "service_account_local_system")
         {
            foreach (JsonElement other in Item(root, "service_account").GetProperty("findings").EnumerateArray())
            {
               string otherCode = other.GetProperty("code").GetString();
               ClassicAssert.AreNotEqual("autoban_firewall_not_permitted", otherCode, "LocalSystem may write firewall rules.");
               ClassicAssert.AreNotEqual("update_apply_by_administrator", otherCode, "LocalSystem may run an installer.");
            }
         }

         // The service manager's own answer, where this process can ask it.
         string registered = RegisteredLogonAccount();
         if (registered != null && code != "service_account_root")
         {
            bool localSystem = string.Equals(registered, "LocalSystem", StringComparison.OrdinalIgnoreCase) ||
                               string.Equals(registered, @".\LocalSystem", StringComparison.OrdinalIgnoreCase) ||
                               string.Equals(registered, @"NT AUTHORITY\SYSTEM", StringComparison.OrdinalIgnoreCase);
            ClassicAssert.AreEqual(localSystem ? "service_account_local_system" : "service_account_own", code,
               "The service is registered to log on as " + registered + ", and the checklist says " + code + ".");
         }
      }

      // ----------------------------------------------------------------- helpers

      // The account the hMailServer service is registered to log on as, as
      // sc.exe qc reports it, when the server is this machine, this machine is
      // Windows - the Windows suite - and the service is what is running. Null
      // anywhere else: a console server started with /Debug, which a debugger or
      // a coverage run drives, runs as whoever started it while the stopped
      // service is still registered as LocalSystem, and the two would disagree
      // about nothing the checklist got wrong. Null too when it cannot be read.
      private static string RegisteredLogonAccount()
      {
         if (Environment.OSVersion.Platform != PlatformID.Win32NT || TestPorts.Host != "127.0.0.1")
            return null;

         string state = ServiceControl("query hMailServer");
         if (state == null || !Regex.IsMatch(state, @"STATE\s*:\s*4\s+RUNNING", RegexOptions.IgnoreCase))
            return null;

         string configuration = ServiceControl("qc hMailServer");
         if (configuration == null)
            return null;

         Match match = Regex.Match(configuration, @"SERVICE_START_NAME\s*:\s*(\S.*?)\s*$", RegexOptions.Multiline);
         return match.Success ? match.Groups[1].Value : null;
      }

      // What sc.exe says to arguments; null when it cannot be run.
      private static string ServiceControl(string arguments)
      {
         try
         {
            var start = new ProcessStartInfo(System.IO.Path.Combine(Environment.SystemDirectory, "sc.exe"), arguments)
            {
               UseShellExecute = false,
               RedirectStandardOutput = true,
               CreateNoWindow = true
            };

            using (Process process = Process.Start(start))
            {
               string output = process.StandardOutput.ReadToEnd();
               process.WaitForExit(10000);
               return output;
            }
         }
         catch (Exception e) when (e is System.ComponentModel.Win32Exception || e is InvalidOperationException)
         {
            return null;
         }
      }

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

      private static JsonElement FindingWithCode(JsonElement root, string key, string code)
      {
         foreach (JsonElement finding in Item(root, key).GetProperty("findings").EnumerateArray())
         {
            if (finding.GetProperty("code").GetString() == code)
               return finding;
         }

         Assert.Fail("No finding " + code + " on " + key + ": " + Item(root, key));
         return default;
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

      /// <summary>
      ///    The id of the SMTP listener record on port 25: the bench's own, its
      ///    authentication remembered for the teardown, or - where there is none, as
      ///    on the Linux suite, which listens on 2525 - one written for the test.
      ///    The checklist reads the table, so the record is never bound: nothing
      ///    reinitialises while it exists.
      /// </summary>
      private long Port25Record()
      {
         foreach (JsonElement port in Get("/api/v1/ports").RootElement.EnumerateArray())
         {
            if (port.GetProperty("protocol").GetString() == "smtp" && port.GetProperty("port").GetInt32() == 25)
            {
               _port25Changed = port.GetProperty("id").GetInt64();
               _port25ModeBefore = port.GetProperty("smtp_authentication").GetString();
               if (_port25ModeBefore != "on")
                  SetSmtpAuthentication(_port25Changed, "on");
               return _port25Changed;
            }
         }

         (int status, string body) created = Http("POST", "/api/v1/ports", "{\"protocol\":\"smtp\",\"address\":\"0.0.0.0\",\"port\":25}");
         ClassicAssert.AreEqual(201, created.status, created.body);
         using (JsonDocument document = JsonDocument.Parse(created.body))
            _port25Written = document.RootElement.GetProperty("id").GetInt64();
         return _port25Written;
      }

      /// <summary>PUT takes the whole port: what the listing says, with the mode changed.</summary>
      private static void SetSmtpAuthentication(long id, string mode)
      {
         foreach (JsonElement port in Get("/api/v1/ports").RootElement.EnumerateArray())
         {
            if (port.GetProperty("id").GetInt64() != id)
               continue;

            var body = new Dictionary<string, object>
            {
               ["protocol"] = port.GetProperty("protocol").GetString(),
               ["address"] = port.GetProperty("address").GetString(),
               ["port"] = port.GetProperty("port").GetInt32(),
               ["connection_security"] = port.GetProperty("connection_security").GetString(),
               ["certificate_id"] = port.GetProperty("certificate_id").GetInt64(),
               ["client_certificate_policy"] = port.GetProperty("client_certificate_policy").GetString(),
               ["client_certificate_ca_file"] = port.GetProperty("client_certificate_ca_file").GetString(),
               ["smtp_authentication"] = mode
            };

            (int status, string answer) = Http("PUT", "/api/v1/ports/" + id, JsonSerializer.Serialize(body));
            ClassicAssert.AreEqual(200, status, answer);
            return;
         }

         Assert.Fail("No port " + id + " is listed.");
      }

      private void PutMinutesToBanBack()
      {
         if (_minutesToBanBefore < 0)
            return;

         Http("PUT", "/api/v1/settings", "{\"minutes_to_ban\":" + _minutesToBanBefore + "}");
         _minutesToBanBefore = -1;
      }

      private void RemoveWhatWasAdded()
      {
         PutMinutesToBanBack();

         if (_port25Written != 0)
         {
            Http("DELETE", "/api/v1/ports/" + _port25Written);
            _port25Written = 0;
         }

         if (_port25Changed != 0)
         {
            SetSmtpAuthentication(_port25Changed, _port25ModeBefore ?? "on");
            _port25Changed = 0;
         }

         TCPIPPorts ports = _settings.TCPIPPorts;
         for (int i = ports.Count - 1; i >= 0; i--)
         {
            if (ports[i].PortNumber == TlsPort)
               ports.DeleteByDBID(ports[i].ID);
         }

         SSLCertificates certificates = _settings.SSLCertificates;
         for (int i = certificates.Count - 1; i >= 0; i--)
         {
            if (certificates[i].Name == CertificateName || certificates[i].Name == InstallationCertificateName)
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
