// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Security
{
   /// <summary>
   ///    A host added to AcmeDomains gets a certificate that names it, end to end, over
   ///    COM (Utilities.RunAcmeCheck and GetAcmeStatus) - and a certificate the CA hands
   ///    back that is not the one asked for is not installed (GitLab work item 98).
   ///
   ///    The CA is FakeAcmeCa, which issues whatever this fixture gives it. The pair in
   ///    use is made here, at run time, so that no private key is written into the
   ///    repository: an EC key, the certificate in use for mail.example.test, and the one
   ///    the CA hands out for mail.example.test and new.example.test with the same key -
   ///    the server reuses the key it holds (AcmeReuseKey, on by default), so that is the
   ///    key its request is made with. The certificate for another key is the CA's
   ///    mistake the server has to catch.
   ///
   ///    Windows only, and not linked by the Linux suite: with both halves of the pair in
   ///    the certificate directory the REST listener, which that suite drives everything
   ///    through, would start serving HTTPS with them. Here REST is off and COM is used.
   /// </summary>
   [TestFixture]
   public class TlsAcmeIssuance : TestFixtureBase
   {
      // 10281 and 10282 are this fixture's CAs, outside RestApiAcme's 10270 to 10280:
      // the spacing between checks is kept against the CA's directory URL for the
      // life of the process, so a CA that fixture left in its cooldown would hold
      // this one's checks.
      private const int FirstCaPort = 10281;
      private const int SecondCaPort = 10282;

      private const string RecordName = "ACME (automatic)";

      private readonly Dictionary<int, int> _portCertificates = new Dictionary<int, int>();

      private string AcmeDirectory
      {
         get { return Paths.Combine(_application.Settings.Directories.ProgramDirectory, "hm_acme_issuance_test"); }
      }

      private string CertificateFile
      {
         get { return Paths.Combine(AcmeDirectory, "fullchain.pem"); }
      }

      [SetUp]
      public void RecordPortCertificates()
      {
         _portCertificates.Clear();
         TCPIPPorts ports = _application.Settings.TCPIPPorts;
         for (int i = 0; i < ports.Count; i++)
            _portCertificates[ports[i].ID] = ports[i].SSLCertificateID;
      }

      [TearDown]
      public void DisableAcmeAndRestore()
      {
         try
         {
            // ACME off where it applies, and the bench quiet, before the record goes:
            // a check at the start a deployment made would otherwise find the pair
            // with no record, deploy it again and restart the servers into the next
            // fixture (AcmeBench).
            AcmeBench.SwitchOff(_application);

            IniFileSetting.Write("AcmeDomains", null);
            IniFileSetting.Write("AcmeDirectoryUrl", null);
            IniFileSetting.Write("AcmeCertificateDirectory", null);

            TCPIPPorts ports = _application.Settings.TCPIPPorts;
            ports.Refresh();
            for (int i = 0; i < ports.Count; i++)
            {
               TCPIPPort port = ports[i];
               int before;
               if (_portCertificates.TryGetValue(port.ID, out before) && port.SSLCertificateID != before)
               {
                  port.SSLCertificateID = before;
                  port.Save();
               }
            }

            SSLCertificate record = FindRecord();
            while (record != null)
            {
               record.Delete();
               record = FindRecord();
            }

            _application.Reinitialize();
         }
         finally
         {
            try
            {
               if (Directory.Exists(AcmeDirectory))
                  Directory.Delete(AcmeDirectory, true);
            }
            catch (IOException)
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
         }
      }

      // ------------------------------------------------------------------ helpers

      private SSLCertificate FindRecord()
      {
         SSLCertificates certificates = _application.Settings.SSLCertificates;
         certificates.Refresh();
         for (int i = 0; i < certificates.Count; i++)
         {
            if (certificates[i].Name == RecordName)
               return certificates[i];
         }
         return null;
      }

      private Dictionary<string, object> Status()
      {
         return MiniJson.Object(_application.Utilities.GetAcmeStatus());
      }

      private Dictionary<string, object> WaitFor(Func<Dictionary<string, object>, bool> condition, string what, int seconds = 60)
      {
         return AcmeBench.WaitFor(_application, condition, what, seconds);
      }

      // An EC key the CNG provider lets this process export, so that it can be
      // written as the PKCS#8 file the server reads.
      private static ECDsaCng MakeKey()
      {
         var parameters = new CngKeyCreationParameters { ExportPolicy = CngExportPolicies.AllowPlaintextExport };
         return new ECDsaCng(CngKey.Create(CngAlgorithm.ECDsaP256, null, parameters));
      }

      private static string Pem(string label, byte[] der)
      {
         string body = Convert.ToBase64String(der, Base64FormattingOptions.InsertLineBreaks).Replace("\r\n", "\n");
         // The armour written in two halves, so that the source is not mistaken for
         // a key by a secret scanner: the key is made when the test runs.
         return "-----BEGIN " + label + "-----\n" + body + "\n-----END " + label + "-----\n";
      }

      private static string Certificate(ECDsa key, params string[] names)
      {
         var request = new CertificateRequest("CN=" + names[0], key, HashAlgorithmName.SHA256);
         var alternatives = new SubjectAlternativeNameBuilder();
         foreach (string name in names)
            alternatives.AddDnsName(name);
         request.CertificateExtensions.Add(alternatives.Build());

         using (X509Certificate2 certificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(20)))
            return Pem("CERTIFICATE", certificate.RawData);
      }

      // ------------------------------------------------------------------ the test

      [Test]
      [Description("A host added to AcmeDomains is issued for at once and the new certificate installed, covering it; before that, a certificate the CA sent for another key was refused and the certificate in use kept byte for byte.")]
      public void AHostAddedToAcmeDomainsIsIssuedForAndAWrongCertificateIsRefused()
      {
         using (ECDsaCng key = MakeKey())
         using (ECDsaCng otherKey = MakeKey())
         using (var first = new FakeAcmeCa(FirstCaPort))
         using (var second = new FakeAcmeCa(SecondCaPort))
         {
            Directory.CreateDirectory(AcmeDirectory);
            File.WriteAllText(CertificateFile, Certificate(key, "mail.example.test"), new UTF8Encoding(false));
            File.WriteAllText(Paths.Combine(AcmeDirectory, "privkey.pem"),
               Pem("PRIVATE KEY", key.Key.Export(CngKeyBlobFormat.Pkcs8PrivateBlob)), new UTF8Encoding(false));
            byte[] inUse = File.ReadAllBytes(CertificateFile);

            // The CA's mistake: both names, and another key.
            first.CertificatePem = Certificate(otherKey, "mail.example.test", "new.example.test");

            IniFileSetting.Write("AcmeCertificateDirectory", AcmeDirectory);
            IniFileSetting.Write("AcmeDirectoryUrl", first.DirectoryUrl);
            IniFileSetting.Write("AcmeDomains", "mail.example.test");
            IniFileSetting.Write("AcmeEnabled", "1");
            _application.Reinitialize();

            // The check at start: nothing due, and the pair, which no record names yet,
            // is deployed - which restarts the servers.
            WaitFor(s => MiniJson.Text(s, "last_attempt.trigger") == "start" && MiniJson.Text(s, "last_attempt.outcome") == "not_due" &&
                         MiniJson.Flag(s, "certificate.deployed"),
               "the check at start to find nothing due and deploy the pair");
            Assert.AreEqual(0, first.Count("POST", "/order"));

            // The save that adds the host starts a check - or, when the check at the start
            // the deployment's restart made is running then, that check reads the new list
            // and issues, and the save's check finds the cooldown its failure left. Either
            // way the first attempt to issue after the save is the one to read, whatever
            // started it: the save's own trigger is RestApiAcme's to hold.
            long before = MiniJson.Number(Status(), "last_issuance.id");
            IniFileSetting.Write("AcmeDomains", "mail.example.test,new.example.test");

            Dictionary<string, object> refused = WaitFor(s => MiniJson.Number(s, "last_issuance.id") > before && MiniJson.At(s, "running") == null,
               "the check the save started to end");
            Assert.AreEqual("failed", MiniJson.Text(refused, "last_issuance.outcome"), "A certificate for another key was installed.");
            Assert.AreEqual("certificate", MiniJson.Text(refused, "last_issuance.failed_step"));
            StringAssert.Contains("not for the key", MiniJson.Text(refused, "last_issuance.error"));
            Assert.AreEqual(1, first.Count("POST", "/cert/1"), "The certificate was never downloaded, so nothing was refused.");
            CollectionAssert.AreEqual(inUse, File.ReadAllBytes(CertificateFile), "The certificate in use was replaced by one for another key.");
            Assert.IsFalse(File.Exists(CertificateFile + ".new"));

            // The CA put right: another directory, so the cooldown the failure left at the
            // first one holds nothing here, and a request for a check over COM.
            second.CertificatePem = Certificate(key, "mail.example.test", "new.example.test");
            IniFileSetting.Write("AcmeDirectoryUrl", second.DirectoryUrl);

            Dictionary<string, object> accepted = MiniJson.Object(_application.Utilities.RunAcmeCheck());
            Assert.IsTrue(MiniJson.Flag(accepted, "accepted"), "The request was not accepted: " + MiniJson.Text(accepted, "error"));
            long id = MiniJson.Number(accepted, "attempt.id");

            // last_issuance, not last_attempt: the deployment restarts the servers, and the
            // check at that start, which finds nothing due, is the last attempt then.
            Dictionary<string, object> issued = WaitFor(s => MiniJson.Number(s, "last_issuance.id") == id && MiniJson.At(s, "running") == null,
               "the requested check to issue", 90);
            Assert.AreEqual("request", MiniJson.Text(issued, "last_issuance.trigger"));
            Assert.AreEqual("issued", MiniJson.Text(issued, "last_issuance.outcome"), "Error: " + MiniJson.Text(issued, "last_issuance.error"));
            Assert.AreEqual("names", MiniJson.Text(issued, "last_issuance.reason"));
            Assert.IsTrue(MiniJson.Flag(issued, "covers_domains"));
            CollectionAssert.AreEqual(new[] { "mail.example.test", "new.example.test" },
               MiniJson.List(issued, "certificate.names").Cast<string>().ToArray());
            Assert.IsTrue(MiniJson.Number(issued, "last_issued_at") > 0);
            Assert.AreEqual(second.CertificatePem, File.ReadAllText(CertificateFile).Replace("\r\n", "\n"), "The certificate the CA issued is not the one in use.");

            // The installation restarts the servers, and the check at that start - an id
            // after the one that issued - finds the certificate it installed, names every
            // host and orders nothing. Waited for here, so that no restart and no check
            // of this test's is still to come when it ends.
            Dictionary<string, object> settled = WaitFor(s => MiniJson.Text(s, "last_attempt.trigger") == "start" &&
                                                              MiniJson.Number(s, "last_attempt.id") > id && MiniJson.At(s, "running") == null,
               "the check at the start the installation made");
            Assert.AreEqual("not_due", MiniJson.Text(settled, "last_attempt.outcome"), "Error: " + MiniJson.Text(settled, "last_attempt.error"));
            Assert.IsTrue(MiniJson.Flag(settled, "certificate.deployed"));

            // COM answers "has not loaded its configuration" while the servers come back.
            SSLCertificate record = null;
            RetryHelper.TryAction(TimeSpan.FromSeconds(60), () => record = FindRecord());
            Assert.IsNotNull(record, "The deployment's record is gone.");
            Assert.AreEqual(CertificateFile, record.CertificateFile);
         }
      }
   }
}
