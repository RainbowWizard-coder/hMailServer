// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.SSL
{
   [TestFixture]
   public class SslTlsVersionTests : TestFixtureBase
   {
      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
      }

      private Account _account;

      private void SetSslVersions(bool tlsv10, bool tlsv11, bool tlsv12, bool tlsv13)
      {
         SslSetup.SetupSSLPorts(_application, new SslVersions
         {
            Tls10 = tlsv10,
            Tls11 = tlsv11,
            Tls12 = tlsv12,
            Tls13 = tlsv13
         });

         Thread.Sleep(1000);
      }

      [Test]
      public void ItShouldBePossibleToDisableSslV3()
      {
         SetSslVersions(true, true, true, true);
         var smtpClientSimulator =
            new SmtpClientSimulator(true, SslProtocols.Ssl3, 25001, IPAddress.Parse("127.0.0.1"));

         try
         {
            string errorMessage;
            smtpClientSimulator.Send(false, _account.Address, "test", _account.Address, _account.Address, "Test",
               "test",
               out errorMessage);

            Assert.Fail("Was able to establish SSLv3 connection");
         }
         catch (AuthenticationException)
         {
            // on windows 10
         }
         catch (Win32Exception)
         {
            // on newer windows 10
         }
         catch (IOException)
         {
            // on windows xp
         }
      }

      [Test]
      public void WhenSSL3IsDisabledTLSShouldWork()
      {
         SetSslVersions(true, true, true, true);
         var smtpClientSimulator = new SmtpClientSimulator(true, SslProtocols.Tls12, 25001, IPAddress.Parse("127.0.0.1"));

         string errorMessage;
         smtpClientSimulator.Send(false, _account.Address, "test", _account.Address, _account.Address, "Test", "test",
            out errorMessage);

         var message = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, "test");
         Assert.IsTrue(message.Contains("version=TLSv1"), message);
      }

      [Test]
      public void Tls12ShouldWork()
      {
         SetSslVersions(false, false, true, false);
         var smtpClientSimulator = new SmtpClientSimulator(true, SslProtocols.Tls12, 25001, IPAddress.Parse("127.0.0.1"));

         string errorMessage;
         smtpClientSimulator.Send(false, _account.Address, "test", _account.Address, _account.Address, "Test", "test",
            out errorMessage);

         var message = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, "test");
         Assert.IsTrue(message.Contains("version=TLSv1.2"), message);
      }

      [Test]
      public void WhenTls12IsDisabled_ConnectionWithTls12ShouldFail()
      {
         SetSslVersions(false, false, false, true);
         var smtpClientSimulator = new SmtpClientSimulator(true, SslProtocols.Tls12, 25001, IPAddress.Parse("127.0.0.1"));

         try
         {
            string errorMessage;
            smtpClientSimulator.Send(false, _account.Address, "test", _account.Address, _account.Address, "Test",
               "test",
               out errorMessage);

            Assert.Fail("Was able to establish TLS 1.2 connection when TLS 1.2 was disabled");
         }
         catch (AuthenticationException)
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
         catch (Win32Exception)
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
         catch (IOException)
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      [Test]
      [NUnit.Framework.Description("A TLS 1.2 listener offers the uncompressed elliptic-curve point format alone: until 6.3.6 every option from bit 32 up was set on its context, SSL_OP_LEGACY_EC_POINT_FORMATS among them, and it offered the two compressed formats RFC 8422 deprecates as well")]
      public void Tls12ListenerOffersOnlyTheUncompressedEcPointFormat()
      {
         string why = WhyTheServersOpenSslCannotTell();
         if (why != null)
            Assert.Ignore(why);

         SetSslVersions(false, false, true, true);

         byte[] offered = OfferedEcPointFormats(25001);

         // The extension's data: a one-byte length, then the formats - 0 uncompressed,
         // 1 ansiX962_compressed_prime, 2 ansiX962_compressed_char2.
         Assert.AreEqual(new byte[] { 0x01, 0x00 }, offered,
            "The listener's ServerHello offered the EC point formats " + BitConverter.ToString(offered) +
            "; with only the options the server asks for it offers the uncompressed format alone (01-00). " +
            "03-00-01-02 is what every option from bit 32 up being set gives, SSL_OP_LEGACY_EC_POINT_FORMATS among them.");
      }

      /// <summary>
      ///    Why this server's OpenSSL cannot show the difference, or null when it can. OpenSSL
      ///    offers only the uncompressed format by default since 3.6, and the option that
      ///    offers the compressed ones too came with that; before 3.6 every server offers all
      ///    three. The Windows server links OpenSSL 4.0. The Linux server links the
      ///    distribution's, which is the one `openssl version` names on the machine it runs on.
      /// </summary>
      private static string WhyTheServersOpenSslCannotTell()
      {
         if (Environment.OSVersion.Platform == PlatformID.Win32NT)
            return null;

         string said;
         try
         {
            var start = new ProcessStartInfo("openssl", "version")
            {
               UseShellExecute = false,
               RedirectStandardOutput = true,
               CreateNoWindow = true
            };

            using (Process process = Process.Start(start))
            {
               said = process.StandardOutput.ReadToEnd();
               process.WaitForExit();
            }
         }
         catch (Exception e)
         {
            return "There is no openssl program to name the OpenSSL the server links (" + e.Message + ").";
         }

         Match version = Regex.Match(said, @"^OpenSSL (\d+)\.(\d+)");
         if (!version.Success)
            return "openssl version answered \"" + said.Trim() + "\", which names no OpenSSL version.";

         int major = int.Parse(version.Groups[1].Value);
         int minor = int.Parse(version.Groups[2].Value);
         if (major > 3 || (major == 3 && minor >= 6))
            return null;

         return "This server's OpenSSL is " + major + "." + minor + ", which offers every EC point format whatever the options say; 3.6 and later offer the uncompressed one alone unless asked.";
      }

      private static void AddUInt16(List<byte> target, int value)
      {
         target.Add((byte) (value >> 8));
         target.Add((byte) value);
      }

      private static void AddExtension(List<byte> target, int extensionType, byte[] data)
      {
         AddUInt16(target, extensionType);
         AddUInt16(target, data.Length);
         target.AddRange(data);
      }

      private static byte[] ReadExactly(Stream stream, int count)
      {
         var buffer = new byte[count];
         int read = 0;

         while (read < count)
         {
            int thisRead = stream.Read(buffer, read, count - read);
            if (thisRead <= 0)
               Assert.Fail("The server closed the connection after " + read + " of " + count + " expected bytes.");

            read += thisRead;
         }

         return buffer;
      }

      /// <summary>
      ///    Speaks TLS 1.2 by hand to an implicit-TLS listener - SslStream does not show the
      ///    extensions - with a ClientHello that offers ECDHE suites alone and all three EC
      ///    point formats, and answers the data of the ec_point_formats extension of the
      ///    ServerHello: the server's own list, whatever the client offered.
      /// </summary>
      private static byte[] OfferedEcPointFormats(int port)
      {
         var extensions = new List<byte>();

         // supported_groups: x25519, secp256r1, secp384r1.
         AddExtension(extensions, 0x000A, new byte[] { 0x00, 0x06, 0x00, 0x1D, 0x00, 0x17, 0x00, 0x18 });
         // ec_point_formats: uncompressed, ansiX962_compressed_prime, ansiX962_compressed_char2.
         AddExtension(extensions, 0x000B, new byte[] { 0x03, 0x00, 0x01, 0x02 });
         // signature_algorithms.
         AddExtension(extensions, 0x000D, new byte[]
         {
            0x00, 0x0A,
            0x04, 0x03, // ecdsa_secp256r1_sha256
            0x08, 0x04, // rsa_pss_rsae_sha256
            0x04, 0x01, // rsa_pkcs1_sha256
            0x05, 0x03, // ecdsa_secp384r1_sha384
            0x08, 0x05  // rsa_pss_rsae_sha384
         });
         // renegotiation_info, empty: a first handshake.
         AddExtension(extensions, 0xFF01, new byte[] { 0x00 });

         var body = new List<byte>();
         body.Add(0x03);
         body.Add(0x03); // client_version: TLS 1.2, and no supported_versions, so no TLS 1.3

         var random = new byte[32];
         new Random(20260930).NextBytes(random);
         body.AddRange(random);

         body.Add(0x00); // no session id

         // ECDHE suites only, so that the server's answer is an ECC one and carries the extension.
         byte[] suites = { 0xC0, 0x2F, 0xC0, 0x30, 0xC0, 0x2B, 0xC0, 0x2C, 0xCC, 0xA8, 0xCC, 0xA9 };
         AddUInt16(body, suites.Length);
         body.AddRange(suites);

         body.Add(0x01);
         body.Add(0x00); // compression: null only

         AddUInt16(body, extensions.Count);
         body.AddRange(extensions);

         var handshake = new List<byte>();
         handshake.Add(0x01); // client_hello
         handshake.Add((byte) (body.Count >> 16));
         handshake.Add((byte) (body.Count >> 8));
         handshake.Add((byte) body.Count);
         handshake.AddRange(body);

         var record = new List<byte>();
         record.Add(0x16); // handshake
         record.Add(0x03);
         record.Add(0x01);
         AddUInt16(record, handshake.Count);
         record.AddRange(handshake);

         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", port);
            client.ReceiveTimeout = 20000;
            client.SendTimeout = 20000;

            NetworkStream stream = client.GetStream();
            byte[] hello = record.ToArray();
            stream.Write(hello, 0, hello.Length);
            stream.Flush();

            byte[] header = ReadExactly(stream, 5);
            byte[] payload = ReadExactly(stream, (header[3] << 8) | header[4]);

            if (header[0] == 0x15)
               Assert.Fail("Port " + port + " answered the TLS 1.2 ClientHello with an alert (level " + payload[0] +
                           ", description " + payload[1] + ").");

            Assert.AreEqual(0x16, (int) header[0], "Port " + port + ": expected a handshake record.");
            Assert.AreEqual(0x02, (int) payload[0], "Port " + port + ": expected a ServerHello.");

            int messageEnd = 4 + ((payload[1] << 16) | (payload[2] << 8) | payload[3]);
            Assert.IsTrue(messageEnd <= payload.Length, "Port " + port + ": the ServerHello is not whole in its first record.");

            int pos = 4;
            Assert.AreEqual(0x0303, (payload[pos] << 8) | payload[pos + 1], "Port " + port + ": the server did not answer with TLS 1.2.");
            pos += 2 + 32;           // server_version, random
            pos += 1 + payload[pos]; // session_id
            pos += 2 + 1;            // cipher_suite, compression_method

            Assert.IsTrue(pos + 2 <= messageEnd, "Port " + port + ": the ServerHello has no extensions.");
            int extensionsEnd = pos + 2 + ((payload[pos] << 8) | payload[pos + 1]);
            pos += 2;

            while (pos + 4 <= extensionsEnd)
            {
               int extensionType = (payload[pos] << 8) | payload[pos + 1];
               int extensionLength = (payload[pos + 2] << 8) | payload[pos + 3];
               pos += 4;

               if (extensionType == 0x000B)
               {
                  var data = new byte[extensionLength];
                  Array.Copy(payload, pos, data, 0, extensionLength);
                  return data;
               }

               pos += extensionLength;
            }

            Assert.Fail("Port " + port + ": the ServerHello carries no ec_point_formats extension.");
            return null;
         }
      }
   }
}