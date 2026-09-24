// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The organisation CA for S/MIME (Common/Util/OrganisationCa.h,
   ///    docs/SmimeAuthority.md): a domain's own authority, created over REST, that
   ///    signs the PKCS#10 requests its accounts make for keys they keep, publishes
   ///    the root and a signed CRL on the web services listener, answers a
   ///    colleague's certificate by address, and is trusted by the webmail's chain
   ///    check from its own record.
   ///
   ///    The requests are made here with .NET's CertificateRequest - the webmail's
   ///    own request is held to OpenSSL by build/check-portal-smime.js - and every
   ///    certificate the server answers is read back with X509Certificate2 and
   ///    chained with X509Chain against the root the listener serves, not against
   ///    anything the response that issued it said. .NET Framework has no CRL
   ///    reader, so the CRL is read by a small DER walk below, and its signature is
   ///    checked with the root's own key.
   ///
   ///    Both listeners are started here: the REST listener on 9946 and the web
   ///    services listener on 9947, which is where the root and the CRL are served,
   ///    over plain HTTP and without a credential. A few cases need a row changed
   ///    under the server - a key made unopenable, a certificate expired or moved
   ///    back in time - and reach the SQL CE test database for it; on another
   ///    database they are Inconclusive rather than silently passed.
   /// </summary>
   [TestFixture]
   public class RestApiSmimeAuthority : TestFixtureBase
   {
      private const int WebServicesPort = 9947;
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9946;
      private const string Password = "Authority-Passw0rd!";
      private const string AdminPassword = "testar";
      private const string EmailProtection = "1.3.6.1.5.5.7.3.4";

      private Account _ada;
      private Account _bob;

      private string AdaAddress
      {
         get { return "ada@" + _domain.Name; }
      }

      private string BobAddress
      {
         get { return "bob@" + _domain.Name; }
      }

      private string AuthorityPath
      {
         get { return "/api/v1/domains/" + _domain.Name + "/smime-ca"; }
      }

      [SetUp]
      public void StartListeners()
      {
         _ada = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, AdaAddress, Password);
         _bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, BobAddress, Password);
         _settings.SetAdministratorPassword(AdminPassword);

         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebServicesPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", "0");

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         // NUnit does not run TearDown after a SetUp that failed, so a listener
         // that did not come up is taken down here before the failure goes on.
         try
         {
            Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).Status, "The REST listener did not answer /api/v1/status.");
         }
         catch (Exception)
         {
            StopListeners();
            throw;
         }
      }

      [TearDown]
      public void StopListeners()
      {
         try
         {
            // Whatever a test changed goes back, whether it reached its own last
            // line or not: a base URL left set would put a distribution point into
            // the certificates of every fixture after this one, and a directory
            // left off would empty it.
            Admin("PUT", "/api/v1/settings", "{\"smime_authority_base_url\":\"\",\"directory_enabled\":true}");
         }
         finally
         {
            IniFileSetting.Write("WebServicesHttpPort", "0");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A domain without an authority answers 404 on the root route and the enrolment says so; created, the root downloads over plain HTTP by the domain's name and by its fingerprint and never from the REST listener, parses, is self-signed with CA:TRUE pathlen:0, keyCertSign/cRLSign, emailProtection and a critical name constraint to the domain, for ten years, matches the fingerprint the Deck is answered, and the creation is on the audit trail with the key withheld; a second create is 409.")]
      public void AnAuthorityIsCreatedOnceAndItsRootIsPublished()
      {
         Assert.AreEqual(404, Public("/smime-ca/" + _domain.Name + ".pem").Status, "No authority: the root route is a 404.");
         Response none = Admin("GET", AuthorityPath, null);
         Assert.AreEqual(200, none.Status, none.Body);
         Assert.IsNull(MiniJson.At(MiniJson.Object(none.Body), "authority"), none.Body);

         RSA key;
         Response refused = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         Assert.AreEqual(404, refused.Status, refused.Body);
         StringAssert.Contains("no S/MIME authority", refused.Body, "The enrolment says why.");
         StringAssert.Contains("\"available\":false", As(AdaAddress, "GET", "/api/v1/me/smime", null).Body,
            "The webmail is told the domain has none.");

         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         Dictionary<string, object> document = MiniJson.Object(created.Body);
         Assert.AreEqual("P-256", MiniJson.Text(document, "authority.key_type"), created.Body);
         Assert.IsTrue(MiniJson.Flag(document, "authority.key_opens"), "The key opens on the machine that protected it.");

         Response again = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(409, again.Status, again.Body);
         Assert.AreEqual(400, Admin("POST", AuthorityPath, "{\"key\":\"dsa\"}").Status, "A key type the authority does not make is refused.");

         X509Certificate2 root = RootOf(_domain.Name);
         StringAssert.Contains("CN=" + _domain.Name + " S/MIME Authority", root.Subject);
         Assert.AreEqual(root.Subject, root.Issuer, "Self-signed.");
         var constraints = root.Extensions["2.5.29.19"] as X509BasicConstraintsExtension;
         Assert.IsNotNull(constraints, "basicConstraints is present.");
         Assert.IsTrue(constraints.CertificateAuthority && constraints.HasPathLengthConstraint && constraints.PathLengthConstraint == 0,
            "CA:TRUE, pathlen:0.");
         var usage = root.Extensions["2.5.29.15"] as X509KeyUsageExtension;
         Assert.IsNotNull(usage);
         Assert.AreEqual(X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.CrlSign, usage.KeyUsages);
         Assert.That((root.NotAfter.ToUniversalTime() - root.NotBefore.ToUniversalTime()).TotalDays, Is.InRange(3650.0, 3655.0), "Ten years.");
         Assert.That(root.GetSerialNumber().Length, Is.InRange(1, 16), "A random serial of at most 127 bits.");
         Assert.AreEqual(Sha256Hex(root.RawData), MiniJson.Text(document, "authority.fingerprint"),
            "The fingerprint the Deck shows is the root the listener serves.");

         // What a leaked key would otherwise cost: the root vouches for S/MIME,
         // at the domain's own addresses, and nothing else.
         Assert.IsTrue(EnhancedUsagesOf(root).Contains(EmailProtection), "The root is for emailProtection.");
         X509Extension nameConstraints = root.Extensions["2.5.29.30"];
         Assert.IsNotNull(nameConstraints, "The root carries a name constraint.");
         Assert.IsTrue(nameConstraints.Critical, "A name constraint is critical (RFC 5280).");
         StringAssert.Contains(_domain.Name, Encoding.ASCII.GetString(nameConstraints.RawData), "It permits the domain's own addresses.");

         using (var chain = NewChain())
         {
            chain.ChainPolicy.ExtraStore.Add(root);
            Assert.IsTrue(chain.Build(root), "The root's own signature verifies.");
            Assert.IsTrue(chain.ChainStatus.All(s => s.Status == X509ChainStatusFlags.UntrustedRoot || s.Status == X509ChainStatusFlags.NoError),
               string.Join(", ", chain.ChainStatus.Select(s => s.Status.ToString())));
         }

         Response der = Public("/smime-ca/" + _domain.Name + ".cer");
         Assert.AreEqual(200, der.Status);
         Assert.AreEqual("application/pkix-cert", der.Header("Content-Type"));
         CollectionAssert.AreEqual(root.RawData, der.BodyBytes, "The DER is the same certificate.");

         // By its fingerprint, the name an issued certificate gives it; and never
         // from the REST listener.
         Response byFingerprint = Public("/smime-ca/" + Sha256Hex(root.RawData) + ".cer");
         Assert.AreEqual(200, byFingerprint.Status);
         CollectionAssert.AreEqual(root.RawData, byFingerprint.BodyBytes, "The fingerprint names the same certificate.");
         Assert.AreNotEqual(200, Raw(RestPort, "GET", "/smime-ca/" + _domain.Name + ".pem", null, null, null, null).Status,
            "The root is the web services listener's to serve, not the REST listener's.");

         Response trail = Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20authority&action=created&limit=5", null);
         Assert.AreEqual(200, trail.Status, trail.Body);
         StringAssert.Contains(_domain.Name, trail.Body, "The creation is on the audit trail, under the domain's name.");
         StringAssert.Contains("caprivatekey=(changed)", trail.Body, "The key column is recorded, as changed and nothing more.");
         object envelope;
         if (TryScalar("select caprivatekey from hm_smimeca where cadomainid = " + _domain.ID, out envelope))
         {
            string stored = Convert.ToString(envelope);
            StringAssert.StartsWith("DPAPI:", stored, "The key is a DPAPI envelope, never the legacy scheme.");
            StringAssert.DoesNotContain(stored.Substring(6, 40), trail.Body, "No part of the envelope reached the audit trail.");
         }

         // The negative control: another domain, with none, still answers 404.
         SingletonProvider<TestSetup>.Instance.AddDomain("no-authority.test");
         Assert.AreEqual(404, Public("/smime-ca/no-authority.test.pem").Status);
         Assert.AreEqual(404, Public("/smime-ca/no-authority.test.crl").Status);
         Assert.AreEqual(404, Public("/smime-ca/" + new string('0', 64) + ".pem").Status, "A fingerprint no root has is a 404 too.");
      }

      [Test]
      [Description("The authority's key is a DPAPI envelope even with ProtectStoredSecretsWithDPAPI off, which is what makes the server's other secrets use the legacy scheme - and it still signs.")]
      public void TheKeyIsNeverStoredUnderTheLegacyScheme()
      {
         if (!IsCeDatabase())
            Assert.Inconclusive("The stored envelope is read from the SQL CE test database; this server runs another.");

         string before = IniFileSetting.Read("ProtectStoredSecretsWithDPAPI");
         IniFileSetting.Write("ProtectStoredSecretsWithDPAPI", "0");
         _application.Reinitialize();
         try
         {
            Response created = Admin("POST", AuthorityPath, "{}");
            Assert.AreEqual(201, created.Status, created.Body);

            object envelope;
            Assert.IsTrue(TryScalar("select caprivatekey from hm_smimeca where cadomainid = " + _domain.ID, out envelope));
            StringAssert.StartsWith("DPAPI:", Convert.ToString(envelope), "Not the legacy scheme, whatever the setting says.");

            RSA key;
            Assert.AreEqual(201, Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Status, "And it opens: an enrolment is signed.");
         }
         finally
         {
            if (string.IsNullOrEmpty(before))
               IniFileSetting.Delete("ProtectStoredSecretsWithDPAPI");
            else
               IniFileSetting.Write("ProtectStoredSecretsWithDPAPI", before);
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A request for the account's own address, or its alias, is signed: one year, chained to the root, CA:FALSE, SAN rfc822Name and nothing else whatever the request asked, emailAddress, emailProtection, SKI and the root's AKI, the account's own name, the serial in the issued list. Another account's address or alias, an inactive alias, two addresses, an address too long for a certificate, a 1024-bit key, a request whose signature does not verify, an app password, a support session and an unauthenticated request are refused.")]
      public void EnrolmentSignsTheAccountsOwnAddressAndNothingElse()
      {
         _ada.PersonFirstName = "Ada";
         _ada.PersonLastName = "Lovelace";
         _ada.Save();
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);
         X509Certificate2 root = RootOf(_domain.Name);

         RSA key;
         Response issued = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         Assert.AreEqual(201, issued.Status, issued.Body);
         Dictionary<string, object> answer = MiniJson.Object(issued.Body);
         X509Certificate2 certificate = CertificateOf(MiniJson.Text(answer, "certificate"));

         Assert.AreEqual(root.Thumbprint, ChainEnd(certificate, root).Thumbprint, "It chains to the root the listener serves.");
         Assert.AreEqual(AdaAddress, EmailNameOf(certificate), "The subjectAltName carries the address.");
         StringAssert.Contains("CN=Ada Lovelace", certificate.Subject, "The name is the account's, not the request's.");
         StringAssert.Contains("E=" + AdaAddress, certificate.Subject, "And the subject's emailAddress is the address.");
         Assert.IsTrue(EnhancedUsagesOf(certificate).Contains(EmailProtection), "emailProtection.");
         var usage = certificate.Extensions["2.5.29.15"] as X509KeyUsageExtension;
         Assert.AreEqual(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, usage.KeyUsages, "An RSA key signs and is encrypted to.");
         var leafConstraints = certificate.Extensions["2.5.29.19"] as X509BasicConstraintsExtension;
         Assert.IsNotNull(leafConstraints);
         Assert.IsFalse(leafConstraints.CertificateAuthority, "CA:FALSE.");
         var subjectKey = certificate.Extensions["2.5.29.14"] as X509SubjectKeyIdentifierExtension;
         Assert.IsNotNull(subjectKey, "A subject key identifier.");
         Assert.AreEqual(SubjectKeyIdOf(root), AuthorityKeyIdOf(certificate), "The authority key identifier is the root's.");
         Assert.That((certificate.NotAfter.ToUniversalTime() - certificate.NotBefore.ToUniversalTime()).TotalDays, Is.InRange(365.0, 365.01), "One year.");
         Assert.AreEqual(MiniJson.Number(answer, "serial"), Convert.ToInt64(certificate.SerialNumber, 16), "The serial answered is the certificate's.");
         Assert.AreEqual(Sha256Hex(certificate.RawData), MiniJson.Text(answer, "fingerprint"));
         CollectionAssert.AreEqual(root.RawData, Convert.FromBase64String((string) MiniJson.List(answer, "chain")[0]), "The chain answered is the root.");
         Assert.IsFalse(MiniJson.Flag(answer, "confirmed"), "It is pending until its key is stored.");
         Assert.IsNull(certificate.Extensions["2.5.29.31"], "No base URL, no CRL distribution point.");
         Assert.IsNull(certificate.Extensions["1.3.6.1.5.5.7.1.1"], "No base URL, no authority information access.");

         Response listing = Admin("GET", AuthorityPath, null);
         StringAssert.Contains("\"serial\":" + MiniJson.Number(answer, "serial") + ",\"address\":\"" + AdaAddress + "\"", listing.Body);
         StringAssert.Contains("\"status\":\"valid\"", listing.Body);
         StringAssert.Contains("\"confirmed\":false", listing.Body);
         StringAssert.Contains("\"fingerprint\":\"" + MiniJson.Text(answer, "fingerprint") + "\"", As(AdaAddress, "GET", "/api/v1/me/smime", null).Body,
            "The person's page lists it.");

         // An alias of the account's own domain that points at it.
         string alias = "lovelace@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, alias, AdaAddress);
         Response forAlias = Enrol(AdaAddress, RsaRequest(2048, alias, out key), true);
         Assert.AreEqual(201, forAlias.Status, forAlias.Body);
         Assert.AreEqual(alias, EmailNameOf(CertificateOf(MiniJson.Text(MiniJson.Object(forAlias.Body), "certificate"))));

         // An elliptic-curve key: it signs and agrees keys.
         using (ECDsa ec = ECDsa.Create(ECCurve.NamedCurves.nistP256))
         {
            var request = new CertificateRequest("CN=Ada, E=" + AdaAddress, ec, HashAlgorithmName.SHA256);
            request.CertificateExtensions.Add(EmailName(AdaAddress));
            Response elliptic = Enrol(AdaAddress, request.CreateSigningRequest(), true);
            Assert.AreEqual(201, elliptic.Status, elliptic.Body);
            var ecUsage = CertificateOf(MiniJson.Text(MiniJson.Object(elliptic.Body), "certificate")).Extensions["2.5.29.15"] as X509KeyUsageExtension;
            Assert.AreEqual(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyAgreement, ecUsage.KeyUsages);
         }

         // A request that asks to be an authority and names a host as well: the
         // certificate is built from nothing the request said but its key and
         // its address.
         using (RSA greedy = RSA.Create(2048))
         {
            var request = new CertificateRequest("CN=Ada, E=" + AdaAddress, greedy, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            request.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
            var names = new SubjectAlternativeNameBuilder();
            names.AddEmailAddress(AdaAddress);
            names.AddDnsName("mail." + _domain.Name);
            request.CertificateExtensions.Add(names.Build());
            Response asked = Enrol(AdaAddress, request.CreateSigningRequest(), true);
            Assert.AreEqual(201, asked.Status, asked.Body);
            X509Certificate2 made = CertificateOf(MiniJson.Text(MiniJson.Object(asked.Body), "certificate"));
            var madeConstraints = made.Extensions["2.5.29.19"] as X509BasicConstraintsExtension;
            Assert.IsFalse(madeConstraints.CertificateAuthority, "Asked for CA:TRUE, issued CA:FALSE.");
            List<Der> sanEntries = Der.Read(made.Extensions["2.5.29.17"].RawData, 0).Children();
            Assert.AreEqual(1, sanEntries.Count, "The subjectAltName holds the address alone.");
            Assert.AreEqual(0x81, sanEntries[0].Tag, "And it is an rfc822Name - no dNSName was copied.");
         }

         // What is refused, each with a 400 that names it.
         Response other = Enrol(AdaAddress, RsaRequest(2048, BobAddress, out key), false);
         Assert.AreEqual(400, other.Status, other.Body);
         StringAssert.Contains(BobAddress, other.Body, "Another account's address is not certified for this one.");
         Assert.AreEqual(400, Enrol(AdaAddress, RsaRequest(2048, "nobody@" + _domain.Name, out key), false).Status, "Nor an address nobody has.");

         string toBob = "tobob@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, toBob, BobAddress);
         Assert.AreEqual(400, Enrol(AdaAddress, RsaRequest(2048, toBob, out key), false).Status, "Nor an alias that points at another account.");

         string sleeping = "sleeping@" + _domain.Name;
         Alias inactive = SingletonProvider<TestSetup>.Instance.AddAlias(_domain, sleeping, AdaAddress);
         inactive.Active = false;
         inactive.Save();
         Assert.AreEqual(400, Enrol(AdaAddress, RsaRequest(2048, sleeping, out key), false).Status, "Nor an alias that is switched off.");

         using (RSA two = RSA.Create(2048))
         {
            var request = new CertificateRequest("CN=Ada, E=" + AdaAddress, two, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            request.CertificateExtensions.Add(EmailName(BobAddress));
            Response both = Enrol(AdaAddress, request.CreateSigningRequest(), false);
            Assert.AreEqual(400, both.Status, both.Body);
            StringAssert.Contains("more than one address", both.Body, "An emailAddress and a different rfc822Name are two addresses.");
         }

         Response tooLong = Enrol(AdaAddress, RsaRequest(2048, new string('a', 130) + "@" + _domain.Name, out key), false);
         Assert.AreEqual(400, tooLong.Status, tooLong.Body);
         StringAssert.Contains("128", tooLong.Body, "An address longer than a certificate's emailAddress holds is a 400, not a failed signature.");

         Response small = Enrol(AdaAddress, RsaRequest(1024, AdaAddress, out key), false);
         Assert.AreEqual(400, small.Status, small.Body);
         StringAssert.Contains("1024", small.Body);

         byte[] tampered = RsaRequest(2048, AdaAddress, out key);
         int at = IndexOf(tampered, Encoding.ASCII.GetBytes("Test"));
         tampered[at] = (byte) 'B';
         Response broken = Enrol(AdaAddress, tampered, false);
         Assert.AreEqual(400, broken.Status, broken.Body);
         StringAssert.Contains("signature", broken.Body, "A request its own key did not sign is refused.");

         Assert.AreEqual(400, As(AdaAddress, "POST", "/api/v1/me/smime/enroll", "{\"csr\":\"not a request\"}").Status);
         Assert.AreEqual(400, As(AdaAddress, "POST", "/api/v1/me/smime/enroll", "{\"csr\":\"" + Convert.ToBase64String(RsaRequest(2048, AdaAddress, out key)) + "\",\"key\":\"x\"}").Status,
            "An unknown field is refused.");
         Assert.AreEqual(401, Raw(RestPort, "POST", "/api/v1/me/smime/enroll", null,
            "{\"csr\":\"" + Convert.ToBase64String(RsaRequest(2048, AdaAddress, out key)) + "\"}", "application/json", null).Status,
            "Without a credential there is no enrolment.");
         Assert.AreEqual(403, Admin("POST", "/api/v1/me/smime/enroll", "{\"csr\":\"x\"}").Status, "The administrator password is not an account.");

         // A credential for a program, and an administrator acting for the
         // person: whoever holds the key reads the mail encrypted to it, so
         // neither may have the authority certify one.
         Response minted = As(AdaAddress, "POST", "/api/v1/me/app-passwords", "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.Status, minted.Body);
         string asProgram = Auth(AdaAddress, Regex.Match(minted.Body, "\"password\":\"([^\"]+)\"").Groups[1].Value);
         Response byProgram = Raw(RestPort, "POST", "/api/v1/me/smime/enroll", asProgram,
            "{\"csr\":\"" + Convert.ToBase64String(RsaRequest(2048, AdaAddress, out key)) + "\"}", "application/json", null);
         Assert.AreEqual(403, byProgram.Status, byProgram.Body);
         StringAssert.Contains("app password", byProgram.Body);
         Assert.AreEqual(403, Raw(RestPort, "DELETE", "/api/v1/me/smime/issued/" + MiniJson.Number(answer, "serial"), asProgram, null, null, null).Status,
            "Nor revoke the person's certificate.");
         Assert.AreEqual(200, Raw(RestPort, "GET", "/api/v1/me/smime", asProgram, null, null, null).Status, "The same credential still reads the store.");

         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/preferences", "{\"support_allowed\":\"1\"}").Status);
         Response opened = Admin("POST", "/api/v1/accounts/" + AdaAddress + "/support-session", null);
         Assert.AreEqual(201, opened.Status, opened.Body);
         string cookie = Regex.Match(opened.Header("Set-Cookie") ?? "", "hmailsession=([^;]+)").Groups[1].Value;
         Assert.IsFalse(string.IsNullOrEmpty(cookie), opened.HeaderText);
         Response bySupport = Raw(RestPort, "POST", "/api/v1/me/smime/enroll", null,
            "{\"csr\":\"" + Convert.ToBase64String(RsaRequest(2048, AdaAddress, out key)) + "\"}", "application/json",
            "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(403, bySupport.Status, bySupport.Body);
         StringAssert.Contains("support session", bySupport.Body);

         Assert.That(MiniJson.List(MiniJson.Object(Admin("GET", AuthorityPath, null).Body), "issued").Count, Is.EqualTo(4),
            "Four were issued - the address, the alias, the elliptic key and the greedy request - and nothing refused was.");
      }

      [Test]
      [Description("A domain whose name would pass the 64 characters a commonName holds gets a root named by the words alone, still constrained to the domain.")]
      public void ALongDomainNameGivesTheRootTheWordsAlone()
      {
         string longName = "a-domain-name-long-enough-to-pass-the-common-name-limit.test";
         SingletonProvider<TestSetup>.Instance.AddDomain(longName);
         Response created = Admin("POST", "/api/v1/domains/" + longName + "/smime-ca", "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         X509Certificate2 root = RootOf(longName);
         Assert.AreEqual("CN=S/MIME Authority", root.Subject, "The words alone.");
         StringAssert.Contains(longName, Encoding.ASCII.GetString(root.Extensions["2.5.29.30"].RawData), "The constraint still names the domain.");
      }

      [Test]
      [Description("A request in PEM, a P-384 key and an account with no name are taken - the CN is then the address; a P-521 key and a request over 16 KB are refused.")]
      public void TheRequestFormsAndKeysTheAuthorityTakes()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);

         RSA key;
         string pem = PemOf(RsaRequest(2048, AdaAddress, out key), "CERTIFICATE REQUEST");
         Response fromPem = As(AdaAddress, "POST", "/api/v1/me/smime/enroll", "{\"csr\":\"" + pem.Replace("\r", "").Replace("\n", "\\n") + "\",\"keep\":false}");
         Assert.AreEqual(201, fromPem.Status, fromPem.Body);
         X509Certificate2 certificate = CertificateOf(MiniJson.Text(MiniJson.Object(fromPem.Body), "certificate"));
         StringAssert.Contains("CN=" + AdaAddress, certificate.Subject, "An account with no name is named by its address.");

         Response p384 = Enrol(AdaAddress, EcRequest(ECCurve.NamedCurves.nistP384, AdaAddress), true);
         Assert.AreEqual(201, p384.Status, p384.Body);

         Response p521 = Enrol(AdaAddress, EcRequest(ECCurve.NamedCurves.nistP521, AdaAddress), true);
         Assert.AreEqual(400, p521.Status, p521.Body);
         StringAssert.Contains("P-256 or P-384", p521.Body);

         Response huge = As(AdaAddress, "POST", "/api/v1/me/smime/enroll", "{\"csr\":\"" + new string('A', 16 * 1024 + 4) + "\"}");
         Assert.AreEqual(400, huge.Status, huge.Body);
         StringAssert.Contains("16 KB", huge.Body);
      }

      [Test]
      [Description("With smime_authority_base_url set, a certificate names its CRL and its root under it by the root's fingerprint; a base URL that is not http(s) with a host, or holds what a URI cannot, is refused; the older path by the root's number is still answered.")]
      public void TheBaseUrlPutsTheCrlIntoTheCertificates()
      {
         foreach (string refused in new[] { "ftp://pki.example.test", "http://pki.example.test/a,b", "http://", "https://", "http:///x",
                                            "http://pki.example.test/a{b}", "http://user@pki.example.test", "http://pki.example.test:99999" })
         {
            Response answer = Admin("PUT", "/api/v1/settings", "{\"smime_authority_base_url\":\"" + refused + "\"}");
            Assert.AreEqual(400, answer.Status, refused + " was taken: " + answer.Body);
         }
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"smime_authority_base_url\":\"http://pki.example.test:8080/pki/\"}").Status,
            "A host, a port and a path are taken.");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"smime_authority_base_url\":\"http://pki.example.test/\"}").Status);

         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long rootId = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         string rootFingerprint = MiniJson.Text(MiniJson.Object(created.Body), "authority.fingerprint");

         RSA key;
         Response issued = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         Assert.AreEqual(201, issued.Status, issued.Body);
         X509Certificate2 certificate = CertificateOf(MiniJson.Text(MiniJson.Object(issued.Body), "certificate"));

         X509Extension distribution = certificate.Extensions["2.5.29.31"];
         Assert.IsNotNull(distribution, "A CRL distribution point.");
         StringAssert.Contains("http://pki.example.test/smime-ca/" + rootFingerprint + ".crl", Encoding.ASCII.GetString(distribution.RawData));
         X509Extension access = certificate.Extensions["1.3.6.1.5.5.7.1.1"];
         Assert.IsNotNull(access, "Authority information access.");
         StringAssert.Contains("http://pki.example.test/smime-ca/" + rootFingerprint + ".cer", Encoding.ASCII.GetString(access.RawData));

         // And the path it names is the one the listener answers.
         Response crl = Public("/smime-ca/" + rootFingerprint + ".crl");
         Assert.AreEqual(200, crl.Status);
         Assert.AreEqual("application/pkix-crl", crl.Header("Content-Type"));
         Assert.AreEqual(200, Public("/smime-ca/" + rootFingerprint + ".cer").Status);

         // The first build wrote the root's number into certificates; that form
         // is still answered.
         Assert.AreEqual(200, Public("/smime-ca/" + _domain.Name + "/" + rootId + ".crl").Status);
      }

      [Test]
      [Description("An enrolment is pending until its key is stored: not answered to colleagues and superseding nothing; storing the key in the account's own store, or the confirm route, confirms it, and only then is the address's earlier certificate revoked as superseded. Another account cannot confirm it, and a revoked one cannot be confirmed.")]
      public void EnrolmentIsPendingUntilItsKeyIsStored()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);
         string lookup = "/api/v1/me/directory/" + Uri.EscapeDataString(AdaAddress) + "/smime";

         RSA key;
         Response firstIssued = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         Assert.AreEqual(201, firstIssued.Status, firstIssued.Body);
         Dictionary<string, object> first = MiniJson.Object(firstIssued.Body);
         long firstSerial = MiniJson.Number(first, "serial");
         Assert.AreEqual(404, As(BobAddress, "GET", lookup, null).Status, "A pending certificate is not answered: its key may never be stored.");

         Response stored = StoreOwn(AdaAddress, first);
         Assert.AreEqual(201, stored.Status, stored.Body);
         StringAssert.Contains("\"authority\":true", stored.Body, "The server marks it as the authority's, from its bytes.");
         Response found = As(BobAddress, "GET", lookup, null);
         Assert.AreEqual(200, found.Status, "Stored, it is answered: " + found.Body);
         StringAssert.Contains(MiniJson.Text(first, "fingerprint"), found.Body);

         Response secondIssued = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         Assert.AreEqual(201, secondIssued.Status, secondIssued.Body);
         Dictionary<string, object> second = MiniJson.Object(secondIssued.Body);
         long secondSerial = MiniJson.Number(second, "serial");
         Assert.AreEqual(firstSerial, (long) (double) MiniJson.List(second, "supersedes").Single(), "It names what it will supersede.");
         Assert.AreEqual("valid", MiniJson.Text(IssuedRow(Admin("GET", AuthorityPath, null).Body, firstSerial), "status"),
            "Nothing is superseded yet: a store that never happens leaves the first valid.");
         StringAssert.DoesNotContain(MiniJson.Text(second, "fingerprint"), As(BobAddress, "GET", lookup, null).Body, "The second is not answered yet.");

         Assert.AreEqual(404, As(BobAddress, "POST", "/api/v1/me/smime/issued/" + secondSerial + "/confirm", null).Status, "Another account's is the same 404 as none.");
         Response confirmed = As(AdaAddress, "POST", "/api/v1/me/smime/issued/" + secondSerial + "/confirm", null);
         Assert.AreEqual(200, confirmed.Status, confirmed.Body);
         Assert.AreEqual(firstSerial, (long) (double) MiniJson.List(MiniJson.Object(confirmed.Body), "superseded").Single(), confirmed.Body);

         Dictionary<string, object> firstRow = IssuedRow(Admin("GET", AuthorityPath, null).Body, firstSerial);
         Assert.AreEqual("revoked", MiniJson.Text(firstRow, "status"));
         Assert.AreEqual("superseded", MiniJson.Text(firstRow, "reason"));
         Assert.IsTrue(CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes).Revoked.Any(r => r.Serial == firstSerial && r.Reason == 4));
         Response now = As(BobAddress, "GET", lookup, null);
         StringAssert.Contains(MiniJson.Text(second, "fingerprint"), now.Body, "The second is answered now.");
         StringAssert.DoesNotContain(MiniJson.Text(first, "fingerprint"), now.Body, "And the first no longer.");

         Response twice = As(AdaAddress, "POST", "/api/v1/me/smime/issued/" + secondSerial + "/confirm", null);
         Assert.AreEqual(200, twice.Status, twice.Body);
         Assert.AreEqual(0, MiniJson.List(MiniJson.Object(twice.Body), "superseded").Count, "Confirming a confirmed certificate changes nothing.");
         Assert.AreEqual(409, As(AdaAddress, "POST", "/api/v1/me/smime/issued/" + firstSerial + "/confirm", null).Status, "A revoked one cannot be confirmed.");
      }

      [Test]
      [Description("A second enrolment for the same address, once its key is stored, revokes the first as superseded - revoked in the list and on the CRL - unless it keeps it.")]
      public void ASecondEnrolmentSupersedesTheFirstUnlessKept()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);
         X509Certificate2 root = RootOf(_domain.Name);

         RSA key;
         long first = MiniJson.Number(EnrolAndStore(AdaAddress, RsaRequest(2048, AdaAddress, out key), false), "serial");
         Dictionary<string, object> secondAnswer = EnrolAndStore(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         long secondSerial = MiniJson.Number(secondAnswer, "serial");
         Assert.AreEqual(first + 1, secondSerial, "One counter for the domain.");

         Response listing = Admin("GET", AuthorityPath, null);
         Assert.AreEqual("superseded", MiniJson.Text(IssuedRow(listing.Body, first), "reason"), listing.Body);

         Crl list = CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes);
         Assert.IsTrue(VerifiesWith(list, root), "The CRL is the root's.");
         Assert.IsTrue(list.Revoked.Any(r => r.Serial == first && r.Reason == 4), "The superseded one is on the CRL, reason superseded.");
         Assert.IsFalse(list.Revoked.Any(r => r.Serial == secondSerial), "The new one is not.");

         Dictionary<string, object> kept = EnrolAndStore(AdaAddress, RsaRequest(2048, AdaAddress, out key), true);
         Assert.AreEqual(0, MiniJson.List(kept, "supersedes").Count, "Kept: nothing to supersede.");
         Assert.AreEqual("valid", MiniJson.Text(IssuedRow(Admin("GET", AuthorityPath, null).Body, secondSerial), "status"));
         Assert.IsFalse(CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes).Revoked.Any(r => r.Serial == secondSerial),
            "A second device keeps the first device's certificate valid.");
      }

      [Test]
      [Description("An account is issued five certificates a day and a sixth is 429 while another account still enrols; and it holds ten valid ones at once, an eleventh being 409.")]
      public void AnAccountEnrolsFiveTimesADayAndHoldsTenCertificates()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);

         RSA key;
         for (int i = 0; i < 5; i++)
         {
            Response issued = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), true);
            Assert.AreEqual(201, issued.Status, "Enrolment " + (i + 1) + ": " + issued.Body);
         }

         Response sixth = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), true);
         Assert.AreEqual(429, sixth.Status, sixth.Body);
         StringAssert.Contains("in the last day", sixth.Body);
         Assert.AreEqual(201, Enrol(BobAddress, RsaRequest(2048, BobAddress, out key), true).Status, "The limit is the account's.");

         // Ten valid at once: the five are moved two days back, so that the day's
         // limit does not stand in front of it.
         if (!TryExecute("update hm_smimeissued set issuednotbefore = issuednotbefore - 172800 where issuedaccountid = " + _ada.ID))
            Assert.Inconclusive("The limit of ten needs the SQL CE test database to move the day's certificates back; this server runs another.");

         for (int i = 0; i < 5; i++)
         {
            Response issued = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), true);
            Assert.AreEqual(201, issued.Status, "Enrolment " + (i + 6) + ": " + issued.Body);
         }

         Assert.IsTrue(TryExecute("update hm_smimeissued set issuednotbefore = issuednotbefore - 172800 where issuedaccountid = " + _ada.ID));
         Response eleventh = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), true);
         Assert.AreEqual(409, eleventh.Status, eleventh.Body);
         StringAssert.Contains("revoke one first", eleventh.Body);
      }

      [Test]
      [Description("A colleague of the domain fetches the certificate by address, opted out or not; an account of another domain, an inactive account's certificate, a revoked one, an alias pointed elsewhere and any while the directory is off, are 404. A stored copy is marked as the authority's from its own bytes - a real fingerprint beside other bytes is not - and its record's status follows a revocation.")]
      public void AColleagueFindsTheCertificateByAddressWithinTheDomain()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);
         RSA key;
         Dictionary<string, object> answer = EnrolAndStore(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         string fingerprint = MiniJson.Text(answer, "fingerprint");
         string lookup = "/api/v1/me/directory/" + Uri.EscapeDataString(AdaAddress) + "/smime";

         Response found = As(BobAddress, "GET", lookup, null);
         Assert.AreEqual(200, found.Status, found.Body);
         StringAssert.Contains("\"fingerprint\":\"" + fingerprint + "\"", found.Body);
         StringAssert.Contains("BEGIN CERTIFICATE", found.Body, "In PEM as well.");

         // Stored as a correspondent's certificate, the server marks it; a
         // certificate it did not issue is not marked, whatever the page says.
         string body = "{\"address\":\"" + AdaAddress + "\",\"name\":\"Ada\",\"fingerprint\":\"" + fingerprint + "\",\"certificate\":\"" +
                       MiniJson.Text(answer, "certificate") + "\",\"not_after\":" + MiniJson.Number(answer, "not_after") + "}";
         Response stored = As(BobAddress, "PUT", "/api/v1/me/smime/recipients", body);
         Assert.AreEqual(201, stored.Status, stored.Body);
         StringAssert.Contains("\"authority\":true", stored.Body);
         StringAssert.Contains("\"authority_status\":\"valid\"", stored.Body);
         X509Certificate2 stranger = SelfSigned("stranger@elsewhere.test");
         Response foreign = As(BobAddress, "PUT", "/api/v1/me/smime/recipients",
            "{\"address\":\"stranger@elsewhere.test\",\"name\":\"Stranger\",\"fingerprint\":\"" + Sha256Hex(stranger.RawData) + "\",\"certificate\":\"" +
            Convert.ToBase64String(stranger.RawData) + "\",\"not_after\":0,\"authority\":true}");
         Assert.AreEqual(201, foreign.Status, foreign.Body);
         StringAssert.Contains("\"authority\":false", foreign.Body, "The mark is the server's to set.");

         // And a real issued fingerprint put beside another certificate's bytes is
         // not marked: the mark is decided from the bytes.
         string carol = "carol@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, carol, Password);
         Response paired = As(carol, "PUT", "/api/v1/me/smime/recipients",
            "{\"address\":\"" + AdaAddress + "\",\"name\":\"Ada\",\"fingerprint\":\"" + fingerprint + "\",\"certificate\":\"" +
            Convert.ToBase64String(stranger.RawData) + "\",\"not_after\":0}");
         Assert.AreEqual(201, paired.Status, paired.Body);
         StringAssert.Contains("\"authority\":false", paired.Body, "An issued fingerprint does not mark bytes that are not its certificate.");

         // Opting out of the listing is not invisibility.
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":true}").Status);
         Assert.AreEqual(200, As(BobAddress, "GET", lookup, null).Status, "Somebody who has the address still finds the certificate.");

         // Another domain's account is answered its own domain and no other.
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("elsewhere-authority.test");
         SingletonProvider<TestSetup>.Instance.AddAccount(other, "carol@elsewhere-authority.test", Password);
         Assert.AreEqual(404, As("carol@elsewhere-authority.test", "GET", lookup, null).Status);

         // The directory switched off answers nothing, as its photo route does.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"directory_enabled\":false}").Status);
         Assert.AreEqual(404, As(BobAddress, "GET", lookup, null).Status);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"directory_enabled\":true}").Status);
         Assert.AreEqual(200, As(BobAddress, "GET", lookup, null).Status);

         // A revoked certificate is not answered, and the copy Bob stored says so.
         long serial = MiniJson.Number(answer, "serial");
         Assert.AreEqual(200, Admin("DELETE", AuthorityPath + "/issued/" + serial + "?reason=keyCompromise", null).Status);
         Assert.AreEqual(404, As(BobAddress, "GET", lookup, null).Status);
         StringAssert.Contains("\"authority_status\":\"revoked\"", As(BobAddress, "GET", "/api/v1/me/smime", null).Body,
            "The page is told its stored copy is revoked, and stops encrypting to it.");

         // A new one is answered again.
         Dictionary<string, object> again = EnrolAndStore(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         Response foundAgain = As(BobAddress, "GET", lookup, null);
         Assert.AreEqual(200, foundAgain.Status);
         StringAssert.Contains(MiniJson.Text(again, "fingerprint"), foundAgain.Body);

         // An alias's certificate, while the alias is still the account's.
         string aliasAddress = "lovelace@" + _domain.Name;
         Alias alias = SingletonProvider<TestSetup>.Instance.AddAlias(_domain, aliasAddress, AdaAddress);
         EnrolAndStore(AdaAddress, RsaRequest(2048, aliasAddress, out key), true);
         string aliasLookup = "/api/v1/me/directory/" + Uri.EscapeDataString(aliasAddress) + "/smime";
         Assert.AreEqual(200, As(BobAddress, "GET", aliasLookup, null).Status, "The alias's certificate is answered.");
         alias.Value = BobAddress;
         alias.Save();
         Assert.AreEqual(404, As(BobAddress, "GET", aliasLookup, null).Status, "Pointed at somebody else, the address is no longer Ada's.");

         // Nor an inactive account's.
         _ada.Active = false;
         _ada.Save();
         Assert.AreEqual(404, As(BobAddress, "GET", lookup, null).Status, "An account that cannot sign in is not answered.");
      }

      [Test]
      [Description("The CRL downloads, is X.509 v2 signed by the root, lists a revoked serial after the revoke and not before, with nextUpdate within a week; the first revocation's reason stands; the webmail's chain check refuses a revoked certificate at once; a revoke is on the audit trail; the person revokes their own and nobody else's, keyCompromise by default.")]
      public void TheCrlIsSignedByTheRootAndListsWhatWasRevoked()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);
         X509Certificate2 root = RootOf(_domain.Name);
         RSA key;
         Dictionary<string, object> firstAnswer = MiniJson.Object(Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Body);
         long serial = MiniJson.Number(firstAnswer, "serial");
         string certificate = MiniJson.Text(firstAnswer, "certificate");

         Response before = Public("/smime-ca/" + _domain.Name + ".crl");
         Assert.AreEqual(200, before.Status);
         Assert.AreEqual("application/pkix-crl", before.Header("Content-Type"));
         Crl listBefore = CrlOf(before.BodyBytes);
         Assert.AreEqual(1, listBefore.Version, "X.509 v2.");
         Assert.IsTrue(VerifiesWith(listBefore, root), "Signed by the root.");
         Assert.IsFalse(listBefore.Revoked.Any(r => r.Serial == serial), "Not revoked yet, not listed.");
         Assert.IsTrue(listBefore.NextUpdate.HasValue);
         Assert.That((listBefore.NextUpdate.Value - listBefore.ThisUpdate).TotalDays, Is.InRange(6.99, 7.01), "nextUpdate a week on.");
         Assert.That(listBefore.NextUpdate.Value, Is.GreaterThan(DateTime.UtcNow), "And not already past.");

         // The negative control: a changed byte and the signature no longer verifies.
         Crl altered = CrlOf(before.BodyBytes);
         altered.Tbs[altered.Tbs.Length - 1] ^= 0x01;
         Assert.IsFalse(VerifiesWith(altered, root), "The check above proves something.");

         string chainBody = "{\"certificates\":[\"" + certificate + "\"],\"purpose\":\"sign\"}";
         StringAssert.Contains("\"trusted\":true", As(BobAddress, "POST", "/api/v1/me/smime/chain", chainBody).Body, "Trusted before the revoke.");

         Assert.AreEqual(404, Admin("DELETE", AuthorityPath + "/issued/999999?reason=keyCompromise", null).Status);
         Assert.AreEqual(400, Admin("DELETE", AuthorityPath + "/issued/" + serial + "?reason=bored", null).Status);
         Response revoked = Admin("DELETE", AuthorityPath + "/issued/" + serial + "?reason=keyCompromise", null);
         Assert.AreEqual(200, revoked.Status, revoked.Body);
         Assert.AreEqual(409, Admin("DELETE", AuthorityPath + "/issued/" + serial + "?reason=superseded", null).Status, "Revoked already.");

         Crl listAfter = CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes);
         Assert.IsTrue(VerifiesWith(listAfter, root));
         Assert.IsTrue(listAfter.Revoked.Any(r => r.Serial == serial && r.Reason == 1), "Listed, reason keyCompromise - the first revocation's, not the refused second's.");

         // This webmail reads the authority's record, not a CRL: refused at once.
         Response verdict = As(BobAddress, "POST", "/api/v1/me/smime/chain", chainBody);
         Assert.AreEqual(200, verdict.Status, verdict.Body);
         StringAssert.Contains("\"trusted\":false", verdict.Body, verdict.Body);
         StringAssert.Contains("revoked (keyCompromise)", verdict.Body);

         Response trail = Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20certificate&action=updated&limit=10", null);
         Assert.AreEqual(200, trail.Status, trail.Body);
         StringAssert.Contains(AdaAddress + " serial " + serial, trail.Body, "The revoke is on the audit trail, by address and serial.");

         // The person's own route: their own certificate, and nobody else's.
         long own = MiniJson.Number(MiniJson.Object(Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Body), "serial");
         Assert.AreEqual(404, As(BobAddress, "DELETE", "/api/v1/me/smime/issued/" + own, null).Status, "Another account's is the same 404 as none.");
         Response mine = As(AdaAddress, "DELETE", "/api/v1/me/smime/issued/" + own + "?reason=superseded", null);
         Assert.AreEqual(200, mine.Status, mine.Body);
         Assert.IsTrue(CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes).Revoked.Any(r => r.Serial == own && r.Reason == 4));

         // With no reason given, keyCompromise.
         long unexplained = MiniJson.Number(MiniJson.Object(Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Body), "serial");
         Assert.AreEqual(200, As(AdaAddress, "DELETE", "/api/v1/me/smime/issued/" + unexplained, null).Status);
         Assert.IsTrue(CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes).Revoked.Any(r => r.Serial == unexplained && r.Reason == 1),
            "The default reason is keyCompromise.");
      }

      [Test]
      [Description("The chain check accepts a chain ending at the domain's authority and says authority: true, even though the machine's store does not know the root; a chain ending at an unknown self-signed root is refused, so is the authority's chain for a reader of another domain, and so is a certificate the root signed that the authority's record does not have.")]
      public void TheChainCheckTrustsTheDomainsAuthorityAndNothingElse()
      {
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long caId = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         RSA key;
         Dictionary<string, object> answer = MiniJson.Object(Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Body);
         string certificate = MiniJson.Text(answer, "certificate");

         Response verdict = As(BobAddress, "POST", "/api/v1/me/smime/chain", "{\"certificates\":[\"" + certificate + "\"],\"purpose\":\"sign\"}");
         Assert.AreEqual(200, verdict.Status, verdict.Body);
         StringAssert.Contains("\"trusted\":true", verdict.Body, verdict.Body);
         StringAssert.Contains("\"authority\":true", verdict.Body, verdict.Body);

         X509Certificate2 stranger = SelfSigned("stranger@elsewhere.test");
         Response unknown = As(BobAddress, "POST", "/api/v1/me/smime/chain",
            "{\"certificates\":[\"" + Convert.ToBase64String(stranger.RawData) + "\"],\"purpose\":\"sign\"}");
         Assert.AreEqual(200, unknown.Status, unknown.Body);
         StringAssert.Contains("\"trusted\":false", unknown.Body, unknown.Body);
         StringAssert.Contains("\"authority\":false", unknown.Body);

         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("other-authority.test");
         SingletonProvider<TestSetup>.Instance.AddAccount(other, "dan@other-authority.test", Password);
         Response outsider = As("dan@other-authority.test", "POST", "/api/v1/me/smime/chain", "{\"certificates\":[\"" + certificate + "\"],\"purpose\":\"sign\"}");
         StringAssert.Contains("\"trusted\":false", outsider.Body, "Another domain's reader is not given this domain's roots.");

         // A certificate the root's key signed that the record does not have -
         // what one made with a key that left this server would be.
         if (!TryExecute("delete from hm_smimeissued where issuedcaid = " + caId + " and issuedserial = " + MiniJson.Number(answer, "serial")))
            Assert.Inconclusive("The unrecorded certificate needs the SQL CE test database; this server runs another.");
         Response unrecorded = As(BobAddress, "POST", "/api/v1/me/smime/chain", "{\"certificates\":[\"" + certificate + "\"],\"purpose\":\"sign\"}");
         StringAssert.Contains("\"trusted\":false", unrecorded.Body, unrecorded.Body);
         StringAssert.Contains("not issued by this authority", unrecorded.Body);
      }

      [Test]
      [Description("After a rotation new certificates chain to the new root and take the next serial, the old root is still downloadable by its fingerprint with its CRL, the old certificate still verifies through the chain route, and the retirement is on the audit trail; once the old root's certificates have expired it is neither published nor trusted.")]
      public void RotationKeepsTheOldRootPublishedAndItsCertificatesValid()
      {
         Assert.AreEqual(404, Admin("POST", AuthorityPath + "/rotate", "{}").Status, "Nothing to rotate.");
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long oldId = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         string oldFingerprint = MiniJson.Text(MiniJson.Object(created.Body), "authority.fingerprint");
         X509Certificate2 oldRoot = RootOf(_domain.Name);

         RSA key;
         Dictionary<string, object> oldAnswer = MiniJson.Object(Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Body);
         string oldCertificate = MiniJson.Text(oldAnswer, "certificate");

         Response rotated = Admin("POST", AuthorityPath + "/rotate", "{\"key\":\"rsa3072\"}");
         Assert.AreEqual(200, rotated.Status, rotated.Body);
         Dictionary<string, object> document = MiniJson.Object(rotated.Body);
         Assert.AreNotEqual(oldId, MiniJson.Number(document, "authority.id"));
         Assert.AreEqual("RSA-3072", MiniJson.Text(document, "authority.key_type"));
         Dictionary<string, object> retired = RetiredRoot(document, oldId);
         Assert.IsNotNull(retired, "The old root is listed as retired: " + rotated.Body);
         Assert.IsTrue(MiniJson.Flag(retired, "published"), "Published while a certificate it signed lives.");
         Assert.IsTrue(MiniJson.Flag(retired, "key_held"), "And it keeps its key to sign its CRL.");

         X509Certificate2 newRoot = RootOf(_domain.Name);
         Assert.AreNotEqual(oldRoot.Thumbprint, newRoot.Thumbprint, "The domain's root route answers the new root.");

         Response fresh = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), true);
         Assert.AreEqual(201, fresh.Status, fresh.Body);
         Dictionary<string, object> freshAnswer = MiniJson.Object(fresh.Body);
         X509Certificate2 freshCertificate = CertificateOf(MiniJson.Text(freshAnswer, "certificate"));
         Assert.AreEqual(newRoot.Thumbprint, ChainEnd(freshCertificate, newRoot, oldRoot).Thumbprint, "A new enrolment chains to the new root.");
         Assert.AreEqual(MiniJson.Number(oldAnswer, "serial") + 1, MiniJson.Number(freshAnswer, "serial"), "The serial counter is carried on.");

         Response oldPem = Public("/smime-ca/" + oldFingerprint + ".pem");
         Assert.AreEqual(200, oldPem.Status, "The old root is still downloadable while a certificate it signed is valid.");
         CollectionAssert.AreEqual(oldRoot.RawData, DerOfPem(oldPem.Body));
         Crl oldList = CrlOf(Public("/smime-ca/" + oldFingerprint + ".crl").BodyBytes);
         Assert.IsTrue(VerifiesWith(oldList, oldRoot), "The old root still signs its own CRL.");
         Assert.IsTrue(VerifiesWith(CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes), newRoot), "The domain's CRL is the new root's.");

         string chainBody = "{\"certificates\":[\"" + oldCertificate + "\"],\"purpose\":\"sign\"}";
         Response verdict = As(BobAddress, "POST", "/api/v1/me/smime/chain", chainBody);
         StringAssert.Contains("\"trusted\":true", verdict.Body, "A certificate signed before the rotation still verifies.");
         StringAssert.Contains("\"authority\":true", verdict.Body);

         // The retirement itself, which only a rotation writes; an enrolment's
         // own update of the root (its serial counter) does not carry caretired.
         Response trail = Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20authority&action=updated&limit=50", null);
         Assert.IsTrue(MiniJson.List(MiniJson.Object(trail.Body), "entries").Cast<Dictionary<string, object>>()
               .Any(e => Regex.IsMatch(MiniJson.Text(e, "detail") ?? "", "caretired=[1-9]")),
            "The retirement of the old root is on the audit trail: " + trail.Body);
         Assert.That(Regex.Matches(Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20authority&action=created&limit=20", null).Body, "\"action\":\"created\"").Count,
            Is.GreaterThanOrEqualTo(2), "Both roots' creation are.");

         // Once the last certificate the old root signed has expired, it has
         // nothing left to vouch for.
         if (!TryExecute("update hm_smimeissued set issuednotafter = 1 where issuedcaid = " + oldId))
            Assert.Inconclusive("Expiring the old root's certificates needs the SQL CE test database; this server runs another.");
         Assert.AreEqual(404, Public("/smime-ca/" + oldFingerprint + ".pem").Status, "No longer published.");
         StringAssert.Contains("\"trusted\":false", As(BobAddress, "POST", "/api/v1/me/smime/chain", chainBody).Body,
            "And no longer a trust anchor, whatever its old certificate says of its own validity.");
      }

      [Test]
      [Description("A root rotated away with no certificate alive has nothing to sign: its key is wiped at the rotation and it is not published.")]
      public void ARootRetiredWithNothingAliveLosesItsKey()
      {
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long oldId = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         string oldFingerprint = MiniJson.Text(MiniJson.Object(created.Body), "authority.fingerprint");
         Assert.AreEqual(200, Public("/smime-ca/" + oldFingerprint + ".pem").Status, "The current root is published.");

         Response rotated = Admin("POST", AuthorityPath + "/rotate", "{}");
         Assert.AreEqual(200, rotated.Status, rotated.Body);
         Dictionary<string, object> retired = RetiredRoot(MiniJson.Object(rotated.Body), oldId);
         Assert.IsNotNull(retired, rotated.Body);
         Assert.IsFalse(MiniJson.Flag(retired, "published"), "Nothing it signed is alive.");
         Assert.IsFalse(MiniJson.Flag(retired, "key_held"), "So its key is gone.");
         Assert.AreEqual(404, Public("/smime-ca/" + oldFingerprint + ".pem").Status);

         object envelope;
         if (TryScalar("select caprivatekey from hm_smimeca where caid = " + oldId, out envelope))
            Assert.AreEqual("", Convert.ToString(envelope), "The column itself is empty.");
      }

      [Test]
      [Description("A rotation whose new root cannot be written puts the old root back as the current one.")]
      public void ARotationThatCannotBeStoredPutsTheOldRootBack()
      {
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long id = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         X509Certificate2 root = RootOf(_domain.Name);

         IniFileSetting.Write("SimulateDatabaseFailureFor", "INSERT INTO hm_smimeca");
         _application.Reinitialize();
         Response rotated;
         try
         {
            rotated = Admin("POST", AuthorityPath + "/rotate", "{}");
         }
         finally
         {
            IniFileSetting.Write("SimulateDatabaseFailureFor", "");
            _application.Reinitialize();
         }
         CustomAsserts.AssertReportedError("HM6119");

         Assert.AreEqual(500, rotated.Status, rotated.Body);
         Dictionary<string, object> document = MiniJson.Object(Admin("GET", AuthorityPath, null).Body);
         Assert.AreEqual(id, MiniJson.Number(document, "authority.id"), "The old root is the current one again.");
         Assert.AreEqual(0, MiniJson.List(document, "retired").Count, "And nothing is left retired.");
         Assert.AreEqual(root.Thumbprint, RootOf(_domain.Name).Thumbprint);
      }

      [Test]
      [Description("A root whose key does not open on this machine signs nothing: an enrolment is 503 and says to rotate, the error is reported once, its CRL is the same 404 as a root that is not there while the root itself is served, and the Deck says the key does not open; a rotation mends it.")]
      public void AKeyThatDoesNotOpenSignsNothing()
      {
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long id = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         if (!TryExecute("update hm_smimeca set caprivatekey = 'DPAPI:AAAA' where caid = " + id))
            Assert.Inconclusive("Making the key unopenable needs the SQL CE test database; this server runs another.");

         RSA key;
         Response refused = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         Assert.AreEqual(503, refused.Status, refused.Body);
         StringAssert.Contains("rotates the authority", refused.Body);
         CustomAsserts.AssertReportedError("HM7231");

         Assert.AreEqual(404, Public("/smime-ca/" + _domain.Name + ".crl").Status, "No CRL it cannot sign.");
         Assert.AreEqual(200, Public("/smime-ca/" + _domain.Name + ".pem").Status, "The root itself is still served.");
         Assert.IsFalse(MiniJson.Flag(MiniJson.Object(Admin("GET", AuthorityPath, null).Body), "authority.key_opens"), "The Deck is told.");
         // Again: refused, and not reported again - the teardown's look at the
         // error log is what proves that.
         Assert.AreEqual(503, Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Status);

         Assert.AreEqual(200, Admin("POST", AuthorityPath + "/rotate", "{}").Status);
         Assert.AreEqual(201, Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Status, "Rotated, it signs.");
      }

      [Test]
      [Description("Deleting an account revokes every certificate the authority issued to it, as cessationOfOperation, keeps it on the CRL, takes the account's number off it, and names each on the audit trail; deleting an account that was issued none writes nothing there.")]
      public void DeletingAnAccountRevokesItsCertificates()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);
         X509Certificate2 root = RootOf(_domain.Name);
         string carol = "carol@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, carol, Password);
         RSA key;
         long serial = MiniJson.Number(MiniJson.Object(Enrol(carol, RsaRequest(2048, carol, out key), false).Body), "serial");

         // An account with no certificate: its deletion leaves no row here.
         string dave = "dave@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, dave, Password);
         long before = MiniJson.Number(MiniJson.Object(Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20certificate&limit=1", null).Body), "total");
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/accounts/" + dave, null).Status);
         Assert.AreEqual(before, MiniJson.Number(MiniJson.Object(Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20certificate&limit=1", null).Body), "total"),
            "An account that was issued nothing is deleted without a row on the S/MIME certificate trail.");

         Assert.AreEqual(200, Admin("DELETE", "/api/v1/accounts/" + carol, null).Status);

         Response listing = Admin("GET", AuthorityPath, null);
         Dictionary<string, object> row = IssuedRow(listing.Body, serial);
         Assert.AreEqual("revoked", MiniJson.Text(row, "status"), listing.Body);
         Assert.AreEqual("cessationOfOperation", MiniJson.Text(row, "reason"), listing.Body);
         Assert.AreEqual(0, MiniJson.Number(row, "account_id"), "The account's number is taken off it - which is what keeps a number handed out again from owning it.");

         Crl list = CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes);
         Assert.IsTrue(VerifiesWith(list, root));
         Assert.IsTrue(list.Revoked.Any(r => r.Serial == serial && r.Reason == 5), "On the CRL, reason cessationOfOperation.");

         Response trail = Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20certificate&action=updated&limit=20", null);
         StringAssert.Contains(carol + " serial " + serial, trail.Body, "The revocation is named by its address and serial.");
         StringAssert.Contains(carol + ": the certificates of a deleted account", trail.Body, "And the number taken off, by the address.");
      }

      [Test]
      [Description("Erasing an address revokes its valid certificate and takes the address and the certificate off the row, while the serial stays on the CRL; the address is not answered any more.")]
      public void ErasingAnAddressKeepsItsSerialOnTheCrl()
      {
         Assert.AreEqual(201, Admin("POST", AuthorityPath, "{}").Status);
         string carol = "carol@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, carol, Password);
         RSA key;
         Dictionary<string, object> answer = EnrolAndStore(carol, RsaRequest(2048, carol, out key), false);
         long serial = MiniJson.Number(answer, "serial");
         string lookup = "/api/v1/me/directory/" + Uri.EscapeDataString(carol) + "/smime";
         Assert.AreEqual(200, As(BobAddress, "GET", lookup, null).Status, "Answered before the erasure.");

         Response erased = Admin("POST", "/api/v1/addresses/erase", "{\"address\":\"" + carol + "\",\"include_archive\":false}");
         Assert.AreEqual(200, erased.Status, erased.Body);

         Dictionary<string, object> row = IssuedRow(Admin("GET", AuthorityPath, null).Body, serial);
         Assert.AreEqual("", MiniJson.Text(row, "address"), "The address is off the row.");
         Assert.AreEqual(0, MiniJson.Number(row, "account_id"));
         Assert.AreEqual("revoked", MiniJson.Text(row, "status"));
         Assert.AreEqual("cessationOfOperation", MiniJson.Text(row, "reason"));
         Assert.AreEqual(MiniJson.Text(answer, "fingerprint"), MiniJson.Text(row, "fingerprint"), "The fingerprint stays.");
         Assert.IsTrue(CrlOf(Public("/smime-ca/" + _domain.Name + ".crl").BodyBytes).Revoked.Any(r => r.Serial == serial && r.Reason == 5),
            "The serial stays on the CRL, for a certificate that is still out there.");
         Assert.AreEqual(404, As(BobAddress, "GET", lookup, null).Status);

         Response trail = Admin("GET", "/api/v1/audit?object_type=S%2FMIME%20certificate&action=updated&limit=20", null);
         StringAssert.Contains("serial " + serial + ", erased", trail.Body, "The erasure is named by the serial alone.");
      }

      [Test]
      [Description("The authority survives a configuration backup and a restore that deleted the domain: the same root, a key that opens, the next serial, every certificate with its account found again by address and a deleted account's left at 0, and the CRL distribution point inside a certificate issued before the restore answering a CRL signed by its root that lists what was revoked.")]
      public void TheAuthorityComesBackFromABackup()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"smime_authority_base_url\":\"http://pki.example.test\"}").Status);
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long caId = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         string fingerprint = MiniJson.Text(MiniJson.Object(created.Body), "authority.fingerprint");

         RSA key;
         Dictionary<string, object> first = EnrolAndStore(AdaAddress, RsaRequest(2048, AdaAddress, out key), false);
         long firstSerial = MiniJson.Number(first, "serial");
         X509Certificate2 firstCertificate = CertificateOf(MiniJson.Text(first, "certificate"));
         string carol = "carol@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, carol, Password);
         long carolSerial = MiniJson.Number(MiniJson.Object(Enrol(carol, RsaRequest(2048, carol, out key), false).Body), "serial");
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/accounts/" + carol, null).Status);
         long secondSerial = MiniJson.Number(EnrolAndStore(AdaAddress, RsaRequest(2048, AdaAddress, out key), true), "serial");
         Assert.AreEqual(200, Admin("DELETE", AuthorityPath + "/issued/" + firstSerial + "?reason=keyCompromise", null).Status);
         long nextSerial = MiniJson.Number(MiniJson.Object(Admin("GET", AuthorityPath, null).Body), "authority.next_serial");

         string backupDirectory = Paths.Combine(Path.GetTempPath(), TestSetup.UniqueString());
         Directory.CreateDirectory(backupDirectory);
         try
         {
            RunBackup(backupDirectory);
            DeleteAndRestore(backupDirectory);
         }
         finally
         {
            try { Directory.Delete(backupDirectory, true); } catch (IOException) { }
         }

         Dictionary<string, object> document = MiniJson.Object(Admin("GET", AuthorityPath, null).Body);
         Assert.AreEqual(fingerprint, MiniJson.Text(document, "authority.fingerprint"), "The same root.");
         Assert.IsTrue(MiniJson.Flag(document, "authority.key_opens"), "Its key opens on the machine that protected it.");
         Assert.AreEqual(nextSerial, MiniJson.Number(document, "authority.next_serial"), "The serial counter is carried.");

         string restored = Admin("GET", AuthorityPath, null).Body;
         Dictionary<string, object> firstRow = IssuedRow(restored, firstSerial);
         Assert.AreEqual("revoked", MiniJson.Text(firstRow, "status"));
         Assert.AreEqual("keyCompromise", MiniJson.Text(firstRow, "reason"));
         Dictionary<string, object> secondRow = IssuedRow(restored, secondSerial);
         Assert.AreEqual("valid", MiniJson.Text(secondRow, "status"));
         Assert.AreNotEqual(0, MiniJson.Number(secondRow, "account_id"), "Ada's certificate found her again by address.");
         Assert.AreEqual(0, MiniJson.Number(IssuedRow(restored, carolSerial), "account_id"), "A deleted account's stays at 0.");
         StringAssert.Contains("\"serial\":" + secondSerial, As(AdaAddress, "GET", "/api/v1/me/smime", null).Body, "And Ada's page lists it.");

         Response third = Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), true);
         Assert.AreEqual(201, third.Status, third.Body);
         Assert.AreEqual(nextSerial, MiniJson.Number(MiniJson.Object(third.Body), "serial"), "The next certificate takes the next serial.");

         // The distribution point a certificate issued before the restore carries.
         string points = Encoding.ASCII.GetString(firstCertificate.Extensions["2.5.29.31"].RawData);
         StringAssert.Contains("/smime-ca/" + fingerprint + ".crl", points);
         Crl list = CrlOf(Public("/smime-ca/" + fingerprint + ".crl").BodyBytes);
         Assert.IsTrue(VerifiesWith(list, RootOf(_domain.Name)), "Signed by the root it names.");
         Assert.IsTrue(list.Revoked.Any(r => r.Serial == firstSerial && r.Reason == 1), "Listing what was revoked before the backup.");
         Assert.IsTrue(list.Revoked.Any(r => r.Serial == carolSerial && r.Reason == 5));

         // The negative controls: a fingerprint no root has is a 404, and the row
         // number the root had before the restore - which is why a certificate
         // does not name it - names nothing now.
         Assert.AreEqual(404, Public("/smime-ca/" + new string('0', 64) + ".crl").Status);
         long newId = MiniJson.Number(document, "authority.id");
         if (newId != caId)
            Assert.AreEqual(404, Public("/smime-ca/" + _domain.Name + "/" + caId + ".crl").Status, "The old row number is gone.");
      }

      [Test]
      [Description("Deleting the domain deletes its authority and everything it issued (the foreign keys).")]
      public void DeletingTheDomainDeletesTheAuthority()
      {
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long caId = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         string fingerprint = MiniJson.Text(MiniJson.Object(created.Body), "authority.fingerprint");
         RSA key;
         Assert.AreEqual(201, Enrol(AdaAddress, RsaRequest(2048, AdaAddress, out key), false).Status);
         int domainId = _domain.ID;
         string name = _domain.Name;

         Assert.AreEqual(200, Admin("DELETE", "/api/v1/domains/" + name, null).Status);
         // By the fingerprint, which does not go through the domain: a 404 here
         // is the root's row gone, not the domain's name not found.
         Assert.AreEqual(404, Public("/smime-ca/" + fingerprint + ".pem").Status);

         int roots;
         int certificates;
         if (!TryCount("select count(*) from hm_smimeca where cadomainid = " + domainId, out roots) ||
             !TryCount("select count(*) from hm_smimeissued where issuedcaid = " + caId, out certificates))
         {
            Assert.Inconclusive("The row counts are read from the SQL CE test database; this server runs another, and the fingerprint's 404 above is all this proved.");
            return;
         }

         Assert.AreEqual(0, roots, "The domain's roots went with it.");
         Assert.AreEqual(0, certificates, "And what they issued went with them.");
      }

      [Test]
      [Description("The authority's routes are scoped to a credential's domains and refused to a read-only key's writes.")]
      public void TheAuthorityRoutesAreScopedToTheKeysDomains()
      {
         Response created = Admin("POST", AuthorityPath, "{}");
         Assert.AreEqual(201, created.Status, created.Body);
         long id = MiniJson.Number(MiniJson.Object(created.Body), "authority.id");
         SingletonProvider<TestSetup>.Instance.AddDomain("scoped-authority.test");

         string scopedId, scopedKey, readOnlyId, readOnlyKey;
         CreateKey("smimeca - scoped", "full", "scoped-authority.test", out scopedId, out scopedKey);
         CreateKey("smimeca - readonly", "readonly", null, out readOnlyId, out readOnlyKey);
         try
         {
            var routes = new[]
            {
               new[] { "GET", AuthorityPath, null },
               new[] { "POST", AuthorityPath, "{}" },
               new[] { "POST", AuthorityPath + "/rotate", "{}" },
               new[] { "DELETE", AuthorityPath + "/issued/1", null },
            };

            foreach (string[] route in routes)
            {
               Response refused = Bearer(route[0], route[1], scopedKey, route[2]);
               Assert.AreEqual(403, refused.Status, "A key for another domain reached " + route[0] + " " + route[1] + ": " + refused.Body);
            }
            Assert.AreEqual(200, Bearer("GET", "/api/v1/domains/scoped-authority.test/smime-ca", scopedKey, null).Status, "The key's own domain answers.");

            Assert.AreEqual(200, Bearer("GET", AuthorityPath, readOnlyKey, null).Status, "A read-only key may read the authority.");
            foreach (string[] route in routes.Skip(1))
            {
               Response refused = Bearer(route[0], route[1], readOnlyKey, route[2]);
               Assert.AreEqual(403, refused.Status, "A read-only key reached " + route[0] + " " + route[1] + ": " + refused.Body);
            }

            Assert.AreEqual(id, MiniJson.Number(MiniJson.Object(Admin("GET", AuthorityPath, null).Body), "authority.id"), "Nothing was replaced.");
         }
         finally
         {
            Admin("DELETE", "/api/v1/apikeys/" + scopedId, null);
            Admin("DELETE", "/api/v1/apikeys/" + readOnlyId, null);
         }
      }

      // ------------------------------------------------------------- requests

      private static byte[] RsaRequest(int bits, string email, out RSA key)
      {
         key = RSA.Create(bits);
         var request = new CertificateRequest("CN=Test, E=" + email, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
         request.CertificateExtensions.Add(EmailName(email));
         return request.CreateSigningRequest();
      }

      private static byte[] EcRequest(ECCurve curve, string email)
      {
         using (ECDsa ec = ECDsa.Create(curve))
         {
            var request = new CertificateRequest("CN=Test, E=" + email, ec, HashAlgorithmName.SHA256);
            request.CertificateExtensions.Add(EmailName(email));
            return request.CreateSigningRequest();
         }
      }

      private static X509Extension EmailName(string email)
      {
         var names = new SubjectAlternativeNameBuilder();
         names.AddEmailAddress(email);
         return names.Build();
      }

      private static string PemOf(byte[] der, string label)
      {
         string text = Convert.ToBase64String(der);
         var pem = new StringBuilder("-----BEGIN " + label + "-----\n");
         for (int i = 0; i < text.Length; i += 64)
            pem.Append(text.Substring(i, Math.Min(64, text.Length - i))).Append('\n');
         pem.Append("-----END " + label + "-----\n");
         return pem.ToString();
      }

      private static X509Certificate2 SelfSigned(string email)
      {
         using (RSA key = RSA.Create(2048))
         {
            var request = new CertificateRequest("CN=Stranger, E=" + email, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, true));
            request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection { new Oid(EmailProtection) }, false));
            request.CertificateExtensions.Add(EmailName(email));
            using (X509Certificate2 made = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(30)))
            {
               return new X509Certificate2(made.RawData);
            }
         }
      }

      private Response Enrol(string address, byte[] request, bool keep)
      {
         return As(address, "POST", "/api/v1/me/smime/enroll",
            "{\"csr\":\"" + Convert.ToBase64String(request) + "\",\"keep\":" + (keep ? "true" : "false") + "}");
      }

      /// <summary>
      ///    The certificate an enrolment answered, put in the account's own store as
      ///    the webmail puts it - with a wrapped key of the right shape, which the
      ///    server cannot open and does not try to - which is what confirms it.
      /// </summary>
      private Response StoreOwn(string address, Dictionary<string, object> issued)
      {
         string body = "{\"address\":\"" + MiniJson.Text(issued, "address") + "\",\"name\":\"Mine\",\"fingerprint\":\"" + MiniJson.Text(issued, "fingerprint") +
                       "\",\"certificate\":\"" + MiniJson.Text(issued, "certificate") + "\",\"chain\":[]," +
                       "\"key\":{\"v\":1,\"kdf\":\"PBKDF2-SHA256\",\"iterations\":600000,\"salt\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"iv\":\"AAAAAAAAAAAAAAAA\",\"data\":\"AQIDBA==\"}," +
                       "\"not_after\":" + MiniJson.Number(issued, "not_after") + "}";
         return As(address, "PUT", "/api/v1/me/smime/own", body);
      }

      private Dictionary<string, object> EnrolAndStore(string address, byte[] request, bool keep)
      {
         Response issued = Enrol(address, request, keep);
         Assert.AreEqual(201, issued.Status, issued.Body);
         Dictionary<string, object> answer = MiniJson.Object(issued.Body);
         Response stored = StoreOwn(address, answer);
         Assert.AreEqual(201, stored.Status, stored.Body);
         return answer;
      }

      private static Dictionary<string, object> IssuedRow(string listing, long serial)
      {
         Dictionary<string, object> row = MiniJson.List(MiniJson.Object(listing), "issued").Cast<Dictionary<string, object>>()
            .FirstOrDefault(r => MiniJson.Number(r, "serial") == serial);
         Assert.IsNotNull(row, "No certificate with serial " + serial + " in: " + listing);
         return row;
      }

      private static Dictionary<string, object> RetiredRoot(Dictionary<string, object> document, long id)
      {
         return MiniJson.List(document, "retired").Cast<Dictionary<string, object>>().FirstOrDefault(r => MiniJson.Number(r, "id") == id);
      }

      // ------------------------------------------------------------- certificates

      private X509Certificate2 RootOf(string domainName)
      {
         Response pem = Public("/smime-ca/" + domainName + ".pem");
         Assert.AreEqual(200, pem.Status, "The root is served: " + pem.Body);
         Assert.AreEqual("application/x-pem-file", pem.Header("Content-Type"));
         return new X509Certificate2(DerOfPem(pem.Body));
      }

      private static byte[] DerOfPem(string pem)
      {
         Match block = Regex.Match(pem, "-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", RegexOptions.Singleline);
         Assert.IsTrue(block.Success, "A PEM certificate: " + pem);
         return Convert.FromBase64String(Regex.Replace(block.Groups[1].Value, "\\s", ""));
      }

      private static X509Certificate2 CertificateOf(string base64)
      {
         Assert.IsFalse(string.IsNullOrEmpty(base64), "A certificate was answered.");
         return new X509Certificate2(Convert.FromBase64String(base64));
      }

      private static X509Chain NewChain()
      {
         var chain = new X509Chain();
         chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
         chain.ChainPolicy.VerificationFlags = X509VerificationFlags.AllowUnknownCertificateAuthority;
         chain.ChainPolicy.UrlRetrievalTimeout = TimeSpan.FromSeconds(1);
         return chain;
      }

      /// <summary>The certificate a chain built from the leaf ends at, the roots given as the only extra store.</summary>
      private static X509Certificate2 ChainEnd(X509Certificate2 leaf, params X509Certificate2[] roots)
      {
         using (X509Chain chain = NewChain())
         {
            foreach (X509Certificate2 root in roots)
               chain.ChainPolicy.ExtraStore.Add(root);

            Assert.IsTrue(chain.Build(leaf), "The chain builds: " + string.Join(", ", chain.ChainStatus.Select(s => s.Status + " " + s.StatusInformation)));
            Assert.IsTrue(chain.ChainStatus.All(s => s.Status == X509ChainStatusFlags.UntrustedRoot || s.Status == X509ChainStatusFlags.NoError),
               "Nothing is wrong with it but that this machine does not trust the root: " + string.Join(", ", chain.ChainStatus.Select(s => s.Status.ToString())));
            return chain.ChainElements[chain.ChainElements.Count - 1].Certificate;
         }
      }

      private static string EmailNameOf(X509Certificate2 certificate)
      {
         X509Extension names = certificate.Extensions["2.5.29.17"];
         Assert.IsNotNull(names, "A subjectAltName.");
         Der sequence = Der.Read(names.RawData, 0);
         Der email = sequence.Children().FirstOrDefault(n => n.Tag == 0x81);
         Assert.IsNotNull(email, "An rfc822Name.");
         return Encoding.ASCII.GetString(email.Content());
      }

      private static List<string> EnhancedUsagesOf(X509Certificate2 certificate)
      {
         var usages = certificate.Extensions["2.5.29.37"] as X509EnhancedKeyUsageExtension;
         var list = new List<string>();
         if (usages != null)
         {
            foreach (Oid oid in usages.EnhancedKeyUsages)
               list.Add(oid.Value);
         }
         return list;
      }

      private static string SubjectKeyIdOf(X509Certificate2 certificate)
      {
         var identifier = certificate.Extensions["2.5.29.14"] as X509SubjectKeyIdentifierExtension;
         Assert.IsNotNull(identifier, "A subject key identifier.");
         return identifier.SubjectKeyIdentifier.ToUpperInvariant();
      }

      // The keyIdentifier [0] of an authorityKeyIdentifier, as upper-case hex:
      // .NET Framework has no reader for the extension.
      private static string AuthorityKeyIdOf(X509Certificate2 certificate)
      {
         X509Extension extension = certificate.Extensions["2.5.29.35"];
         Assert.IsNotNull(extension, "An authority key identifier.");
         Der keyIdentifier = Der.Read(extension.RawData, 0).Children().FirstOrDefault(n => n.Tag == 0x80);
         Assert.IsNotNull(keyIdentifier, "A keyIdentifier in it.");
         return string.Concat(keyIdentifier.Content().Select(b => b.ToString("X2")));
      }

      private static string Sha256Hex(byte[] data)
      {
         using (SHA256 sha = SHA256.Create())
         {
            return string.Concat(sha.ComputeHash(data).Select(b => b.ToString("x2")));
         }
      }

      private static int IndexOf(byte[] haystack, byte[] needle)
      {
         for (int i = 0; i + needle.Length <= haystack.Length; i++)
         {
            bool match = true;
            for (int j = 0; j < needle.Length && match; j++)
               match = haystack[i + j] == needle[j];
            if (match)
               return i;
         }
         throw new AssertionException("The bytes were not found.");
      }

      // ------------------------------------------------------------- the CRL, by hand

      private sealed class Revocation
      {
         public long Serial;
         public int Reason;
      }

      private sealed class Crl
      {
         public byte[] Tbs;
         public byte[] Signature;
         public int Version;
         public DateTime ThisUpdate;
         public DateTime? NextUpdate;
         public readonly List<Revocation> Revoked = new List<Revocation>();
      }

      /// <summary>
      ///    A DER node: its tag, and where its content starts and ends in the bytes
      ///    it was read from. Enough of X.690 for a CRL and a subjectAltName: short
      ///    and long definite lengths, low tag numbers.
      /// </summary>
      private sealed class Der
      {
         public byte[] Source;
         public int Tag;
         public int Start;
         public int ContentStart;
         public int Length;

         public int End
         {
            get { return ContentStart + Length; }
         }

         public static Der Read(byte[] source, int offset)
         {
            var node = new Der { Source = source, Start = offset, Tag = source[offset] };
            int at = offset + 1;
            int length = source[at++];
            if ((length & 0x80) != 0)
            {
               int count = length & 0x7f;
               length = 0;
               for (int i = 0; i < count; i++)
                  length = length * 256 + source[at++];
            }
            node.ContentStart = at;
            node.Length = length;
            Assert.That(node.End, Is.LessThanOrEqualTo(source.Length), "The DER runs past its bytes.");
            return node;
         }

         public List<Der> Children()
         {
            var children = new List<Der>();
            int at = ContentStart;
            while (at < End)
            {
               Der child = Read(Source, at);
               children.Add(child);
               at = child.End;
            }
            return children;
         }

         public byte[] Content()
         {
            var bytes = new byte[Length];
            Array.Copy(Source, ContentStart, bytes, 0, Length);
            return bytes;
         }

         public byte[] Whole()
         {
            var bytes = new byte[End - Start];
            Array.Copy(Source, Start, bytes, 0, bytes.Length);
            return bytes;
         }
      }

      private static Crl CrlOf(byte[] der)
      {
         Assert.IsNotNull(der);
         Assert.That(der.Length, Is.GreaterThan(0), "A CRL was served.");

         List<Der> top = Der.Read(der, 0).Children();
         Assert.AreEqual(3, top.Count, "tbsCertList, signatureAlgorithm, signatureValue.");

         var crl = new Crl { Tbs = top[0].Whole() };
         byte[] bits = top[2].Content();
         crl.Signature = bits.Skip(1).ToArray();

         List<Der> fields = top[0].Children();
         int i = 0;
         crl.Version = fields[i].Tag == 0x02 ? (int) Number(fields[i++].Content()) : 0;
         i++; // signature algorithm
         i++; // issuer
         crl.ThisUpdate = TimeOf(fields[i++]);
         if (i < fields.Count && (fields[i].Tag == 0x17 || fields[i].Tag == 0x18))
            crl.NextUpdate = TimeOf(fields[i++]);
         if (i < fields.Count && fields[i].Tag == 0x30)
         {
            foreach (Der entry in fields[i].Children())
            {
               List<Der> parts = entry.Children();
               var revocation = new Revocation { Serial = Number(parts[0].Content()) };
               // crlEntryExtensions: the reason code is an ENUMERATED inside the
               // OCTET STRING of the extension 2.5.29.21.
               if (parts.Count > 2)
               {
                  foreach (Der extension in parts[2].Children())
                  {
                     List<Der> items = extension.Children();
                     if (OidOf(items[0].Content()) != "2.5.29.21")
                        continue;
                     Der value = Der.Read(items[items.Count - 1].Content(), 0);
                     revocation.Reason = (int) Number(value.Content());
                  }
               }
               crl.Revoked.Add(revocation);
            }
         }

         return crl;
      }

      /// <summary>
      ///    Whether the CRL's signature verifies with the root's key: ECDSA with the
      ///    signature's DER SEQUENCE of r and s turned into the r||s .NET Framework's
      ///    VerifyData takes, or RSA PKCS#1 v1.5 as it stands.
      /// </summary>
      private static bool VerifiesWith(Crl crl, X509Certificate2 root)
      {
         using (ECDsa ec = root.GetECDsaPublicKey())
         {
            if (ec != null)
            {
               List<Der> pair = Der.Read(crl.Signature, 0).Children();
               int size = (ec.KeySize + 7) / 8;
               byte[] raw = new byte[size * 2];
               Fixed(pair[0].Content(), size).CopyTo(raw, 0);
               Fixed(pair[1].Content(), size).CopyTo(raw, size);
               return ec.VerifyData(crl.Tbs, raw, HashAlgorithmName.SHA256);
            }
         }

         using (RSA rsa = root.GetRSAPublicKey())
         {
            return rsa != null && rsa.VerifyData(crl.Tbs, crl.Signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
         }
      }

      // An INTEGER's content to exactly size bytes: the sign byte dropped, zeros put in front.
      private static byte[] Fixed(byte[] integer, int size)
      {
         byte[] trimmed = integer.SkipWhile((b, index) => b == 0 && index < integer.Length - 1).ToArray();
         Assert.That(trimmed.Length, Is.LessThanOrEqualTo(size), "An ECDSA half no larger than the curve.");
         var fixedBytes = new byte[size];
         Array.Copy(trimmed, 0, fixedBytes, size - trimmed.Length, trimmed.Length);
         return fixedBytes;
      }

      private static long Number(byte[] content)
      {
         long value = 0;
         foreach (byte b in content)
            value = value * 256 + b;
         return value;
      }

      private static DateTime TimeOf(Der node)
      {
         string text = System.Text.Encoding.ASCII.GetString(node.Content());
         string format = node.Tag == 0x17 ? "yyMMddHHmmss'Z'" : "yyyyMMddHHmmss'Z'";
         return DateTime.ParseExact(text, format, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal);
      }

      private static string OidOf(byte[] content)
      {
         var parts = new List<long> { content[0] / 40, content[0] % 40 };
         long value = 0;
         for (int i = 1; i < content.Length; i++)
         {
            value = value * 128 + (content[i] & 0x7f);
            if ((content[i] & 0x80) == 0)
            {
               parts.Add(value);
               value = 0;
            }
         }
         return string.Join(".", parts);
      }

      // ------------------------------------------------------------- the database, for the few probes

      /// <summary>The ini's Type is MSSQLCE: the test database this fixture's probes can reach.</summary>
      private static bool IsCeDatabase()
      {
         string ini = IniFileSetting.ExistingIniFiles().FirstOrDefault();
         if (ini == null)
            return false;
         string type = Regex.Match(File.ReadAllText(ini), @"(?m)^\s*Type\s*=\s*(\S+)").Groups[1].Value;
         return string.Equals(type, "MSSQLCE", StringComparison.OrdinalIgnoreCase);
      }

      /// <summary>
      ///    An open ADODB connection to the SQL CE test database, as
      ///    Infrastructure/ReferentialIntegrity.cs reads it, or null when the server
      ///    runs another database, which these probes cannot reach.
      /// </summary>
      private static object OpenCe(out Type connectionType)
      {
         connectionType = null;
         if (!IsCeDatabase())
            return null;

         string text = File.ReadAllText(IniFileSetting.ExistingIniFiles().First());
         string folder = Regex.Match(text, @"(?m)^\s*DatabaseFolder\s*=\s*(.+?)\s*$").Groups[1].Value;
         string database = Regex.Match(text, @"(?m)^\s*Database\s*=\s*(.+?)\s*$").Groups[1].Value;
         string encoded = Regex.Match(text, @"(?m)^\s*Password\s*=\s*(.+?)\s*$").Groups[1].Value;
         string password = System.Text.Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(encoded), null, DataProtectionScope.LocalMachine));
         string connectionString = "Provider=Microsoft.SQLSERVER.CE.OLEDB.4.0;Data Source=" + Paths.Combine(folder, database + ".sdf") +
                                   ";SSCE:Database Password=" + password;

         connectionType = Type.GetTypeFromProgID("ADODB.Connection", true);
         object connection = Activator.CreateInstance(connectionType);
         connectionType.InvokeMember("Open", BindingFlags.InvokeMethod, null, connection, new object[] { connectionString });
         return connection;
      }

      /// <summary>The first column of the first row of a query, or false when the probe cannot reach the database.</summary>
      private static bool TryScalar(string sql, out object value)
      {
         value = null;
         Type connectionType;
         object connection = OpenCe(out connectionType);
         if (connection == null)
            return false;

         try
         {
            object recordset = connectionType.InvokeMember("Execute", BindingFlags.InvokeMethod, null, connection, new object[] { sql });
            Type recordsetType = recordset.GetType();
            object fields = recordsetType.InvokeMember("Fields", BindingFlags.GetProperty, null, recordset, null);
            object field = fields.GetType().InvokeMember("Item", BindingFlags.GetProperty, null, fields, new object[] { 0 });
            value = field.GetType().InvokeMember("Value", BindingFlags.GetProperty, null, field, null);
            recordsetType.InvokeMember("Close", BindingFlags.InvokeMethod, null, recordset, null);
            return true;
         }
         finally
         {
            connectionType.InvokeMember("Close", BindingFlags.InvokeMethod, null, connection, null);
         }
      }

      private static bool TryCount(string sql, out int count)
      {
         count = -1;
         object value;
         if (!TryScalar(sql, out value))
            return false;
         count = Convert.ToInt32(value);
         return true;
      }

      /// <summary>A statement that changes rows under the server, or false when the probe cannot reach the database.</summary>
      private static bool TryExecute(string sql)
      {
         Type connectionType;
         object connection = OpenCe(out connectionType);
         if (connection == null)
            return false;

         try
         {
            connectionType.InvokeMember("Execute", BindingFlags.InvokeMethod, null, connection, new object[] { sql });
            return true;
         }
         finally
         {
            connectionType.InvokeMember("Close", BindingFlags.InvokeMethod, null, connection, null);
         }
      }

      // ------------------------------------------------------------- the backup

      private void RunBackup(string directory)
      {
         var settings = _application.Settings.Backup;
         settings.BackupDomains = true;
         settings.BackupMessages = false;
         settings.BackupSettings = false;
         settings.CompressDestinationFiles = true;
         settings.Destination = directory;

         CustomAsserts.AssertDeleteFile(settings.LogFile);

         _application.BackupManager.StartBackup();

         for (int attempt = 0; attempt < 120; attempt++)
         {
            try
            {
               string log = TestSetup.ReadExistingTextFile(settings.LogFile);

               if (log.Contains("BACKUP ERROR:"))
                  Assert.Fail("The backup failed. Log: " + log);

               if (log.Contains("Backup completed successfully"))
                  return;
            }
            catch (Exception sharing) when (!ExceptionPolicy.IsFatal(sharing))
            {
               // The server is writing the file; read it again in a moment.
            }

            Thread.Sleep(250);
         }

         Assert.Fail("The backup did not finish within 30 seconds.");
      }

      private void DeleteAndRestore(string directory)
      {
         // The disaster a restore assumes: every domain gone, and the authority
         // with its domain.
         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         string startTime = _application.Status.StartTime;

         // Status.StartTime has one-second resolution; step past the second.
         Thread.Sleep(1100);

         FileInfo[] archives = new DirectoryInfo(directory).GetFiles("*.7z");
         Assert.AreEqual(1, archives.Length, "Expected exactly one archive in " + directory);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = true;
         backup.RestoreMessages = false;
         backup.RestoreSettings = false;
         backup.StartRestore();

         for (int attempt = 0; attempt < 600; attempt++)
         {
            try
            {
               string now = _application.Status.StartTime;
               if (now.Length > 0 && now != startTime)
               {
                  // The listeners come back with the reinitialisation.
                  Thread.Sleep(500);
                  return;
               }
            }
            catch (Exception talking) when (!ExceptionPolicy.IsFatal(talking))
            {
               // The COM server is reinitialising.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not finish within a minute.");
      }

      // ------------------------------------------------------------- HTTP

      private Response Admin(string method, string path, string body)
      {
         return Raw(RestPort, method, path, Auth("Administrator", AdminPassword), body, "application/json", null);
      }

      private Response As(string address, string method, string path, string body)
      {
         return Raw(RestPort, method, path, Auth(address, Password), body, "application/json", null);
      }

      private Response Bearer(string method, string path, string token, string body)
      {
         return Raw(RestPort, method, path, "Bearer " + token, body, "application/json", null);
      }

      private void CreateKey(string label, string scope, string domains, out string id, out string key)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";
         Response created = Admin("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, created.Status, "POST /api/v1/apikeys must create a key: " + created.Body);
         id = Regex.Match(created.Body, "\"id\"\\s*:\\s*\"([^\"]*)\"").Groups[1].Value;
         key = Regex.Match(created.Body, "\"key\"\\s*:\\s*\"([^\"]*)\"").Groups[1].Value;
         Assert.IsFalse(string.IsNullOrEmpty(key), created.Body);
      }

      /// <summary>The web services listener: plain HTTP, no credential, as a mail client fetches a CRL.</summary>
      private Response Public(string path)
      {
         return Raw(WebServicesPort, "GET", path, null, null, null, null);
      }

      private static string Auth(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public byte[] BodyBytes = new byte[0];
         public string HeaderText = "";
         public readonly Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : null;
         }
      }

      private static Response Raw(int port, string method, string path, string authorization, string body, string contentType, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", port);
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
            {
               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1:" + port + "\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (extraHeaders != null)
                  head.Append(extraHeaders);

               byte[] bodyBytes = body == null ? new byte[0] : System.Text.Encoding.UTF8.GetBytes(body);
               if (body != null)
               {
                  head.Append("Content-Type: " + contentType + "\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = System.Text.Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               var memory = new MemoryStream();
               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               byte[] raw = memory.ToArray();
               var response = new Response();

               // The head is ASCII; the body may be DER, so it is kept as bytes too.
               int separator = -1;
               for (int i = 0; i + 3 < raw.Length; i++)
               {
                  if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n')
                  {
                     separator = i;
                     break;
                  }
               }

               response.HeaderText = System.Text.Encoding.ASCII.GetString(raw, 0, separator >= 0 ? separator : raw.Length);
               if (separator >= 0)
               {
                  response.BodyBytes = raw.Skip(separator + 4).ToArray();
                  response.Body = System.Text.Encoding.UTF8.GetString(response.BodyBytes);
               }

               string[] lines = response.HeaderText.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out response.Status);
               }

               for (int i = 1; i < lines.Length; i++)
               {
                  int colon = lines[i].IndexOf(':');
                  if (colon > 0)
                     response.Headers[lines[i].Substring(0, colon).Trim()] = lines[i].Substring(colon + 1).Trim();
               }

               return response;
            }
         }
      }
   }
}
