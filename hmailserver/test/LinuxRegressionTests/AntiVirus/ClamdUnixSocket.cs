// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiVirus
{
   /// <summary>
   ///    The Linux server reaching clamd over its Unix socket: ClamAVHost set to the
   ///    absolute path of the socket, as clamav-daemon's LocalSocket is on Debian and
   ///    Ubuntu (/var/run/clamav/clamd.ctl) and on Alpine (/run/clamav/clamd.sock).
   ///    This project's own fixture and not a file of the Windows suite's: the Windows
   ///    server refuses a path (AntiVirus/ClamdSocketPathOnWindows.cs there), and
   ///    RegressionTests/AntiVirus/ClamAV.cs needs a real clamd on TCP 3310, started
   ///    from C:\clamav.
   ///
   ///    Each test stands up a fake clamd of its own on a Unix socket - the
   ///    n-prefixed PING, VERSION and INSTREAM, and clamd's own forms of its two
   ///    verdicts - under HMTEST_FAKE_CLAMD_DIR, or the temporary directory, so the
   ///    server has to be on this machine and see that directory: a server under the
   ///    systemd unit, whose /tmp is private, needs HMTEST_FAKE_CLAMD_DIR set to a
   ///    directory it can reach. With HMTEST_CLAMD_SOCKET naming a real clamd's
   ///    socket, the last test sends a clean message and EICAR through that clamd.
   ///
   ///    What the connection test (PING, VERSION and the two samples) does over the
   ///    socket is held by the clamd-socket-tests program the Linux build runs: the
   ///    REST API has no route for it yet, so the shim's TestClamAVScanner skips.
   /// </summary>
   [TestFixture]
   public class ClamdUnixSocket : TestFixtureBase
   {
      private hMailServer.AntiVirus _antiVirus;
      private string _directory;
      private eAntivirusAction _action;

      [SetUp]
      public new void SetUp()
      {
         if (!TestTarget.IsLocal)
            NotOnThisServer.Ignore("the fake clamd's Unix socket is on this machine, and the server under test is at " + TestTarget.Host);

         _antiVirus = _application.Settings.AntiVirus;
         _action = _antiVirus.Action;
         _antiVirus.Action = eAntivirusAction.hDeleteEmail;

         string parent = Environment.GetEnvironmentVariable("HMTEST_FAKE_CLAMD_DIR");
         if (string.IsNullOrWhiteSpace(parent))
            parent = Path.GetTempPath();

         // Short, so that the socket's path fits a socket address (107 bytes).
         _directory = Path.Combine(parent.Trim(), "hm-clamd-" + Guid.NewGuid().ToString("N").Substring(0, 8));
         Directory.CreateDirectory(_directory);
         File.SetUnixFileMode(_directory, (UnixFileMode) Convert.ToInt32("755", 8));
      }

      [TearDown]
      public void PutClamAvBack()
      {
         if (_antiVirus == null)
            return;

         _antiVirus.ClamAVEnabled = false;
         _antiVirus.ClamAVHost = "localhost";
         _antiVirus.ClamAVPort = 3310;
         _antiVirus.Action = _action;

         try
         {
            Directory.Delete(_directory, true);
         }
         catch (IOException)
         {
         }
      }

      [Test]
      [Description("A clean message is streamed to clamd's Unix socket with INSTREAM and delivered: the fake received it, and the port, which a socket does not use, is not looked at.")]
      public void ACleanMessageGoesToTheSocketAndIsDelivered()
      {
         string socket = Path.Combine(_directory, "clamd.sock");
         using (var clamd = new FakeClamd(socket))
         {
            _antiVirus.ClamAVHost = socket;
            _antiVirus.ClamAVPort = 1;
            _antiVirus.ClamAVEnabled = true;

            var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Over the socket", "A body clamd finds nothing in.");
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

            Assert.That(clamd.Commands, Does.Contain("nINSTREAM"), "The message was not streamed to the socket.");
            Assert.That(clamd.Streams.Any(stream => stream.Contains("A body clamd finds nothing in.")), Is.True,
               "The stream the fake received did not carry the message.");
         }
      }

      [Test]
      [Description("A message clamd finds a virus in, over its Unix socket, is deleted with the name clamd gave.")]
      public void AnInfectedMessageIsDeleted()
      {
         string socket = Path.Combine(_directory, "clamd.sock");
         using (var clamd = new FakeClamd(socket))
         {
            _antiVirus.ClamAVHost = socket;
            _antiVirus.ClamAVEnabled = true;

            var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Over the socket", "Carrying " + FakeClamd.Marker + ".");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);

            StringAssert.Contains("Message will be deleted (contained virus " + FakeClamd.VirusName + ").", LogHandler.ReadCurrentDefaultLog());
         }
      }

      [Test]
      [Description("ClamAVHost naming a path with no socket at it is reported as nothing listening there, and the message is delivered as the default policy does for a scanner that cannot be reached.")]
      public void AMissingSocketIsReportedAsNothingListening()
      {
         string socket = Path.Combine(_directory, "no-such.sock");
         _antiVirus.ClamAVHost = socket;
         _antiVirus.ClamAVEnabled = true;

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Over the socket", "Nobody scans this.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         CustomAsserts.AssertReportedError("Unable to connect to ClamAV server at " + socket + ": nothing is listening there, as there is no socket at that path.");
      }

      [Test]
      [Description("A socket nothing accepts on - its listener gone and the file left, as a clamd that stopped uncleanly leaves it - is reported as nothing listening there too.")]
      public void ASocketNothingAcceptsOnIsReportedAsNothingListening()
      {
         // .NET removes the file of a socket it bound when the socket is closed, as
         // clamd does when it stops cleanly; moved aside while bound, the file stays.
         string socket = Path.Combine(_directory, "stale.sock");
         string bound = Path.Combine(_directory, "bound.sock");
         using (var stale = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified))
         {
            stale.Bind(new UnixDomainSocketEndPoint(bound));
            File.SetUnixFileMode(bound, (UnixFileMode) Convert.ToInt32("666", 8));
            File.Move(bound, socket);
         }

         Assert.That(File.Exists(socket), Is.True, "The socket's file should have been left behind.");

         _antiVirus.ClamAVHost = socket;
         _antiVirus.ClamAVEnabled = true;

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Over the socket", "Nobody scans this either.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         CustomAsserts.AssertReportedError("Unable to connect to ClamAV server at " + socket + ": nothing is listening there: the socket is there, and no clamd accepts on it.");
      }

      [Test]
      [Description("With HMTEST_CLAMD_SOCKET naming a real clamd's socket, a clean message is delivered through it and one carrying EICAR is deleted.")]
      public void ARealClamdOnItsSocket()
      {
         string socket = Environment.GetEnvironmentVariable("HMTEST_CLAMD_SOCKET");
         if (string.IsNullOrWhiteSpace(socket))
            Assert.Ignore("HMTEST_CLAMD_SOCKET names no clamd socket; set it to a running clamd's LocalSocket to run this.");

         _antiVirus.ClamAVHost = socket.Trim();
         _antiVirus.ClamAVEnabled = true;

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Clean", "Nothing in here.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         LogHandler.MarkDefaultLog();
         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address, MessageWithEicar(account.Address));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         // Eicar-Test-Signature or Eicar-Signature, by the ClamAV release (ClamAV.cs).
         StringAssert.IsMatch(@"Message will be deleted \(contained virus Eicar[-\w]*\)\.", LogHandler.ReadCurrentDefaultLog());
      }

      // EICAR as a base64 attachment, assembled at run time so that neither it nor its
      // base64 form is a literal here (ClamAV.cs says why it must be an attachment).
      private static string MessageWithEicar(string address)
      {
         var firstPart = @"X5O!P%@AP[4\PZX54(P^)7CC)7}";
         var secondPart = @"$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";
         var encoded = Convert.ToBase64String(Encoding.ASCII.GetBytes(firstPart + secondPart));

         return
            "From: " + address + "\r\n" +
            "To: " + address + "\r\n" +
            "Subject: EICAR\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/mixed; boundary=\"eicar-boundary\"\r\n" +
            "\r\n" +
            "--eicar-boundary\r\n" +
            "Content-Type: text/plain\r\n" +
            "\r\n" +
            "Test message.\r\n" +
            "\r\n" +
            "--eicar-boundary\r\n" +
            "Content-Type: application/octet-stream; name=\"eicar.com\"\r\n" +
            "Content-Transfer-Encoding: base64\r\n" +
            "Content-Disposition: attachment; filename=\"eicar.com\"\r\n" +
            "\r\n" +
            encoded + "\r\n" +
            "\r\n" +
            "--eicar-boundary--\r\n";
      }

      /// <summary>
      ///    A clamd on a Unix socket, one command a connection: PING answered PONG,
      ///    VERSION with a version line, and INSTREAM's length-prefixed chunks read to
      ///    the zero length that ends them and answered "stream: OK", or
      ///    "stream: <name> FOUND" for a stream carrying the marker. The socket is
      ///    made writable by every user, as Debian's LocalSocketMode 666 makes clamd's,
      ///    so that a server running as another user reaches it.
      /// </summary>
      private sealed class FakeClamd : IDisposable
      {
         public const string Marker = "FAKE-CLAMD-INFECTED-MARKER";
         public const string VirusName = "Fake-Clamd.Test-Signature";

         private readonly string _path;
         private readonly Socket _listener;
         private readonly Thread _thread;
         private readonly List<string> _commands = new List<string>();
         private readonly List<string> _streams = new List<string>();
         private volatile bool _stopping;

         public FakeClamd(string path)
         {
            _path = path;
            _listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            _listener.Bind(new UnixDomainSocketEndPoint(path));
            _listener.Listen(16);
            File.SetUnixFileMode(path, (UnixFileMode) Convert.ToInt32("666", 8));

            _thread = new Thread(Serve) { IsBackground = true, Name = "FakeClamd" };
            _thread.Start();
         }

         public string[] Commands
         {
            get { lock (_commands) return _commands.ToArray(); }
         }

         public string[] Streams
         {
            get { lock (_streams) return _streams.ToArray(); }
         }

         public void Dispose()
         {
            _stopping = true;
            _listener.Dispose();
            _thread.Join(5000);
            try
            {
               File.Delete(_path);
            }
            catch (IOException)
            {
            }
         }

         private void Serve()
         {
            while (!_stopping)
            {
               Socket client;
               try
               {
                  client = _listener.Accept();
               }
               catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException)
               {
                  return;
               }

               using (client)
               {
                  client.ReceiveTimeout = 30000;
                  try
                  {
                     Converse(client);
                  }
                  catch (Exception ex) when (ex is SocketException || ex is IOException)
                  {
                  }
               }
            }
         }

         private void Converse(Socket client)
         {
            var command = new StringBuilder();
            var one = new byte[1];
            while (command.Length < 64 && client.Receive(one) == 1 && one[0] != (byte) '\n')
               command.Append((char) one[0]);

            lock (_commands)
               _commands.Add(command.ToString());

            switch (command.ToString())
            {
               case "nPING":
                  Say(client, "PONG");
                  return;
               case "nVERSION":
                  Say(client, "ClamAV 1.4.3/27800/Mon Oct 26 09:00:00 2026");
                  return;
               case "nINSTREAM":
                  break;
               default:
                  return;
            }

            var stream = new MemoryStream();
            for (;;)
            {
               byte[] prefix = ReadExactly(client, 4);
               int size = (prefix[0] << 24) | (prefix[1] << 16) | (prefix[2] << 8) | prefix[3];
               if (size == 0)
                  break;
               if (size < 0 || size > 1024 * 1024)
                  return;
               byte[] chunk = ReadExactly(client, size);
               stream.Write(chunk, 0, chunk.Length);
            }

            string text = Encoding.ASCII.GetString(stream.ToArray());
            lock (_streams)
               _streams.Add(text);

            Say(client, text.Contains(Marker) ? "stream: " + VirusName + " FOUND" : "stream: OK");
         }

         private static byte[] ReadExactly(Socket client, int size)
         {
            var buffer = new byte[size];
            int have = 0;
            while (have < size)
            {
               int got = client.Receive(buffer, have, size - have, SocketFlags.None);
               if (got <= 0)
                  throw new IOException("The client went before the stream ended.");
               have += got;
            }
            return buffer;
         }

         private static void Say(Socket client, string line)
         {
            client.Send(Encoding.ASCII.GetBytes(line + "\n"));
         }
      }
   }
}
