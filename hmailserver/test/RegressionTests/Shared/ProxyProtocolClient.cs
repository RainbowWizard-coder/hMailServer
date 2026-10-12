// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using hMailServer;
using NUnit.Framework;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    A raw TCP client for the IMAP and POP3 listeners' PROXY protocol fixtures
   ///    (IMAP\ProxyProtocol.cs, POP3\ProxyProtocol.cs): byte-level sends, because the
   ///    version 2 header is binary and a string round-trip through an encoding would
   ///    mangle address bytes of 0x80 and above, and strictly bounded reads, so that a
   ///    server waiting for a header the test never sends fails the test instead of
   ///    hanging it. SMTP's fixture has the same client of its own (SMTP\ProxyProtocol.cs).
   /// </summary>
   public sealed class ProxyProtocolClient : IDisposable
   {
      private readonly TcpClient _client;
      private readonly NetworkStream _stream;
      private readonly StringBuilder _pending = new StringBuilder();

      /// <param name="port">25, 110 or 143: the service, which TestPorts turns into the server's port.</param>
      public ProxyProtocolClient(int port)
      {
         _client = new TcpClient();
         _client.Connect(TestPorts.HostAddress, TestPorts.Actual(port));
         _stream = _client.GetStream();
         _stream.ReadTimeout = 15000;
         _stream.WriteTimeout = 15000;
      }

      public void Send(byte[] bytes)
      {
         _stream.Write(bytes, 0, bytes.Length);
         _stream.Flush();
      }

      public void Send(string text)
      {
         Send(Encoding.ASCII.GetBytes(text));
      }

      /// <summary>
      ///    Reads until what has arrived holds token, and returns everything read up to
      ///    and including the end of the line the token is on. What arrived after that
      ///    line is kept for the next read.
      /// </summary>
      public string ReadUntil(string token)
      {
         var buffer = new byte[4096];
         DateTime deadline = DateTime.UtcNow.AddSeconds(20);

         while (true)
         {
            string received = _pending.ToString();
            int tokenAt = received.IndexOf(token, StringComparison.Ordinal);

            if (tokenAt >= 0)
            {
               int lineEnd = received.IndexOf("\r\n", tokenAt, StringComparison.Ordinal);

               if (lineEnd >= 0)
               {
                  _pending.Remove(0, lineEnd + 2);
                  return received.Substring(0, lineEnd + 2);
               }
            }

            if (DateTime.UtcNow >= deadline)
               Assert.Fail("Timed out waiting for \"" + token + "\". Received so far: " + received);

            int read;

            try
            {
               read = _stream.Read(buffer, 0, buffer.Length);
            }
            catch (System.IO.IOException ex)
            {
               Assert.Fail("Nothing more arrived while the test waited for \"" + token + "\" (" + ex.Message +
                           "). Received so far: " + received);
               return null;
            }

            if (read == 0)
               Assert.Fail("The server closed the connection while the test waited for \"" + token +
                           "\". Received so far: " + received);

            _pending.Append(Encoding.ASCII.GetString(buffer, 0, read));
         }
      }

      /// <summary>
      ///    The line that ends the reply to an IMAP command sent with this tag: the
      ///    untagged lines before it are read and dropped.
      /// </summary>
      public string ReadTaggedReply(string tag)
      {
         while (true)
         {
            string line = ReadUntil("\r\n");

            if (line.StartsWith(tag + " ", StringComparison.Ordinal))
               return line;
         }
      }

      /// <summary>
      ///    Reads until the server closes the connection (an orderly close or a reset)
      ///    and returns whatever arrived first. Fails the test if the server keeps the
      ///    connection open instead.
      /// </summary>
      public string ReadUntilRemoteClose()
      {
         var received = new StringBuilder(_pending.ToString());
         _pending.Clear();

         var buffer = new byte[4096];
         DateTime deadline = DateTime.UtcNow.AddSeconds(20);

         while (DateTime.UtcNow < deadline)
         {
            int read;

            try
            {
               read = _stream.Read(buffer, 0, buffer.Length);
            }
            catch (System.IO.IOException ex)
            {
               var socketException = ex.InnerException as SocketException;

               if (socketException != null && socketException.SocketErrorCode == SocketError.TimedOut)
                  Assert.Fail("The server did not close the connection. Received so far: " + received);

               // A reset is the server dropping the connection as well.
               return received.ToString();
            }

            if (read == 0)
               return received.ToString();

            received.Append(Encoding.ASCII.GetString(buffer, 0, read));
         }

         Assert.Fail("The server did not close the connection. Received so far: " + received);
         return null;
      }

      public void Dispose()
      {
         try
         {
            _stream.Dispose();
            _client.Close();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      // ------------------------------------------------------------- the headers

      /// <summary>A PROXY protocol version 1 line for a TCP over IPv4 connection.</summary>
      public static string Version1Tcp4(string sourceAddress, int sourcePort, int destinationPort)
      {
         return "PROXY TCP4 " + sourceAddress + " 127.0.0.1 " + sourcePort + " " + destinationPort + "\r\n";
      }

      /// <summary>
      ///    A PROXY protocol version 2 header. The 12-byte signature is the
      ///    specification's constant, and the length is counted from the address block.
      /// </summary>
      public static byte[] Version2(bool proxyCommand, byte familyAndTransport, byte[] addressBlock)
      {
         var header = new List<byte>();

         // \x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A ("\r\n\r\n\0\r\nQUIT\n")
         header.AddRange(new byte[] { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A });

         // Version 2 in the high nibble; the command PROXY (1) or LOCAL (0) in the low.
         header.Add(proxyCommand ? (byte) 0x21 : (byte) 0x20);
         header.Add(familyAndTransport);
         header.Add((byte) (addressBlock.Length >> 8));
         header.Add((byte) (addressBlock.Length & 0xFF));
         header.AddRange(addressBlock);

         return header.ToArray();
      }

      /// <summary>A version 2 PROXY header for a TCP over IPv4 connection (family and transport 0x11).</summary>
      public static byte[] Version2Tcp4(string sourceAddress, int sourcePort, int destinationPort)
      {
         var block = new List<byte>();

         block.AddRange(IPAddress.Parse(sourceAddress).GetAddressBytes());
         block.AddRange(IPAddress.Parse("127.0.0.1").GetAddressBytes());
         block.Add((byte) (sourcePort >> 8));
         block.Add((byte) (sourcePort & 0xFF));
         block.Add((byte) (destinationPort >> 8));
         block.Add((byte) (destinationPort & 0xFF));

         return Version2(true, 0x11, block.ToArray());
      }

      /// <summary>
      ///    The address the server sees this machine's connections come from: the one
      ///    the trusted lists must name. Found by routing a UDP socket to the server,
      ///    which sends nothing; on the Windows bench and in the Linux suite it is the
      ///    loopback address.
      /// </summary>
      public static string AddressTheServerSees()
      {
         using (var probe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
         {
            probe.Connect(TestPorts.HostAddress, TestPorts.Actual(143));
            return ((IPEndPoint) probe.LocalEndPoint).Address.ToString();
         }
      }

      // ------------------------------------------------------------ the settings

      // Late bound, as BounceTestSupport reaches its settings: the four properties
      // arrived in 6.3.7, and the suite is compiled against whichever type library
      // the bench last registered.

      public static void SetImap(Settings settings, bool enabled, string trustedAddresses)
      {
         ((dynamic) settings).IMAPProxyProtocolTrustedIPs = trustedAddresses;
         ((dynamic) settings).IMAPProxyProtocolEnabled = enabled;
      }

      public static void SetPop3(Settings settings, bool enabled, string trustedAddresses)
      {
         ((dynamic) settings).POP3ProxyProtocolTrustedIPs = trustedAddresses;
         ((dynamic) settings).POP3ProxyProtocolEnabled = enabled;
      }

      public static bool ImapEnabled(Settings settings) => (bool) ((dynamic) settings).IMAPProxyProtocolEnabled;

      public static string ImapTrustedAddresses(Settings settings) => (string) ((dynamic) settings).IMAPProxyProtocolTrustedIPs;

      public static bool Pop3Enabled(Settings settings) => (bool) ((dynamic) settings).POP3ProxyProtocolEnabled;

      public static string Pop3TrustedAddresses(Settings settings) => (string) ((dynamic) settings).POP3ProxyProtocolTrustedIPs;

      /// <summary>
      ///    Both listeners back to what the server ships - off, and nobody trusted -
      ///    writing only what differs. PerformBasicSetup calls it before every test:
      ///    a run stopped inside one of these fixtures would otherwise leave the IMAP
      ///    or POP3 listener waiting for a header from this machine before it greets,
      ///    and every later test that speaks the protocol would fail at connect,
      ///    saying nothing about why.
      /// </summary>
      public static void TurnOffImapAndPop3(Settings settings)
      {
         if (ImapEnabled(settings) || !string.IsNullOrEmpty(ImapTrustedAddresses(settings)))
            SetImap(settings, false, string.Empty);

         if (Pop3Enabled(settings) || !string.IsNullOrEmpty(Pop3TrustedAddresses(settings)))
            SetPop3(settings, false, string.Empty);
      }
   }
}
