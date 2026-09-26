// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.Pkcs;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Gateway encryption to a partner domain (Common/Util/GatewayEncryption.h,
   ///    docs/RemoteDomains.md "Gateway encryption"): a remote domain policy that
   ///    says so has mail to its domain encrypted at this server to the partner's
   ///    own key, whatever the sender's client did.
   ///
   ///    The partner is the suite's SMTP simulator, reached through a route of this
   ///    fixture's own to a domain of its own (partner-gateway.test, so that a
   ///    policy left behind by a failed teardown encrypts nothing any other
   ///    fixture sends) - a policy applies to a routed delivery exactly as to an MX
   ///    one - and what it received is opened here with the partner's private
   ///    half, which the test made: an RSA certificate made with
   ///    CertificateRequest, whose envelope .NET's EnvelopedCms opens, and for
   ///    OpenPGP the fixture key GnuPG made for the webmail's ring
   ///    (OpenPgpTestSupport), which a gpg on the bench opens - Git for Windows
   ///    carries one. Where no gpg can be had, or it cannot import the fixture's
   ///    key, that proof is reported INCONCLUSIVE, never passed; where gpg
   ///    imported the key and cannot open what the server sent, the test fails.
   ///
   ///    Each behaviour has its negative control beside it: the recipient of
   ///    another domain in the same submission, who gets the message as it was -
   ///    behind the same smart host too, and after the partner's copy was made;
   ///    a message already encrypted, which goes as it is, beside one only
   ///    LABELLED encrypted, which does not; a signature check that fails once one
   ///    byte of the body is changed; the fallback that sends against the one
   ///    that refuses; a refused save beside the delivery it must not change.
   ///
   ///    The new COM members bind late, as OpenPgpTestSupport's do: the test project
   ///    is compiled against the type library registered on the machine, which
   ///    carries them only once the built server has been registered.
   /// </summary>
   [TestFixture]
   public class GatewayEncryption : TestFixtureBase
   {
      // REST 9976, the simulators 9977 and 9978: this row's, allocated with the others (9976-9985).
      private static int RestPort = 9976;
      private const int PartnerPort = 9977;
      private const int OtherPort = 9978;

      private const string AdminPassword = "testar";
      private const string PartnerDomain = "partner-gateway.test";
      private const string OtherDomain = "other-gateway.test";

      private const int EncryptNone = 0;
      private const int EncryptOpenPgp = 1;
      private const int EncryptSmime = 2;
      private const int FallbackRefuse = 0;
      private const int FallbackSendUnencrypted = 1;

      // alice's cv25519 subkey, the one a message to her key is encrypted to,
      // as gpg --fingerprint --fingerprint prints it.
      private const string AliceSubkeyFingerprint = "4a144aada87aad8cce8a0659eaa03821629d0473";

      // A PKCS#8 Ed25519 private key, made with openssl genpkey for this test
      // and used for nothing: what the save must refuse wherever it appears.
      private const string PrivateKeyPem =
         "-----BEGIN PRIVATE KEY-----\r\n" +
         "MC4CAQAwBQYDK2VwBCIEIO4QV/wBqaxmLDDwHjmuU0tivuxEbclvJ6/1TOUsSdyQ\r\n" +
         "-----END PRIVATE KEY-----\r\n";

      private Account _account;
      private readonly List<X509Certificate2> _certificates = new List<X509Certificate2>();

      [SetUp]
      public void StartRestApiAndClearPolicies()
      {
         ClearPolicies_();
         EmptyTheTrace_();

         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin_("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "gateway-sender@" + _domain.Name, "test");
      }

      [TearDown]
      public void StopRestApiAndClearPolicies()
      {
         try
         {
            ClearPolicies_();
            EmptyTheTrace_();

            foreach (X509Certificate2 certificate in _certificates)
               certificate.Dispose();
            _certificates.Clear();
         }
         finally
         {
            ServerIniFile.SetSetting("MessageTraceEnabled", null);
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // --------------------------------------------------------------- OpenPGP

      [Test]
      [Description("A policy that encrypts with OpenPGP: a plain message arrives at the partner as PGP/MIME with its Subject and Message-ID kept, the body nowhere in it, the mark and the log naming the primary key and the subkey encrypted to - and it opens, with gpg, to the original.")]
      public void AnOpenPgpPartnerGetsPgpMimeWithTheSubjectAndMessageIdKept()
      {
         AddPolicy_(PartnerDomain, EncryptOpenPgp, OpenPgpTestSupport.RingOwnPublicBase64, FallbackRefuse);

         const string body = "The quarterly figures go under the partner's key, and nowhere else in the clear.";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            AddRoute_(PartnerDomain, PartnerPort, 1);

            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "Quarterly figures", "figures-1@example.test", body));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string received = partner.MessageData;
            string header = HeaderOf_(received);
            string keyName = OpenPgpTestSupport.RingOwnFingerprint + " subkey " + AliceSubkeyFingerprint;

            StringAssert.Contains("multipart/encrypted", header);
            StringAssert.Contains("protocol=\"application/pgp-encrypted\"", header);
            StringAssert.Contains("Subject: Quarterly figures\r\n", header, "The Subject is not hidden, and is kept.");
            StringAssert.Contains("Message-ID: <figures-1@example.test>\r\n", header, "The Message-ID is kept.");
            StringAssert.Contains("X-hMailServer-Gateway-Encrypted: OpenPGP to key " + keyName + "\r\n", header,
               "The mark does not name the primary key and the subkey the message was encrypted to.");
            StringAssert.Contains("-----BEGIN PGP MESSAGE-----", received);
            StringAssert.DoesNotContain(body, received, "The body reached the partner in the clear.");
            StringAssert.DoesNotContain("nowhere else in the clear", received);
            Assert.AreEqual(1, Regex.Matches(header, "^MIME-Version:", RegexOptions.Multiline).Count, "MIME-Version appears exactly once.");

            Assert.IsTrue(LogHandler.DefaultLogContains("Encrypted with OpenPGP to key " + keyName),
               "The application log does not name the key and the subkey the batch was encrypted to.");
            Assert.IsTrue(LogHandler.DefaultLogContains("as the remote domain policy for " + PartnerDomain + " requires"),
               "The application log does not name the policy.");

            // GnuPG's word on the body, last: everything above holds whether or not there is a gpg.
            string opened = OpenWithGpg_(received);
            StringAssert.Contains(body, opened, "GnuPG's plaintext is not the body that was sent.");
            StringAssert.Contains("Subject: Quarterly figures", opened, "The whole original, headers and all, is inside.");
         }
      }

      [Test]
      [Description("The negative control: in the same submission, a recipient of another domain - one no policy encrypts, reached by a route of its own - gets the message as it was, while the partner's copy is encrypted.")]
      public void ARecipientOfAnotherDomainInTheSameSubmissionGetsItPlain()
      {
         AddPolicy_(PartnerDomain, EncryptOpenPgp, OpenPgpTestSupport.RingOwnPublicBase64, FallbackRefuse);

         const string body = "One submission, two domains, one of them a partner.";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         using (SmtpServerSimulator other = Simulator_(OtherPort, "someone@" + OtherDomain))
         {
            AddRoute_(PartnerDomain, PartnerPort, 1);
            AddRoute_(OtherDomain, OtherPort, 1);

            new SmtpClientSimulator().SendRaw(_account.Address, new List<string> { "test@" + PartnerDomain, "someone@" + OtherDomain },
               PlainMessage_("test@" + PartnerDomain + ", someone@" + OtherDomain, "Two domains", "two-domains@example.test", body));

            partner.WaitForCompletion();
            other.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            StringAssert.Contains("multipart/encrypted", HeaderOf_(partner.MessageData));
            StringAssert.DoesNotContain(body, partner.MessageData, "The partner's copy carries the body in the clear.");

            StringAssert.Contains(body, other.MessageData, "The recipient no policy encrypts for did not get the message as it was.");
            StringAssert.DoesNotContain("multipart/encrypted", other.MessageData);
            StringAssert.DoesNotContain("X-hMailServer-Gateway-Encrypted", other.MessageData);
         }
      }

      [Test]
      [Description("Two domains behind ONE smart host - two routes to the same host and port, which the server merges into one delivery target - are sent in two transactions: the partner's recipient alone in an encrypted one, the other domain's alone in a plain one. The other recipient is turned away with a 450 the first time, so its copy is sent on the retry, after the partner's copy was made: a server that had encrypted the queued message in place would send it that ciphertext.")]
      public void RecipientsOfTwoDomainsBehindOneSmartHostAreSentInTransactionsOfTheirOwn()
      {
         AddPolicy_(PartnerDomain, EncryptOpenPgp, OpenPgpTestSupport.RingOwnPublicBase64, FallbackRefuse);

         string partnerAddress = "test@" + PartnerDomain;
         string otherAddress = "someone@" + OtherDomain;
         const string body = "One smart host, two domains, one of them a partner.";

         // Three sessions: the two of the first pass, in whichever order the
         // server takes them, then the retry for the recipient turned away.
         var smartHost = new SmtpServerSimulator(3, PartnerPort, eConnectionSecurity.eCSNone);
         smartHost.AddRecipientResult(new Dictionary<string, int> { [partnerAddress] = 250, [otherAddress] = 450 });
         smartHost.AddRecipientResult(new Dictionary<string, int> { [partnerAddress] = 250, [otherAddress] = 450 });
         smartHost.AddRecipientResult(new Dictionary<string, int> { [partnerAddress] = 250, [otherAddress] = 250 });
         smartHost.StartListen();

         using (smartHost)
         {
            AddRoute_(PartnerDomain, PartnerPort, 3);
            AddRoute_(OtherDomain, PartnerPort, 3);

            new SmtpClientSimulator().SendRaw(_account.Address, new List<string> { partnerAddress, otherAddress },
               PlainMessage_(partnerAddress + ", " + otherAddress, "One smart host", "one-smart-host@example.test", body));

            // Forces the retry once the first pass has turned the other recipient away.
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            smartHost.WaitForCompletion();

            List<List<string>> transactions = Transactions_(smartHost.CommandsReceived);
            Assert.IsFalse(transactions.Any(recipients => recipients.Count > 1),
               "A transaction carried recipients of both domains: " + string.Join(" | ", smartHost.CommandsReceived));

            Assert.AreEqual(2, smartHost.Messages.Count, "Two messages were not accepted: " + string.Join(" | ", smartHost.CommandsReceived));

            // The first accepted is the partner's, made on the first pass.
            string encrypted = smartHost.Messages[0];
            StringAssert.Contains("multipart/encrypted", HeaderOf_(encrypted), "The partner's copy was not encrypted.");
            StringAssert.Contains("X-hMailServer-Gateway-Encrypted: OpenPGP to key", HeaderOf_(encrypted));
            StringAssert.DoesNotContain(body, encrypted, "The partner's copy carries the body in the clear.");

            // The second is the other domain's, on the retry: the message as it was written.
            string plain = smartHost.Messages[1];
            StringAssert.Contains(body, plain, "The other domain's recipient, sent after the partner's copy was made, did not get the message as it was.");
            StringAssert.DoesNotContain("multipart/encrypted", plain);
            StringAssert.DoesNotContain("X-hMailServer-Gateway-Encrypted", plain);

            // And each went to the right recipient: the DATA of each transaction
            // follows the RCPT of the recipient it was for.
            List<string> accepted = AcceptedTransactionRecipients_(smartHost.CommandsReceived);
            CollectionAssert.AreEqual(new[] { partnerAddress, otherAddress }, accepted,
               "The accepted transactions were not the partner's and then the other domain's: " + string.Join(" | ", smartHost.CommandsReceived));
         }
      }

      [Test]
      [Description("A message that is PGP/MIME already - a real OpenPGP message GnuPG encrypted, a session key packet and encrypted data - goes to the partner as it is: the same body bytes, no second layer, and the log says why. The negative controls: a message only LABELLED PGP/MIME, whose PGP MESSAGE is readable literal data, is encrypted; and so is the real message with a readable third part beside the ciphertext.")]
      public void AMessageAlreadyPgpMimeGoesAsItIs()
      {
         AddPolicy_(PartnerDomain, EncryptOpenPgp, OpenPgpTestSupport.RingOwnPublicBase64, FallbackRefuse);
         AddRoute_(PartnerDomain, PartnerPort, 1);

         // build/fixtures/openpgp/pgpmime-encrypted.eml's armour: GnuPG 2.4's
         // message to alice.
         const string armour =
            "-----BEGIN PGP MESSAGE-----\r\n" +
            "\r\n" +
            "hF4D6qA4IWKdBHMSAQdA6emwvyiy/TDZE0+zHYj+mplNqFmvhdjmgb4BWbk5rgAw\r\n" +
            "I6xeASrm1twVIZzpvEQQ0GJzfnO5mCvNOt7fVSXNq6q3kqxV7Qh7Q7J/vJNsUJri\r\n" +
            "0ukBJQVdECh+m30gjUL31Gd98VHj1ToiT/ZCQB6Nrw7IYRV2JswWlFANxQSiLUGW\r\n" +
            "y8qiB2YmFP2kDVJzHh7aUVhOoeYQDY2H/STpiBAJotQpWDegZfM16m50ZDB1p8ma\r\n" +
            "8CQXmboUYggFGfJoZPa6lmbsFppXrn5oF/lcZ1X+vOfY0Q/bZj8EP7CiChcGkJVo\r\n" +
            "TYzZATmxgz+iY4bdAhJnxQiLNRUDzFXsXQSpBULFLpFaAuM1E9ZBQVjfc320rfQi\r\n" +
            "dl3uJnIf64YOXem3miBz741HQOPFH8lZoJr4cqVQ7WIqOD6wr1A9rezKGiuLXQvV\r\n" +
            "4UBC9LKSOHevD7WeKCMPYT/UiO1+XY3g8g1XA+fwVTMCcV8jBRg9Z+GW2MERHHtw\r\n" +
            "2c6g0cBFB4QKL3bgQ98X7oFHoHEZlAoYgoG9n4fFzkGvFp6yRaM9JiCzNJXloSIW\r\n" +
            "moVh2ZDqFUQJX4wcVNPSWseNuWGZ9XQjE5206RuYGKo6mfinEpVzDoA6LSDfBpz9\r\n" +
            "3opeJFM9egzguMHnLzMBp3jfXDpD4F1TyKK6PSaq1tL2jdKwB+xHiAjpG+8HVWaj\r\n" +
            "/jc914D3dKK2hQdDVgCoz/HLhYGaItzLPzKYub/zBW6nGwJKHQTGM5uOvlf0qKxc\r\n" +
            "EIcaUhud7TsUiYdI87uGO/y22SVwhKHpmtxpoS/HT8LZFbuwQAlpYQIkowoT9jS5\r\n" +
            "Z+HrMKr+CcOTADfVSZG/G4dmWi8oWAJ71lV6V2EbRmYhQMo21hUT+57xPAUjwZnv\r\n" +
            "mT+tb7Mt1cSYGiT5QWAlvpqI1nQoCcsPjh25z/wC391HkzrzZIyKMKcjaqXk0QAX\r\n" +
            "bJlZeHkGEJNLftmaW30ZTVWnwUelf2LgJksCdL1MZSt/bMYur0ZtBVubeGddGyAi\r\n" +
            "Xp3IBU/tlm+mi5lv5B4O6t3iFDkIaSQylZeFdeVj\r\n" +
            "=tvHb\r\n" +
            "-----END PGP MESSAGE-----\r\n";

         string body = PgpMimeBody_(armour);

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PgpMimeMessage_("Sealed by the client", "sealed-by-client@example.test", body));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            Assert.AreEqual(body, BodyOf_(partner.MessageData), "A message encrypted end to end already was not sent as it arrived.");
            StringAssert.DoesNotContain("X-hMailServer-Gateway-Encrypted", partner.MessageData, "A second layer was put on a sealed message.");
            Assert.IsTrue(LogHandler.DefaultLogContains("the message is encrypted end to end already. It is sent as it is."),
               "The log does not say why the policy did not encrypt the message.");
         }

         // The label without the substance: the PGP MESSAGE is a literal data
         // packet - "hi", readable by anyone who decodes the base64.
         const string readable = "-----BEGIN PGP MESSAGE-----\r\n\r\nywhiAAAAAABoaQ==\r\n-----END PGP MESSAGE-----\r\n";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PgpMimeMessage_("Only labelled sealed", "labelled-only@example.test", PgpMimeBody_(readable)));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            StringAssert.Contains("X-hMailServer-Gateway-Encrypted: OpenPGP to key", HeaderOf_(partner.MessageData),
               "A message only labelled PGP/MIME, its content readable, went to the partner as it was.");
            StringAssert.DoesNotContain("ywhiAAAAAABoaQ==", partner.MessageData, "The readable content reached the partner outside the key.");
         }

         // Real ciphertext with a passenger: the same GnuPG message and, beside
         // it, a readable part. Before the fix the ciphertext alone answered
         // "encrypted end to end already", and the third part went to a partner
         // whose policy says every message is encrypted - in the clear.
         const string passenger = "A readable part beside real ciphertext.";
         string withPassenger = PgpMimeBody_(armour).Replace("--sealed-by-the-client--\r\n",
            "--sealed-by-the-client\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\n" + passenger + "\r\n--sealed-by-the-client--\r\n");
         StringAssert.Contains(passenger, withPassenger, "The fixture was not built.");

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PgpMimeMessage_("Ciphertext and a passenger", "passenger@example.test", withPassenger));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            StringAssert.Contains("X-hMailServer-Gateway-Encrypted: OpenPGP to key", HeaderOf_(partner.MessageData),
               "Real ciphertext with a readable part beside it went to the partner as it was.");
            StringAssert.DoesNotContain(passenger, partner.MessageData, "The readable part reached the partner outside the key.");
         }
      }

      // ---------------------------------------------------------------- S/MIME

      [Test]
      [Description("A policy that encrypts with S/MIME: the partner gets application/pkcs7-mime enveloped-data, AES-256-CBC, the content key sent by RSAES-OAEP, that opens with the certificate's private key to the original message; the trace records an encrypted event naming the key; nothing is left in the server's temporary directory.")]
      public void AnSmimePartnerGetsEnvelopedDataThatOpensToTheOriginal()
      {
         ServerIniFile.SetSetting("MessageTraceEnabled", "1");
         _application.Reinitialize();

         X509Certificate2 certificate = GatewayCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);
         string fingerprint = Sha256Hex_(certificate.RawData);

         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(certificate), FallbackRefuse);

         const string body = "Under the partner's certificate, and readable by nobody on the way.";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            AddRoute_(PartnerDomain, PartnerPort, 1);
            string[] tempBefore = TempFiles_();

            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "Enveloped for the partner", "enveloped-1@example.test", body));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string received = partner.MessageData;
            string header = HeaderOf_(received);

            StringAssert.Contains("Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=\"smime.p7m\"\r\n", header);
            StringAssert.Contains("Content-Disposition: attachment; filename=\"smime.p7m\"\r\n", header);
            StringAssert.Contains("Subject: Enveloped for the partner\r\n", header);
            StringAssert.Contains("Message-ID: <enveloped-1@example.test>\r\n", header);
            StringAssert.Contains("X-hMailServer-Gateway-Encrypted: S/MIME to key " + fingerprint + "\r\n", header);
            StringAssert.DoesNotContain(body, received, "The body reached the partner in the clear.");

            string inner = OpenEnvelope_(received, certificate, out string contentAlgorithm, out string keyTransport);
            Assert.AreEqual("2.16.840.1.101.3.4.1.42", contentAlgorithm, "The content is not under AES-256-CBC.");
            Assert.AreEqual("1.2.840.113549.1.1.7", keyTransport, "The content key was not sent by RSAES-OAEP.");
            StringAssert.Contains(body, inner, "The envelope does not open to the body that was sent.");
            StringAssert.Contains("Subject: Enveloped for the partner", inner, "The whole original, headers and all, is inside.");
            StringAssert.Contains("Content-Type: text/plain", inner, "The original's own MIME is inside.");

            var trace = _application.GlobalObjects.MessageTrace;
            trace.Search("test@" + PartnerDomain);
            bool recorded = false;
            for (int i = 0; i < trace.Count; i++)
            {
               if (trace[i].EventName == "encrypted" && trace[i].Detail.Contains(fingerprint) && trace[i].Detail.Contains(PartnerDomain))
                  recorded = true;
            }
            Assert.IsTrue(recorded, "The message trace has no encrypted event naming the key and the policy.");

            AssertNoNewTempFiles_(tempBefore);
         }
      }

      [Test]
      [Description("A message that is S/MIME enveloped-data already - made here with EnvelopedCms - goes to the partner as it is. The negative control: opaque SIGNED data, readable by anyone, labelled enveloped-data, is enveloped for the partner: what is under a key is decided by the CMS object, not the label.")]
      public void AnSmimeMessageAlreadyEnvelopedGoesAsItIsAndALabelDoesNotCount()
      {
         X509Certificate2 gateway = GatewayCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);
         X509Certificate2 person = GatewayCertificate_("CN=test." + PartnerDomain, "test@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment | X509KeyUsageFlags.DigitalSignature);

         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(gateway), FallbackRefuse);
         AddRoute_(PartnerDomain, PartnerPort, 1);

         var enveloped = new EnvelopedCms(new ContentInfo(Encoding.ASCII.GetBytes("Content-Type: text/plain\r\n\r\nSealed by the client for the person.\r\n")));
         enveloped.Encrypt(new CmsRecipient(person));
         string envelopedBody = Convert.ToBase64String(enveloped.Encode(), Base64FormattingOptions.InsertLineBreaks) + "\r\n";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               Pkcs7Message_("Enveloped by the client", "enveloped-by-client@example.test", envelopedBody));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            Assert.AreEqual(envelopedBody, BodyOf_(partner.MessageData), "A message enveloped end to end already was not sent as it arrived.");
            StringAssert.DoesNotContain("X-hMailServer-Gateway-Encrypted", partner.MessageData, "A second layer was put on an enveloped message.");
         }

         const string signedText = "Signed by the client, and readable by anyone who decodes it.";
         var signed = new SignedCms(new ContentInfo(Encoding.ASCII.GetBytes("Content-Type: text/plain\r\n\r\n" + signedText + "\r\n")), false);
         signed.ComputeSignature(new CmsSigner(person), true);
         string signedBody = Convert.ToBase64String(signed.Encode(), Base64FormattingOptions.InsertLineBreaks) + "\r\n";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               Pkcs7Message_("Signed, labelled enveloped", "signed-labelled@example.test", signedBody));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string received = partner.MessageData;
            StringAssert.Contains("X-hMailServer-Gateway-Encrypted: S/MIME to key " + Sha256Hex_(gateway.RawData), HeaderOf_(received),
               "Signed data labelled enveloped-data went to the partner as it was.");

            string inner = OpenEnvelope_(received, gateway, out string _, out string _);
            StringAssert.Contains("smime-type=enveloped-data", inner, "The labelled message is not what is inside.");
         }
      }

      [Test]
      [Description("A clear-signed S/MIME message is enveloped for the partner, signature and all: signed is not encrypted, and the signature stays inside.")]
      public void ASignedClearMessageIsEnvelopedWithTheSignatureInside()
      {
         X509Certificate2 certificate = GatewayCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);

         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(certificate), FallbackRefuse);

         string message =
            "From: " + _account.Address + "\r\n" +
            "To: test@" + PartnerDomain + "\r\n" +
            "Subject: Signed in the clear\r\n" +
            "Message-ID: <signed-clear@example.test>\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256;\r\n boundary=\"signed-boundary\"\r\n" +
            "\r\n" +
            "--signed-boundary\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "The signed text, which the signature covers.\r\n" +
            "--signed-boundary\r\n" +
            "Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\n" +
            "Content-Transfer-Encoding: base64\r\n" +
            "\r\n" +
            "MIIBSignatureBytesStandInHere\r\n" +
            "--signed-boundary--\r\n";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            AddRoute_(PartnerDomain, PartnerPort, 1);

            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain, message);

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string received = partner.MessageData;
            StringAssert.Contains("smime-type=enveloped-data", HeaderOf_(received), "A signed message whose content is in the clear was not encrypted.");
            StringAssert.DoesNotContain("The signed text, which the signature covers.", received);

            string inner = OpenEnvelope_(received, certificate, out string _, out string _);
            StringAssert.Contains("multipart/signed", inner, "The signed message is not what is inside.");
            StringAssert.Contains("The signed text, which the signature covers.\r\n--signed-boundary\r\n", inner, "The signed part is not byte for byte inside.");
            StringAssert.Contains("MIIBSignatureBytesStandInHere", inner, "The signature is not inside.");
         }
      }

      // ------------------------------------------------------------------ DKIM

      [Test]
      [Description("The partner's copy carries one DKIM-Signature, made afresh, that verifies over its new body with the domain's key; the original's signature is inside and verifies over the original; one byte changed in the body and the outer signature no longer verifies.")]
      public void TheEncryptedCopyCarriesADkimSignatureThatVerifiesOverItsNewBody()
      {
         string sslExamples = Paths.Combine(TestContext.CurrentContext.TestDirectory, "..\\..\\..\\..\\SSL examples");
         string privateKeyFile = Paths.Combine(sslExamples, "example.key");
         string certificateFile = Paths.Combine(sslExamples, "example.crt");
         Assert.IsTrue(File.Exists(privateKeyFile) && File.Exists(certificateFile), "The suite's example key pair is missing.");

         _domain.DKIMPrivateKeyFile = privateKeyFile;
         _domain.DKIMSelector = "gateway";
         _domain.DKIMSignEnabled = true;
         _domain.Save();

         X509Certificate2 certificate = GatewayCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);
         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(certificate), FallbackRefuse);

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         using (var signingCertificate = new X509Certificate2(certificateFile))
         using (RSA publicKey = signingCertificate.GetRSAPublicKey())
         {
            AddRoute_(PartnerDomain, PartnerPort, 1);

            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "Signed again", "signed-again@example.test", "A body DKIM signs twice: once as written, once as encrypted."));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string received = partner.MessageData;
            string header = HeaderOf_(received);

            Assert.AreEqual(1, Regex.Matches(header, "^DKIM-Signature:", RegexOptions.Multiline | RegexOptions.IgnoreCase).Count,
               "The partner's copy must carry exactly one DKIM-Signature: the new one, the original's taken off.");

            Assert.IsTrue(DkimSignatureVerifies_(received, publicKey, out string why), "The partner's copy's DKIM signature does not verify: " + why);

            // Inside, the message as it was written, with the signature the original was given.
            string inner = OpenEnvelope_(received, certificate, out string _, out string _);
            StringAssert.Contains("DKIM-Signature:", inner, "The original's DKIM signature is not inside.");
            Assert.IsTrue(DkimSignatureVerifies_(inner, publicKey, out string innerWhy), "The original's DKIM signature does not verify inside: " + innerWhy);

            // The negative control: the verifier is checking something.
            int bodyAt = received.IndexOf("\r\n\r\n", StringComparison.Ordinal) + 4;
            char changed = received[bodyAt] == 'M' ? 'N' : 'M';
            string tampered = received.Substring(0, bodyAt) + changed + received.Substring(bodyAt + 1);
            Assert.IsFalse(DkimSignatureVerifies_(tampered, publicKey, out string _), "A changed body still verified: the check checks nothing.");
         }
      }

      // ------------------------------------------------------------ size ceiling

      [Test]
      [Description("The policy's largest message is measured on the partner's encrypted copy, which is what is sent: a message under the ceiling as written goes while nothing encrypts it, and the same message, a third larger once enveloped, is refused 5.3.4 without a connection once the policy encrypts.")]
      public void TheSizeCeilingMeasuresTheEncryptedCopy()
      {
         X509Certificate2 certificate = GatewayCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);

         // About 30 KB of text: under a 36 KB ceiling as written, and over it as
         // the base64 of a CMS object, which is about 1.37 times as long.
         string body = string.Join("\r\n", Enumerable.Repeat(new string('x', 98), 300));

         RemoteDomainPolicy policy = AddPolicy_(PartnerDomain, EncryptNone, "", FallbackRefuse);
         policy.MaxMessageSizeKB = 36;
         policy.Save();
         AddRoute_(PartnerDomain, PartnerPort, 1);

         // The negative control: nothing encrypts it, and it goes.
         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "Under the ceiling", "under-the-ceiling@example.test", body));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);
            StringAssert.Contains(new string('x', 98), partner.MessageData, "The message under the ceiling was not delivered.");
         }

         dynamic late = policy;
         late.Encrypt = EncryptSmime;
         late.EncryptKey = Pem_(certificate);
         policy.Save();

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "Over the ceiling once enveloped", "over-once-enveloped@example.test", body));

            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string bounce = Decoded_(Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, "test"));
            StringAssert.Contains("5.3.4", bounce, "The encrypted copy over the ceiling was not refused 5.3.4.");
            StringAssert.Contains("allows at most 36 KB", bounce, "The notification does not name the ceiling.");
            Assert.IsEmpty(partner.Conversation, "The server connected to the partner with a copy over the policy's ceiling.");
         }
      }

      // -------------------------------------------------------------- fallback

      [Test]
      [Description("A key that has expired since it was saved, under the refusing fallback: nothing is sent, the recipient fails permanently with 5.7.10, the sender is told in a notification naming the policy and the reason, and nothing is left in the server's temporary directory.")]
      public void AKeyExpiredSinceItWasSavedIsRefused5710AndTheSenderIsTold()
      {
         X509Certificate2 certificate = ShortLivedCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain);

         // Saved while it is good - the save judges it - and then let run out.
         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(certificate), FallbackRefuse);
         WaitUntilExpired_(certificate);

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            AddRoute_(PartnerDomain, PartnerPort, 3);
            string[] tempBefore = TempFiles_();

            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "Not in the clear", "refused-at-the-gateway@example.test", "This must not reach the partner unencrypted."));

            // Three tries on the route, and refused at once: a retry would find the same key.
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string bounce = Decoded_(Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, "test"));

            StringAssert.Contains("5.7.10", bounce, "The refusal does not carry 5.7.10.");
            StringAssert.Contains("remote domain policy for " + PartnerDomain, bounce, "The notification does not name the policy.");
            StringAssert.Contains("could not be encrypted", bounce, "The notification does not say what went wrong.");
            StringAssert.Contains("the certificate expired on", bounce, "The notification does not give the reason.");
            StringAssert.Contains("NOT been sent unencrypted", bounce, "The notification does not say the message was withheld.");
            StringAssert.DoesNotContain("Tried", bounce, "The refusal went through the retry path; it is permanent at the first attempt.");

            Assert.IsEmpty(partner.Conversation, "The server connected to the partner for a message it could not encrypt.");

            AssertNoNewTempFiles_(tempBefore);
         }
      }

      [Test]
      [Description("After the key has expired, the policy can still be saved - the fallback switched to send_unencrypted over REST - because a save that does not change the key does not judge it again; the listing says what is wrong with the key. The message is then delivered as it was written, and the application log and the message trace say it went unencrypted.")]
      public void AKeyExpiredSinceItWasSavedIsSentPlainUnderTheSendingFallback()
      {
         ServerIniFile.SetSetting("MessageTraceEnabled", "1");
         _application.Reinitialize();

         X509Certificate2 certificate = ShortLivedCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain);

         RemoteDomainPolicy policy = AddPolicy_(PartnerDomain, EncryptSmime, Pem_(certificate), FallbackRefuse);
         WaitUntilExpired_(certificate);

         // The day the fallback exists for: the key has run out, and the
         // administrator lets the partner's mail through while a new one comes.
         (int status, string body) switched = Admin_("PUT", "/api/v1/remote-domains/" + policy.ID,
            "{\"domain_name\":\"" + PartnerDomain + "\",\"encrypt\":\"smime\",\"encrypt_fallback\":\"send_unencrypted\"}");
         Assert.AreEqual(200, switched.status, "A save that did not change the expired key was refused for it: " + switched.body);
         StringAssert.Contains("\"encrypt_fallback\":\"send_unencrypted\"", switched.body);
         StringAssert.Contains("the certificate expired on", switched.body, "The listing does not say what is wrong with the key held.");

         const string body = "Sent as written, because the policy says a late message is worse than a plain one.";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            AddRoute_(PartnerDomain, PartnerPort, 1);

            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "Plain by the fallback", "expired-plain@example.test", body));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            StringAssert.Contains(body, partner.MessageData, "The fallback did not deliver the message as it was written.");
            StringAssert.DoesNotContain("X-hMailServer-Gateway-Encrypted", partner.MessageData);

            Assert.IsTrue(LogHandler.DefaultLogContains("It is sent UNENCRYPTED, as the policy's fallback allows."),
               "The application log does not say the message went unencrypted.");

            // The trace was emptied when this test began, so a row here is this run's.
            var trace = _application.GlobalObjects.MessageTrace;
            trace.Search("test@" + PartnerDomain);
            bool recorded = false;
            for (int i = 0; i < trace.Count; i++)
            {
               if (trace[i].EventName == "unencrypted" && trace[i].Detail.Contains(PartnerDomain) && trace[i].Detail.Contains("expired"))
                  recorded = true;
            }
            Assert.IsTrue(recorded, "The message trace has no unencrypted event naming the policy and the reason.");
         }
      }

      // ------------------------------------------------------------ validation

      [Test]
      [Description("The partner's key is judged when it is saved: private-key material is refused whatever the method and wherever it is in the paste, and nothing is stored; a key with nothing to encrypt to, an expired certificate and a method with no key are refused by name; a certificate for another domain is taken with a warning; the listing answers the fingerprints, the expiry and the subject and never the key, and neither does the audit trail; a PUT without the key or the method keeps them, and a key of white space removes it.")]
      public void ThePartnersKeyIsJudgedWhenItIsSaved()
      {
         const string domain = "validation-rest.test";

         // The audit trail from here on, so that its rows are this test's.
         _application.Database.ExecuteSQL("delete from hm_audit");

         // ---- private-key material, whatever the method, and nothing stored
         (int status, string body) secret = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"openpgp\",\"encrypt_key\":" + JsonText_(OpenPgpTestSupport.RingOwnSecretArmoured) + "}");
         Assert.AreEqual(400, secret.status, secret.body);
         StringAssert.Contains("SECRET", secret.body, "A private key was not refused by name.");

         (int status, string body) underNone = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt_key\":" + JsonText_(PrivateKeyPem) + "}");
         Assert.AreEqual(400, underNone.status, "A private key was stored under a policy that encrypts nothing: " + underNone.body);
         StringAssert.Contains("PRIVATE", underNone.body);

         (int status, string body) publicThenPrivate = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"openpgp\",\"encrypt_key\":" +
            JsonText_(OpenPgpTestSupport.Armoured(OpenPgpTestSupport.RingOwnPublicBase64) + PrivateKeyPem) + "}");
         Assert.AreEqual(400, publicThenPrivate.status, "A PEM private key after a good OpenPGP public key was stored: " + publicThenPrivate.body);
         StringAssert.Contains("PRIVATE", publicThenPrivate.body);

         X509Certificate2 good = GatewayCertificate_("CN=gateway." + domain, "gateway@" + domain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);
         (int status, string body) certificateThenSecret = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"smime\",\"encrypt_key\":" + JsonText_(Pem_(good) + OpenPgpTestSupport.RingOwnSecretArmoured) + "}");
         Assert.AreEqual(400, certificateThenSecret.status, "An OpenPGP secret key after a good certificate was stored: " + certificateThenSecret.body);
         StringAssert.Contains("SECRET", certificateThenSecret.body);

         Assert.IsNull(PolicyOverCom_(domain), "A refused private key created a policy.");

         // ---- what is not a key a message can be encrypted to
         (int status, string body) noSubkey = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"openpgp\",\"encrypt_key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");
         Assert.AreEqual(400, noSubkey.status, noSubkey.body);
         StringAssert.Contains("no subkey that may encrypt", noSubkey.body, "A key with no encryption subkey was not refused by name.");

         X509Certificate2 expired = GatewayCertificate_("CN=gateway." + domain, "gateway@" + domain,
            DateTimeOffset.UtcNow.AddDays(-30), DateTimeOffset.UtcNow.AddDays(-1), X509KeyUsageFlags.KeyEncipherment);
         (int status, string body) old = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"smime\",\"encrypt_key\":" + JsonText_(Pem_(expired)) + "}");
         Assert.AreEqual(400, old.status, old.body);
         StringAssert.Contains("the certificate expired on", old.body, "An expired certificate was not refused by name.");

         (int status, string body) none = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"smime\"}");
         Assert.AreEqual(400, none.status, "A policy that encrypts with no key to encrypt to was saved. Body: " + none.body);
         StringAssert.Contains("no key is held", none.body, "The refusal of a method with no key does not say so.");

         Assert.IsNull(PolicyOverCom_(domain), "A refused key created a policy.");

         // ---- an OpenPGP key: both fingerprints answered
         (int status, string body) openPgp = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"openpgp-" + domain + "\",\"encrypt\":\"openpgp\",\"encrypt_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\"}");
         Assert.AreEqual(201, openPgp.status, openPgp.body);
         StringAssert.Contains("\"encrypt_key_fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", openPgp.body);
         StringAssert.Contains("\"encrypt_key_subkey_fingerprint\":\"" + AliceSubkeyFingerprint + "\"", openPgp.body,
            "The subkey messages are encrypted to is not answered beside the primary.");

         // ---- a gateway's certificate that names the gateway's own domain: taken, and said
         X509Certificate2 elsewhere = GatewayCertificate_("CN=gateway.elsewhere.test", "gateway@elsewhere.test",
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);
         string pem = Pem_(elsewhere);
         string fingerprint = Sha256Hex_(elsewhere.RawData);
         string keyFragment = Convert.ToBase64String(elsewhere.RawData).Substring(100, 40);
         string expires = elsewhere.NotAfter.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);

         (int status, string body) created = Admin_("POST", "/api/v1/remote-domains",
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"smime\",\"encrypt_fallback\":\"send_unencrypted\",\"encrypt_key\":" + JsonText_("\r\n" + pem + "\r\n") + "}");
         Assert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("\"encrypt\":\"smime\"", created.body);
         StringAssert.Contains("\"encrypt_fallback\":\"send_unencrypted\"", created.body);
         StringAssert.Contains("\"encrypt_key_set\":true", created.body);
         StringAssert.Contains("\"encrypt_key_fingerprint\":\"" + fingerprint + "\"", created.body);
         StringAssert.Contains("\"encrypt_key_subkey_fingerprint\":\"\"", created.body, "A certificate was answered with a subkey.");
         StringAssert.Contains("\"encrypt_key_expires\":\"" + expires + "\"", created.body, "The expiry is not the certificate's notAfter in UTC.");
         StringAssert.Contains("\"encrypt_key_subject\":\"/CN=gateway.elsewhere.test\"", created.body, "The subject is not the certificate's.");
         StringAssert.Contains("\"encrypt_key_problem\":\"\"", created.body);
         StringAssert.Contains("names no address or host in " + domain, created.body, "A certificate for another domain was taken without the warning.");
         StringAssert.DoesNotContain(keyFragment, created.body, "The key was answered back.");
         StringAssert.DoesNotContain("BEGIN CERTIFICATE", created.body, "The key was answered back.");
         int id = IdOf_(created.body);

         (int status, string body) listed = Admin_("GET", "/api/v1/remote-domains", null);
         Assert.AreEqual(200, listed.status);
         StringAssert.Contains("\"encrypt_key_fingerprint\":\"" + fingerprint + "\"", listed.body);
         StringAssert.DoesNotContain(keyFragment, listed.body, "The listing answered the key.");

         // The audit trail records that the key column changed, and never its text.
         (int status, string body) audit = Admin_("GET", "/api/v1/audit?limit=500", null);
         Assert.AreEqual(200, audit.status, audit.body);
         StringAssert.Contains("policyencryptkey=(changed)", audit.body, "The audit trail does not record the key column as withheld.");
         StringAssert.DoesNotContain("BEGIN CERTIFICATE", audit.body, "The audit trail recorded the key's text.");
         StringAssert.DoesNotContain(keyFragment, audit.body, "The audit trail recorded the key's text.");

         // The Deck's form never has the key to send back: a PUT without it keeps it -
         // and a PUT without encrypt keeps the method, so that a client that
         // predates gateway encryption cannot switch it off by not naming it.
         (int status, string body) kept = Admin_("PUT", "/api/v1/remote-domains/" + id,
            "{\"domain_name\":\"" + domain + "\"}");
         Assert.AreEqual(200, kept.status, kept.body);
         StringAssert.Contains("\"encrypt\":\"smime\"", kept.body, "A PUT that did not name encrypt switched encryption off.");
         StringAssert.Contains("\"encrypt_key_fingerprint\":\"" + fingerprint + "\"", kept.body, "A PUT without the key lost the key held.");
         StringAssert.Contains("\"encrypt_fallback\":\"refuse\"", kept.body, "A PUT replaces every other field, the fallback among them.");

         // White space is no key: it removes the key held, as an empty string does.
         (int status, string body) removed = Admin_("PUT", "/api/v1/remote-domains/" + id,
            "{\"domain_name\":\"" + domain + "\",\"encrypt\":\"none\",\"encrypt_key\":\" \\r\\n \"}");
         Assert.AreEqual(200, removed.status, removed.body);
         StringAssert.Contains("\"encrypt_key_set\":false", removed.body, "A key of white space did not remove the key held.");

         (int status, string body) unknown = Admin_("POST", "/api/v1/remote-domains", "{\"domain_name\":\"x-" + domain + "\",\"encrypt\":\"sometimes\"}");
         Assert.AreEqual(400, unknown.status, unknown.body);
         StringAssert.Contains("encrypt", unknown.body);
      }

      [Test]
      [Description("Over COM the key is written and never read back: Encrypt, EncryptFallback and the key's fingerprint, expiry and summary round-trip through the database; EncryptKey answers empty; a private key is refused whatever the method; white space removes the key; an unknown fallback is refused.")]
      public void TheComSurfaceWritesTheKeyAndNeverReadsItBack()
      {
         X509Certificate2 certificate = GatewayCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);
         string fingerprint = Sha256Hex_(certificate.RawData);

         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(certificate), FallbackSendUnencrypted);

         // Read back through the database, not from the object just written:
         // the collection read again hands out policies made from the rows.
         _settings.RemoteDomainPolicies.Refresh();
         dynamic policy = _settings.RemoteDomainPolicies.get_ItemByName(PartnerDomain);
         int encrypt = (int) policy.Encrypt;
         int fallback = (int) policy.EncryptFallback;
         string key = (string) policy.EncryptKey;
         string held = (string) policy.EncryptKeyFingerprint;
         string expires = (string) policy.EncryptKeyExpires;
         string summary = (string) policy.EncryptKeySummary;

         Assert.AreEqual(EncryptSmime, encrypt);
         Assert.AreEqual(FallbackSendUnencrypted, fallback);
         Assert.AreEqual("", key, "EncryptKey answered the key back.");
         Assert.AreEqual(fingerprint, held);
         StringAssert.IsMatch("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$", expires, "The expiry is not ISO 8601 in UTC.");
         StringAssert.Contains("S/MIME key " + fingerprint, summary);
         StringAssert.DoesNotContain("No message can be encrypted", summary, "A usable key was described as unusable.");

         dynamic refused = _settings.RemoteDomainPolicies.Add();
         refused.DomainName = "refused-com.test";
         refused.Encrypt = EncryptSmime;
         refused.EncryptKey = PrivateKeyPem;
         COMException saved = Assert.Throws<COMException>(() => refused.Save());
         StringAssert.Contains("PRIVATE", saved.Message, "A private key was not refused by name.");

         dynamic refusedUnderNone = _settings.RemoteDomainPolicies.Add();
         refusedUnderNone.DomainName = "refused-com.test";
         refusedUnderNone.Encrypt = EncryptNone;
         refusedUnderNone.EncryptKey = Pem_(certificate) + PrivateKeyPem;
         COMException savedUnderNone = Assert.Throws<COMException>(() => refusedUnderNone.Save());
         StringAssert.Contains("PRIVATE", savedUnderNone.Message, "A private key beside a certificate was stored under a policy that encrypts nothing.");
         Assert.IsNull(PolicyOverCom_("refused-com.test"), "A refused private key created a policy.");

         Assert.Throws<COMException>(() => policy.EncryptFallback = 5, "An unknown fallback was stored.");
         Assert.Throws<COMException>(() => policy.Encrypt = 7, "An unknown method was stored.");

         // White space is no key, over COM as over REST.
         policy.Encrypt = EncryptNone;
         policy.EncryptKey = " \r\n\t";
         policy.Save();
         _settings.RemoteDomainPolicies.Refresh();
         string cleared = (string) ((dynamic) _settings.RemoteDomainPolicies.get_ItemByName(PartnerDomain)).EncryptKeySummary;
         Assert.AreEqual("No key is held.", cleared, "A key of white space did not remove the key held.");
      }

      [Test]
      [Description("A save refused over COM - the Control Panel's path - leaves the running policy as the database holds it: the object COM hands out writes a copy of the running record, the refused key must not govern delivery, and the object reads the policy in effect after the refusal. The partner still gets the message enveloped to the key saved before.")]
      public void ARefusedComSaveLeavesTheRunningPolicyAsItWas()
      {
         X509Certificate2 good = GatewayCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1), X509KeyUsageFlags.KeyEncipherment);
         string goodFingerprint = Sha256Hex_(good.RawData);
         X509Certificate2 expired = GatewayCertificate_("CN=old-gateway." + PartnerDomain, "gateway@" + PartnerDomain,
            DateTimeOffset.UtcNow.AddDays(-30), DateTimeOffset.UtcNow.AddDays(-1), X509KeyUsageFlags.KeyEncipherment);

         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(good), FallbackRefuse);

         // The running record, as the Control Panel edits it: the first put_* copies it.
         dynamic attached = _settings.RemoteDomainPolicies.get_ItemByName(PartnerDomain);

         attached.EncryptKey = Pem_(expired);
         COMException expiredRefused = Assert.Throws<COMException>(() => attached.Save());
         StringAssert.Contains("the certificate expired on", expiredRefused.Message);
         Assert.AreEqual(goodFingerprint, (string) attached.EncryptKeyFingerprint, "The object kept the refused key after its save was refused.");

         attached.Encrypt = EncryptOpenPgp;
         Assert.Throws<COMException>(() => attached.Save(), "A certificate was saved as an OpenPGP key.");
         Assert.AreEqual(EncryptSmime, (int) attached.Encrypt, "The object kept the refused method after its save was refused.");

         const string body = "Enveloped to the key that was saved, not to the one that was refused.";

         using (SmtpServerSimulator partner = Simulator_(PartnerPort, "test@" + PartnerDomain))
         {
            AddRoute_(PartnerDomain, PartnerPort, 1);

            new SmtpClientSimulator().SendRaw(_account.Address, "test@" + PartnerDomain,
               PlainMessage_("test@" + PartnerDomain, "After a refused save", "after-refused-save@example.test", body));

            partner.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string received = partner.MessageData;
            StringAssert.Contains("X-hMailServer-Gateway-Encrypted: S/MIME to key " + goodFingerprint + "\r\n", HeaderOf_(received),
               "The message was not enveloped to the key the policy held before the refused save.");
            StringAssert.Contains(body, OpenEnvelope_(received, good, out string _, out string _));
         }
      }

      [Test]
      [Description("The configuration backup carries the partner's key: a policy deleted and restored from the backup comes back encrypting, to the same key - one that has expired since the backup included, since a restore takes the key as the backup held it.")]
      public void TheBackupCarriesTheKey()
      {
         X509Certificate2 certificate = ShortLivedCertificate_("CN=gateway." + PartnerDomain, "gateway@" + PartnerDomain);
         string fingerprint = Sha256Hex_(certificate.RawData);

         AddPolicy_(PartnerDomain, EncryptSmime, Pem_(certificate), FallbackSendUnencrypted);

         string backupDirectory = Paths.Combine(Path.GetTempPath(), TestSetup.UniqueString());
         Directory.CreateDirectory(backupDirectory);

         var backupSettings = _application.Settings.Backup;
         bool backupDomains = backupSettings.BackupDomains;
         bool backupMessages = backupSettings.BackupMessages;
         bool backupSettingsToo = backupSettings.BackupSettings;
         bool compress = backupSettings.CompressDestinationFiles;
         string destination = backupSettings.Destination;

         try
         {
            CustomAsserts.AssertDeleteFile(backupSettings.LogFile);
            backupSettings.BackupDomains = false;
            backupSettings.BackupMessages = false;
            backupSettings.BackupSettings = true;
            backupSettings.CompressDestinationFiles = true;
            backupSettings.Destination = backupDirectory;

            _application.BackupManager.StartBackup();
            Assert.IsTrue(WaitForBackup_(backupSettings.LogFile), "The backup did not complete.");

            // The policy goes, as a disaster would take it - and by the time of
            // the restore, the key in the backup has expired.
            ClearPolicies_();
            Assert.IsNull(PolicyOverCom_(PartnerDomain));
            WaitUntilExpired_(certificate);

            string[] archives = Directory.GetFiles(backupDirectory, "*.7z");
            Assert.AreEqual(1, archives.Length, "The backup wrote no archive.");

            string startTime = _application.Status.StartTime;
            var backup = _application.BackupManager.LoadBackup(archives[0]);
            backup.RestoreDomains = false;
            backup.RestoreMessages = false;
            backup.RestoreSettings = true;
            backup.StartRestore();
            WaitForRestart_(startTime);

            dynamic restored = _application.Settings.RemoteDomainPolicies.get_ItemByName(PartnerDomain);
            int encrypt = (int) restored.Encrypt;
            int fallback = (int) restored.EncryptFallback;
            string held = (string) restored.EncryptKeyFingerprint;
            string summary = (string) restored.EncryptKeySummary;

            Assert.AreEqual(EncryptSmime, encrypt, "The restored policy does not encrypt.");
            Assert.AreEqual(FallbackSendUnencrypted, fallback, "The restored policy lost its fallback.");
            Assert.AreEqual(fingerprint, held, "The restored policy does not hold the key the backup was taken with.");
            StringAssert.Contains("No message can be encrypted to it", summary, "The restored key, expired since, is not described as unusable.");
            StringAssert.Contains("expired", summary);
         }
         finally
         {
            var settingsNow = _application.Settings.Backup;
            settingsNow.BackupDomains = backupDomains;
            settingsNow.BackupMessages = backupMessages;
            settingsNow.BackupSettings = backupSettingsToo;
            settingsNow.CompressDestinationFiles = compress;
            settingsNow.Destination = destination;

            try
            {
               Directory.Delete(backupDirectory, true);
            }
            catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
            {
               // A temp directory of this test's own; failing on it would report the wrong thing.
            }
         }
      }

      // ------------------------------------------------------------- helpers

      private void ClearPolicies_()
      {
         hMailServer.RemoteDomainPolicies policies = _settings.RemoteDomainPolicies;
         for (int i = policies.Count - 1; i >= 0; i--)
            policies[i].Delete();
      }

      private void EmptyTheTrace_()
      {
         _application.Database.ExecuteSQL("delete from hm_messagetrace");
      }

      private RemoteDomainPolicy PolicyOverCom_(string domainName)
      {
         try
         {
            return _settings.RemoteDomainPolicies.get_ItemByName(domainName);
         }
         catch (COMException)
         {
            return null;
         }
      }

      private RemoteDomainPolicy AddPolicy_(string domainName, int method, string key, int fallback)
      {
         RemoteDomainPolicy policy = _settings.RemoteDomainPolicies.Add();
         policy.DomainName = domainName;
         policy.Active = true;

         dynamic late = policy;
         late.Encrypt = method;
         late.EncryptKey = key;
         late.EncryptFallback = fallback;

         policy.Save();
         return policy;
      }

      private void AddRoute_(string domainName, int port, int tries)
      {
         Route route = _settings.Routes.Add();
         route.DomainName = domainName;
         route.TargetSMTPHost = "127.0.0.1";
         route.TargetSMTPPort = port;
         route.NumberOfTries = tries;
         route.MinutesBetweenTry = 5;
         route.TreatRecipientAsLocalDomain = false;
         route.TreatSenderAsLocalDomain = false;
         route.ConnectionSecurity = eConnectionSecurity.eCSNone;
         route.Save();
      }

      private static SmtpServerSimulator Simulator_(int port, string recipient)
      {
         var server = new SmtpServerSimulator(1, port, eConnectionSecurity.eCSNone);
         server.AddRecipientResult(new Dictionary<string, int> { [recipient] = 250 });
         server.StartListen();
         return server;
      }

      /// <summary>
      ///    The recipients of each transaction the simulator saw, in order: a MAIL
      ///    starts one, and every RCPT after it until the next MAIL is its.
      /// </summary>
      private static List<List<string>> Transactions_(List<string> commands)
      {
         var transactions = new List<List<string>>();
         foreach (string command in commands)
         {
            if (command.StartsWith("MAIL", StringComparison.OrdinalIgnoreCase))
               transactions.Add(new List<string>());
            else if (command.StartsWith("RCPT", StringComparison.OrdinalIgnoreCase) && transactions.Count > 0)
               transactions[transactions.Count - 1].Add(AddressOf_(command));
         }
         return transactions;
      }

      /// <summary>The recipient of each transaction that reached DATA, in order.</summary>
      private static List<string> AcceptedTransactionRecipients_(List<string> commands)
      {
         var accepted = new List<string>();
         string last = null;
         foreach (string command in commands)
         {
            if (command.StartsWith("MAIL", StringComparison.OrdinalIgnoreCase))
               last = null;
            else if (command.StartsWith("RCPT", StringComparison.OrdinalIgnoreCase))
               last = AddressOf_(command);
            else if (command == "DATA" && last != null)
               accepted.Add(last);
         }
         return accepted;
      }

      private static string AddressOf_(string command)
      {
         int start = command.IndexOf('<') + 1;
         int end = command.LastIndexOf('>');
         return end > start ? command.Substring(start, end - start) : command;
      }

      private string PlainMessage_(string to, string subject, string messageId, string body)
      {
         return "From: " + _account.Address + "\r\n" +
                "To: " + to + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "Message-ID: <" + messageId + ">\r\n" +
                "Date: Thu, 24 Sep 2026 10:00:00 +0000\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "Content-Transfer-Encoding: 7bit\r\n" +
                "\r\n" +
                body + "\r\n";
      }

      private static string PgpMimeBody_(string armour)
      {
         return "This is an OpenPGP/MIME encrypted message (RFC 3156).\r\n" +
                "\r\n" +
                "--sealed-by-the-client\r\n" +
                "Content-Type: application/pgp-encrypted\r\n" +
                "\r\n" +
                "Version: 1\r\n" +
                "\r\n" +
                "--sealed-by-the-client\r\n" +
                "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n" +
                "\r\n" +
                armour +
                "\r\n" +
                "--sealed-by-the-client--\r\n";
      }

      private string PgpMimeMessage_(string subject, string messageId, string body)
      {
         return "From: " + _account.Address + "\r\n" +
                "To: test@" + PartnerDomain + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "Message-ID: <" + messageId + ">\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";\r\n boundary=\"sealed-by-the-client\"\r\n" +
                "\r\n" +
                body;
      }

      /// <summary>An application/pkcs7-mime message that SAYS enveloped-data, whatever its body is.</summary>
      private string Pkcs7Message_(string subject, string messageId, string base64Body)
      {
         return "From: " + _account.Address + "\r\n" +
                "To: test@" + PartnerDomain + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "Message-ID: <" + messageId + ">\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=\"smime.p7m\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"smime.p7m\"\r\n" +
                "\r\n" +
                base64Body;
      }

      // ---- the server's temporary directory, where each batch's copy is made

      private string[] TempFiles_()
      {
         return Directory.GetFiles(_settings.Directories.TempDirectory);
      }

      /// <summary>Nothing new in the temporary directory once deliveries have settled: the batch's copy is removed on every way out.</summary>
      private void AssertNoNewTempFiles_(string[] before)
      {
         var known = new HashSet<string>(before, StringComparer.OrdinalIgnoreCase);
         string[] left = new string[0];

         for (int i = 0; i < 50; i++)
         {
            left = TempFiles_().Where(file => !known.Contains(file)).ToArray();
            if (left.Length == 0)
               return;
            Thread.Sleep(100);
         }

         Assert.Fail("The server left file(s) in its temporary directory: " + string.Join(", ", left));
      }

      /// <summary>
      ///    The partner's gateway certificate and its private half, made here. The key
      ///    is made in a CryptoAPI provider and the certificate re-imported from a PFX,
      ///    so that EnvelopedCms on .NET Framework finds a key it can decrypt with;
      ///    PersistKeySet is not passed, so the container goes with the certificate.
      /// </summary>
      private X509Certificate2 GatewayCertificate_(string subject, string email, DateTimeOffset notBefore, DateTimeOffset notAfter, X509KeyUsageFlags usage)
      {
         return MakeCertificate_(subject, email, notBefore, () => notAfter, usage);
      }

      /// <summary>
      ///    A certificate good for the next few seconds and no longer: long enough for
      ///    the save to judge it good, short enough to have expired by the delivery.
      ///    The end is fixed once the key exists, because making an RSA key takes a
      ///    time of its own on a slow bench.
      /// </summary>
      private X509Certificate2 ShortLivedCertificate_(string subject, string email)
      {
         return MakeCertificate_(subject, email, DateTimeOffset.UtcNow.AddDays(-1), () => DateTimeOffset.UtcNow.AddSeconds(10),
            X509KeyUsageFlags.KeyEncipherment);
      }

      private static void WaitUntilExpired_(X509Certificate2 certificate)
      {
         TimeSpan left = certificate.NotAfter.ToUniversalTime() - DateTime.UtcNow + TimeSpan.FromSeconds(2);
         if (left > TimeSpan.Zero)
            Thread.Sleep(left);
      }

      private X509Certificate2 MakeCertificate_(string subject, string email, DateTimeOffset notBefore, Func<DateTimeOffset> notAfter, X509KeyUsageFlags usage)
      {
         using (var key = new RSACryptoServiceProvider(2048, new CspParameters(24)))
         {
            key.PersistKeyInCsp = false;

            var request = new CertificateRequest(subject, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
            request.CertificateExtensions.Add(new X509KeyUsageExtension(usage, true));
            request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
               new OidCollection { new Oid("1.3.6.1.5.5.7.3.4") /* id-kp-emailProtection */ }, false));

            if (email != null)
            {
               var names = new SubjectAlternativeNameBuilder();
               names.AddEmailAddress(email);
               request.CertificateExtensions.Add(names.Build());
            }

            using (X509Certificate2 made = request.CreateSelfSigned(notBefore, notAfter()))
            {
               byte[] pfx = made.Export(X509ContentType.Pfx, "gateway");
               var usable = new X509Certificate2(pfx, "gateway", X509KeyStorageFlags.Exportable | X509KeyStorageFlags.UserKeySet);
               _certificates.Add(usable);
               return usable;
            }
         }
      }

      private static string Pem_(X509Certificate2 certificate)
      {
         return "-----BEGIN CERTIFICATE-----\r\n" +
                Convert.ToBase64String(certificate.RawData, Base64FormattingOptions.InsertLineBreaks) +
                "\r\n-----END CERTIFICATE-----\r\n";
      }

      private static string Sha256Hex_(byte[] data)
      {
         using (SHA256 sha = SHA256.Create())
         {
            return string.Concat(sha.ComputeHash(data).Select(b => b.ToString("x2")));
         }
      }

      private static string HeaderOf_(string message)
      {
         int end = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         return end < 0 ? message : message.Substring(0, end + 2);
      }

      private static string BodyOf_(string message)
      {
         int end = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         return end < 0 ? "" : message.Substring(end + 4);
      }

      /// <summary>
      ///    The envelope in the body of the partner's copy, opened with the
      ///    certificate's private key: the content algorithm, and the key
      ///    transport the content key was sent by, as their OIDs.
      /// </summary>
      private static string OpenEnvelope_(string message, X509Certificate2 certificate, out string contentAlgorithm, out string keyTransport)
      {
         byte[] der = Convert.FromBase64String(Regex.Replace(BodyOf_(message), "\\s", ""));

         var envelope = new EnvelopedCms();
         envelope.Decode(der);
         contentAlgorithm = envelope.ContentEncryptionAlgorithm.Oid.Value;
         keyTransport = envelope.RecipientInfos.Count > 0 ? envelope.RecipientInfos[0].KeyEncryptionAlgorithm.Oid.Value : "";
         envelope.Decrypt(new X509Certificate2Collection(certificate));

         return Encoding.UTF8.GetString(envelope.ContentInfo.Content);
      }

      // ---- DKIM, verified here rather than trusted (RFC 6376 sections 3.4, 3.7)

      /// <summary>Each header field as its name and its whole text, folded lines and line end included.</summary>
      private static List<KeyValuePair<string, string>> HeaderFields_(string header)
      {
         var fields = new List<KeyValuePair<string, string>>();
         StringBuilder current = null;
         string name = null;

         foreach (string line in header.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.Length == 0)
               continue;

            if ((line[0] == ' ' || line[0] == '\t') && current != null)
            {
               current.Append(line).Append("\r\n");
               continue;
            }

            if (current != null)
               fields.Add(new KeyValuePair<string, string>(name, current.ToString()));

            int colon = line.IndexOf(':');
            name = colon > 0 ? line.Substring(0, colon).Trim() : line;
            current = new StringBuilder(line).Append("\r\n");
         }

         if (current != null)
            fields.Add(new KeyValuePair<string, string>(name, current.ToString()));

         return fields;
      }

      private static string RelaxedHeader_(string field)
      {
         int colon = field.IndexOf(':');
         string name = field.Substring(0, colon).Trim().ToLowerInvariant();
         string value = field.Substring(colon + 1).Replace("\r\n", "");
         value = Regex.Replace(value, "[ \t]+", " ").Trim();
         return name + ":" + value + "\r\n";
      }

      private static string CanonicalBody_(string body, bool relaxed)
      {
         var lines = new List<string>(body.Split(new[] { "\r\n" }, StringSplitOptions.None));
         if (lines.Count > 0 && lines[lines.Count - 1].Length == 0)
            lines.RemoveAt(lines.Count - 1);

         if (relaxed)
            lines = lines.Select(line => Regex.Replace(line, "[ \t]+", " ").TrimEnd(' ')).ToList();

         while (lines.Count > 0 && lines[lines.Count - 1].Length == 0)
            lines.RemoveAt(lines.Count - 1);

         if (lines.Count == 0)
            return relaxed ? "" : "\r\n";

         return string.Join("\r\n", lines) + "\r\n";
      }

      private static Dictionary<string, string> Tags_(string value)
      {
         var tags = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (string part in value.Split(';'))
         {
            int equals = part.IndexOf('=');
            if (equals < 0)
               continue;
            tags[Regex.Replace(part.Substring(0, equals), "\\s", "")] = Regex.Replace(part.Substring(equals + 1), "\\s", "");
         }
         return tags;
      }

      /// <summary>
      ///    Whether the topmost DKIM-Signature of a message verifies with the key: the
      ///    body hash over the body as it is, and the RSA signature over the fields h=
      ///    names, each taken from the bottom up, then the signature field with its b=
      ///    value emptied - both canonicalized as its c= tag says.
      /// </summary>
      private static bool DkimSignatureVerifies_(string message, RSA publicKey, out string why)
      {
         List<KeyValuePair<string, string>> fields = HeaderFields_(HeaderOf_(message));
         int at = fields.FindIndex(field => string.Equals(field.Key, "DKIM-Signature", StringComparison.OrdinalIgnoreCase));
         if (at < 0)
         {
            why = "the message carries no DKIM-Signature";
            return false;
         }

         string signatureField = fields[at].Value;
         Dictionary<string, string> tags = Tags_(signatureField.Substring(signatureField.IndexOf(':') + 1));
         if (!tags.ContainsKey("bh") || !tags.ContainsKey("b") || !tags.ContainsKey("h"))
         {
            why = "the DKIM-Signature lacks bh=, b= or h=: " + signatureField;
            return false;
         }

         string[] methods = (tags.ContainsKey("c") ? tags["c"] : "simple/simple").Split('/');
         bool relaxedHeader = methods[0] == "relaxed";
         bool relaxedBody = methods.Length > 1 && methods[1] == "relaxed";

         string bodyHash;
         using (SHA256 sha = SHA256.Create())
            bodyHash = Convert.ToBase64String(sha.ComputeHash(Encoding.UTF8.GetBytes(CanonicalBody_(BodyOf_(message), relaxedBody))));

         if (bodyHash != tags["bh"])
         {
            why = "bh= is " + tags["bh"] + " and the body hashes to " + bodyHash;
            return false;
         }

         var used = new HashSet<int> { at };
         var signed = new StringBuilder();
         foreach (string wanted in tags["h"].Split(':'))
         {
            for (int i = fields.Count - 1; i >= 0; i--)
            {
               if (used.Contains(i) || !string.Equals(fields[i].Key, wanted, StringComparison.OrdinalIgnoreCase))
                  continue;
               used.Add(i);
               signed.Append(relaxedHeader ? RelaxedHeader_(fields[i].Value) : fields[i].Value);
               break;
            }
         }

         string withoutSignature = Regex.Replace(signatureField, "(^|;)(\\s*b\\s*=)[^;]*", "$1$2");
         string canonical = relaxedHeader ? RelaxedHeader_(withoutSignature) : withoutSignature;
         if (canonical.EndsWith("\r\n", StringComparison.Ordinal))
            canonical = canonical.Substring(0, canonical.Length - 2);
         signed.Append(canonical);

         bool verified = publicKey.VerifyData(Encoding.UTF8.GetBytes(signed.ToString()), Convert.FromBase64String(tags["b"]),
            HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

         why = verified ? "" : "the RSA signature does not verify over:\r\n" + signed;
         return verified;
      }

      // ---- the backup's own waiting, as BackupRestore does it

      private static bool WaitForBackup_(string logFile)
      {
         for (int i = 0; i < 120; i++)
         {
            try
            {
               if (File.Exists(logFile))
               {
                  string contents = TestSetup.ReadExistingTextFile(logFile);
                  if (contents.IndexOf("Backup completed successfully", StringComparison.Ordinal) > 0)
                     return true;
                  if (contents.IndexOf("BACKUP ERROR:", StringComparison.Ordinal) > 0)
                     return false;
               }
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // probably a share access violation.
            }

            Thread.Sleep(250);
         }

         return false;
      }

      private void WaitForRestart_(string lastStartTime)
      {
         for (int i = 0; i < 600; i++)
         {
            try
            {
               string startTime = _application.Status.StartTime;
               if (startTime.Length > 0 && startTime != lastStartTime)
                  return;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // probably a share access violation.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not complete.");
      }

      // ---- OpenPGP, opened by GnuPG where the bench has one

      private static string ArmourOf_(string message)
      {
         string armour = message.Substring(message.IndexOf("-----BEGIN PGP MESSAGE-----", StringComparison.Ordinal));
         return armour.Substring(0, armour.IndexOf("-----END PGP MESSAGE-----", StringComparison.Ordinal) + "-----END PGP MESSAGE-----".Length) + "\n";
      }

      private static string FindGpg_()
      {
         foreach (string candidate in new[] { @"C:\Program Files\Git\usr\bin\gpg.exe", @"C:\Program Files (x86)\GnuPG\bin\gpg.exe", @"C:\Program Files\GnuPG\bin\gpg.exe" })
         {
            if (File.Exists(candidate))
               return candidate;
         }
         return null;
      }

      /// <summary>
      ///    The plaintext GnuPG makes of the PGP/MIME copy, with the fixture's
      ///    secret key. INCONCLUSIVE when there is no gpg, or it cannot import the
      ///    key - the environment, not the server; a FAILURE when it imported the
      ///    key and cannot open what the server sent.
      /// </summary>
      private static string OpenWithGpg_(string received)
      {
         string gpg = FindGpg_();
         if (gpg == null)
            Assert.Inconclusive("No gpg on this machine, so the PGP/MIME copy was not opened: everything else in this test held. build/check-portal-openpgp.js and the Linux CI step are the GnuPG proof of the writer; this is the proof of the gateway's copy.");

         string home = Path.Combine(Path.GetTempPath(), "gw-gpg-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(home);
         try
         {
            string secret = Path.Combine(home, "fixture.sec.asc");
            File.WriteAllText(secret, OpenPgpTestSupport.RingOwnSecretArmoured);
            string message = Path.Combine(home, "message.asc");
            File.WriteAllText(message, ArmourOf_(received));
            string common = "--batch --yes --no-tty --pinentry-mode loopback --passphrase " + OpenPgpTestSupport.RingPassphrase + " --homedir \"" + OpenPgpTestSupport.GpgHome(gpg, home) + "\" ";

            if (!RunGpg_(gpg, common + "--import \"" + secret + "\"", out string _, out string importErr))
               Assert.Inconclusive("gpg is here and could not import the fixture's secret key (" + importErr.Trim().Replace("\r\n", " ") + "), so the PGP/MIME copy was not opened: the environment, not the server.");

            if (!RunGpg_(gpg, common + "--decrypt \"" + message + "\"", out string decrypted, out string decryptErr))
               Assert.Fail("gpg imported the fixture's secret key and could not open the partner's copy: " + decryptErr.Trim().Replace("\r\n", " "));

            return decrypted;
         }
         finally
         {
            // The agent gpg started for this home directory is stopped before
            // the directory goes, or it holds the directory and outlives the test.
            string gpgconf = Path.Combine(Path.GetDirectoryName(gpg), "gpgconf.exe");
            if (File.Exists(gpgconf))
               RunGpg_(gpgconf, "--homedir \"" + OpenPgpTestSupport.GpgHome(gpg, home) + "\" --kill gpg-agent", out string _, out string _);

            try
            {
               Directory.Delete(home, true);
            }
            catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
            {
               TestContext.WriteLine("The gpg home " + home + " could not be removed: " + tidying.Message);
            }
         }
      }

      private static bool RunGpg_(string gpg, string arguments, out string stdout, out string stderr)
      {
         var start = new ProcessStartInfo(gpg, arguments)
         {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8
         };
         using (var process = Process.Start(start))
         {
            stdout = process.StandardOutput.ReadToEnd();
            stderr = process.StandardError.ReadToEnd();
            if (!process.WaitForExit(30000))
            {
               try { process.Kill(); } catch (Exception) { }
               return false;
            }
            return process.ExitCode == 0;
         }
      }

      // ---- REST

      private static string JsonText_(string text)
      {
         return "\"" + text.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n") + "\"";
      }

      private static int IdOf_(string json)
      {
         Match match = Regex.Match(json, "\"id\"\\s*:\\s*(\\d+)");
         Assert.IsTrue(match.Success, "No id in: " + json);
         return int.Parse(match.Groups[1].Value);
      }

      private static string Decoded_(string raw)
      {
         string joined = raw.Replace("=\r\n", "").Replace("=\n", "");
         return Regex.Replace(joined, "=([0-9A-F]{2})", match => ((char) Convert.ToInt32(match.Groups[1].Value, 16)).ToString());
      }

      private static (int status, string body) Admin_(string method, string path, string requestBody)
      {
         string credentials = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, credentials, requestBody);
      }
   }
}
