// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SSL
{
   /// <summary>
   ///    An HTTPS endpoint the server is given by its ADDRESS - https://127.0.0.1:port/,
   ///    https://[::1]:port/ - and whose certificate names that address the way RFC 5280
   ///    and every public CA name one: as an iPAddress entry of the subjectAltName, and
   ///    nowhere else. The object store, a webhook, an identity provider and the update
   ///    feed are all reached through HttpsClient and CertificateVerifier; the update
   ///    check is the one of them a test can run and read back in one call
   ///    (Status.CheckForUpdate), so it is the client here.
   ///
   ///    The row (docs/DatabaseTls.md, "Certificates for an address") found the Linux
   ///    branch of CertificateVerifier matching the host with X509_check_host alone,
   ///    which never reads an iPAddress entry, and fixed it with X509_check_ip_asc. On
   ///    Windows the check is CryptoAPI's SSL policy, which was measured on 25 September
   ///    2026 to read iPAddress entries already - so on this bench these tests pass
   ///    before the change and after it, and are the guard that it stays so. The
   ///    negative control is the Linux half's: CertificateVerifierTester, which the
   ///    internal suite runs, calls the POSIX matcher on both platforms. The Linux proof
   ///    end to end is the lab run in the row's commit message.
   ///
   ///    The certificates are made here, self-signed, one per address, with the
   ///    server-authentication usage and no CA flag, and are trusted for the length of
   ///    the fixture by putting their public halves in the computer's Trusted Root
   ///    store - which is what the service's CryptoAPI chain reads, and what makes a
   ///    refusal in the negative tests the NAME's and nothing else's. The private keys
   ///    exist only in this process and are gone with it; a certificate left in the
   ///    store by an aborted run can sign nothing, and the next run removes it by its
   ///    subject. That needs an elevated run, and the fixture is skipped, saying so,
   ///    without one.
   ///
   ///    Ports 10290 to 10293, the row's.
   /// </summary>
   [TestFixture]
   public class HttpsAddressCertificates : TestFixtureBase
   {
      private const string SubjectPrefix = "CN=hMailServer regression address certificate";

      // CERT_E_CN_NO_MATCH, as CertificateVerifier's debug line prints a Windows
      // error code: the certificate is trusted and is not for this host.
      private const string NameMismatch = "-2146762481";

      private readonly List<X509Certificate2> _trusted = new List<X509Certificate2>();

      // Whether this run may write the computer's root store; the teardown runs after
      // an ignored setup too, and must not try then.
      private bool _elevated;

      [OneTimeSetUp]
      public void TrustNothingLeftOver()
      {
         _elevated = new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
         if (!_elevated)
            Assert.Ignore("The certificates are trusted by putting them in the computer's Trusted Root store, which needs an elevated test run.");

         RemoveFromRootStore();
      }

      [OneTimeTearDown]
      public void TakeTheCertificatesBack()
      {
         if (_elevated)
            RemoveFromRootStore();

         foreach (var certificate in _trusted)
            certificate.Dispose();
         _trusted.Clear();
      }

      [SetUp]
      public void NoScheduledCheck()
      {
         IniFileSetting.Write("UpdateCheckEnabled", "0");
         IniFileSetting.Write("UpdateChannel", "stable");
      }

      [TearDown]
      public void PutTheFeedBack()
      {
         IniFileSetting.Write("UpdateFeedUrl", "");
         _application.Reinitialize();
      }

      [Test]
      [Description("An endpoint named by an IPv4 address, whose certificate carries the address as an iPAddress entry and nowhere else, is accepted: the update feed is read from it.")]
      public void AnAddressNamedOnlyAsAnIpAddressEntryIsAccepted()
      {
         using (var certificate = AddressCertificate(ipAddress: IPAddress.Loopback))
         using (var endpoint = new TlsResponder(IPAddress.Loopback, 10290, certificate, OlderRelease()))
         {
            UseFeed("https://127.0.0.1:10290/releases/latest");

            var status = _application.Status;
            bool read = RetryUntilTrue(() => status.CheckForUpdate());

            Assert.IsTrue(read, "The feed at 127.0.0.1, whose certificate names 127.0.0.1 as an iPAddress entry, was refused: " + status.UpdateLastError);
            Assert.AreEqual(1, status.UpdateState, "The older release the feed served is not an update. Error: " + status.UpdateLastError);
            ClassicAssert.GreaterOrEqual(endpoint.Requests.Count, 1, "The request crossed the TLS session.");
            StringAssert.StartsWith("GET /releases/latest ", endpoint.Requests[endpoint.Requests.Count - 1]);
         }
      }

      [Test]
      [Description("The same for an IPv6 address, written in a URL's brackets.")]
      public void AnIpv6AddressNamedOnlyAsAnIpAddressEntryIsAccepted()
      {
         if (!Socket.OSSupportsIPv6)
            Assert.Ignore("IPv6 is not available on this machine.");

         using (var certificate = AddressCertificate(ipAddress: IPAddress.IPv6Loopback))
         {
            TlsResponder endpoint;
            try
            {
               endpoint = new TlsResponder(IPAddress.IPv6Loopback, 10292, certificate, OlderRelease());
            }
            catch (SocketException ex)
            {
               Assert.Ignore("Nothing can listen on [::1]:10292 here: " + ex.Message);
               return;
            }

            using (endpoint)
            {
               UseFeed("https://[::1]:10292/releases/latest");

               var status = _application.Status;
               bool read = RetryUntilTrue(() => status.CheckForUpdate());

               Assert.IsTrue(read, "The feed at [::1], whose certificate names ::1 as an iPAddress entry, was refused: " + status.UpdateLastError);
               ClassicAssert.GreaterOrEqual(endpoint.Requests.Count, 1);
            }
         }
      }

      [Test]
      [Description("The negative control: a trusted certificate for another address is refused at the handshake, for its name, and nothing is asked of the endpoint.")]
      public void ACertificateForAnotherAddressIsRefused()
      {
         // Trusted, and valid in every way but the one under test: it names
         // 127.0.0.2, and the feed is at 127.0.0.1.
         using (var certificate = AddressCertificate(ipAddress: IPAddress.Parse("127.0.0.2")))
         using (var endpoint = new TlsResponder(IPAddress.Loopback, 10291, certificate, OlderRelease()))
         {
            UseFeed("https://127.0.0.1:10291/releases/latest");

            var status = _application.Status;
            string log = RefusedForTheName(status, "A certificate for 127.0.0.2 was accepted for 127.0.0.1.");

            StringAssert.Contains("could not be read", status.UpdateLastError);
            Assert.AreEqual(0, endpoint.Requests.Count, "No request may cross a session whose certificate was refused.");
            StringAssert.Contains("Expected host: 127.0.0.1", log, "CertificateVerifier names the host it expected.");
         }
      }

      [Test]
      [Description("A certificate that names the host only as a DNS name is not taken for an address: localhost is not 127.0.0.1 to the check.")]
      public void ANameIsNotAnAddress()
      {
         using (var certificate = AddressCertificate(dnsName: "localhost"))
         using (var endpoint = new TlsResponder(IPAddress.Loopback, 10293, certificate, OlderRelease()))
         {
            UseFeed("https://127.0.0.1:10293/releases/latest");

            var status = _application.Status;
            RefusedForTheName(status, "A certificate for the name localhost was accepted for the address 127.0.0.1.");
            Assert.AreEqual(0, endpoint.Requests.Count);
         }
      }

      // ---- plumbing ------------------------------------------------------------------

      private void UseFeed(string url)
      {
         IniFileSetting.Write("UpdateFeedUrl", url);
         _application.Reinitialize();
      }

      // The service's chain engine reads the root store when it next builds a chain,
      // which on a busy machine can be a moment after the store changed: a success is
      // waited for.
      private static bool RetryUntilTrue(Func<bool> attempt)
      {
         for (int tries = 0; tries < 10; tries++)
         {
            if (attempt())
               return true;
            Thread.Sleep(1000);
         }

         return false;
      }

      // Every check refused, and the refusal the name's (CERT_E_CN_NO_MATCH in
      // CertificateVerifier's debug line) and not the chain's: until the service's
      // chain engine has read the root store, the same certificate is refused as
      // untrusted, which is not what these tests are about - so that answer is
      // waited out, as a success is above. The log it found it in is returned.
      private static string RefusedForTheName(hMailServer.Status status, string acceptedMessage)
      {
         string log = "";
         for (int tries = 0; tries < 10; tries++)
         {
            Assert.IsFalse(status.CheckForUpdate(), acceptedMessage);

            log = LogHandler.ReadCurrentDefaultLog();
            if (log.Contains(NameMismatch))
               return log;

            Thread.Sleep(1000);
         }

         Assert.Fail("Every refusal was for something other than the certificate's name (CERT_E_CN_NO_MATCH, " + NameMismatch + "). Log:\r\n\r\n" + log);
         return log;
      }

      // A self-signed server certificate for one address or one name, trusted until
      // the fixture ends. The subject says what it is and never names the address,
      // so that the common name cannot be what matches.
      private X509Certificate2 AddressCertificate(IPAddress ipAddress = null, string dnsName = null)
      {
         using (var key = new RSACng(2048))
         {
            var request = new CertificateRequest(SubjectPrefix + " " + Guid.NewGuid().ToString("N").Substring(0, 8),
               key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
            request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, true));
            request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection { new Oid("1.3.6.1.5.5.7.3.1") }, false));

            var names = new SubjectAlternativeNameBuilder();
            if (ipAddress != null)
               names.AddIpAddress(ipAddress);
            if (dnsName != null)
               names.AddDnsName(dnsName);
            request.CertificateExtensions.Add(names.Build());

            using (var made = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(2)))
            {
               // SChannel cannot serve the ephemeral key CertificateRequest attaches;
               // exported and read back, the key is in a user container that goes when
               // the certificate is disposed (no PersistKeySet).
               byte[] pfx = made.Export(X509ContentType.Pfx, "regression");
               var usable = new X509Certificate2(pfx, "regression", X509KeyStorageFlags.Exportable | X509KeyStorageFlags.UserKeySet);

               var store = new X509Store(StoreName.Root, StoreLocation.LocalMachine);
               store.Open(OpenFlags.ReadWrite);
               try
               {
                  store.Add(new X509Certificate2(usable.RawData));
               }
               finally
               {
                  store.Close();
               }

               _trusted.Add(new X509Certificate2(usable.RawData));
               return usable;
            }
         }
      }

      // Every certificate this fixture ever put in the store, found by its subject.
      private static void RemoveFromRootStore()
      {
         var store = new X509Store(StoreName.Root, StoreLocation.LocalMachine);
         store.Open(OpenFlags.ReadWrite);
         try
         {
            var ours = new List<X509Certificate2>();
            foreach (var certificate in store.Certificates)
            {
               if (certificate.Subject.StartsWith(SubjectPrefix, StringComparison.Ordinal))
                  ours.Add(certificate);
            }

            foreach (var certificate in ours)
               store.Remove(certificate);
         }
         finally
         {
            store.Close();
         }
      }

      // A release older than any this server could be, so that a feed that was read
      // is "this is the latest release" and nothing is offered.
      private static string OlderRelease()
      {
         return "{\"tag_name\":\"v1.0.0\",\"name\":\"hMailServer 1.0.0\",\"draft\":false,\"prerelease\":false," +
                "\"published_at\":\"2016-01-01T00:00:00Z\",\"html_url\":\"https://example.test/releases/v1.0.0\",\"assets\":[]}";
      }

      /// <summary>
      ///    One HTTPS answer, the same to every request: a TLS listener on one address
      ///    and port with one certificate, which records the request line of every
      ///    request that arrived over a session the client accepted. A client that
      ///    refuses the certificate ends the handshake, and nothing is recorded.
      /// </summary>
      private sealed class TlsResponder : IDisposable
      {
         private readonly TcpListener _listener;
         private readonly X509Certificate2 _certificate;
         private readonly byte[] _answer;
         private readonly List<string> _requests = new List<string>();
         private readonly Thread _thread;
         private volatile bool _stopping;

         public TlsResponder(IPAddress address, int port, X509Certificate2 certificate, string json)
         {
            _certificate = certificate;

            byte[] body = Encoding.UTF8.GetBytes(json);
            byte[] head = Encoding.ASCII.GetBytes("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + body.Length + "\r\nConnection: close\r\n\r\n");
            _answer = new byte[head.Length + body.Length];
            Buffer.BlockCopy(head, 0, _answer, 0, head.Length);
            Buffer.BlockCopy(body, 0, _answer, head.Length, body.Length);

            _listener = new TcpListener(address, port);
            _listener.Start();

            _thread = new Thread(Serve) { IsBackground = true };
            _thread.Start();
         }

         public List<string> Requests
         {
            get
            {
               lock (_requests)
                  return new List<string>(_requests);
            }
         }

         private void Serve()
         {
            while (!_stopping)
            {
               TcpClient client;
               try
               {
                  client = _listener.AcceptTcpClient();
               }
               catch (Exception)
               {
                  return;
               }

               using (client)
               {
                  try
                  {
                     client.ReceiveTimeout = 10000;
                     client.SendTimeout = 10000;

                     using (var tls = new SslStream(client.GetStream(), false))
                     {
                        tls.AuthenticateAsServer(_certificate, false, SslProtocols.Tls12, false);

                        var request = new MemoryStream();
                        var buffer = new byte[4096];
                        while (true)
                        {
                           int read = tls.Read(buffer, 0, buffer.Length);
                           if (read <= 0)
                              break;
                           request.Write(buffer, 0, read);
                           if (Encoding.ASCII.GetString(request.ToArray()).Contains("\r\n\r\n"))
                              break;
                        }

                        string text = Encoding.ASCII.GetString(request.ToArray());
                        int endOfLine = text.IndexOf("\r\n", StringComparison.Ordinal);
                        lock (_requests)
                           _requests.Add(endOfLine >= 0 ? text.Substring(0, endOfLine) : text);

                        tls.Write(_answer, 0, _answer.Length);
                        tls.Flush();
                     }
                  }
                  catch (Exception)
                  {
                     // A handshake the client ended - the refusal the negative tests
                     // are about - or a client that went away. Nothing is recorded.
                  }
               }
            }
         }

         public void Dispose()
         {
            _stopping = true;
            _listener.Stop();
            _thread.Join(5000);
         }
      }
   }
}
