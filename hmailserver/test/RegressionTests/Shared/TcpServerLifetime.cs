// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Shared;

// Not RegressionTests.*, as OneTimeCodesStep: these need no server. They run alone with
//    nunit3-console RegressionTests.dll --where "namespace == ServerFreeTests"
// on a machine whose hMailServer must not be touched, and in every full run.
namespace ServerFreeTests
{
   /// <summary>
   ///    The simulated servers' TcpServer started and stopped as the suite uses it, with
   ///    no hMailServer. On batch44's gate (9 October 2026) SMTP.BounceReports failed
   ///    with a NullReferenceException inside TcpListener.Stop, called from Dispose:
   ///    the listen thread said it had finished and only then stopped its listener, so a
   ///    test that disposed the server the moment WaitForCompletion returned stopped the
   ///    same listener at the same time, and .NET Framework's Stop is not safe against
   ///    itself. And a listener whose construction threw left the listen thread a null
   ///    to stop, a NullReferenceException where nothing catches it - which ends the
   ///    Linux suite's test host, and which NUnit's agent on Windows, run with the legacy
   ///    unhandled-exception policy, lets end the thread alone, without a word.
   /// </summary>
   [TestFixture]
   public class TcpServerLifetime
   {
      private sealed class LineServer : TcpServer
      {
         public LineServer(int port)
            : base(1, port, eConnectionSecurity.eCSNone)
         {
         }

         protected override void HandleClient()
         {
            Send("220 ready\r\n");
            ReadUntil("QUIT\r\n");
            Send("221 bye\r\n");
         }
      }

      private static int FreePort()
      {
         var probe = new TcpListener(IPAddress.Loopback, 0);
         probe.Start();
         int port = ((IPEndPoint) probe.LocalEndpoint).Port;
         probe.Stop();
         return port;
      }

      private static string ReadUntil(NetworkStream stream, string text)
      {
         var received = new StringBuilder();
         var buffer = new byte[256];
         while (received.ToString().IndexOf(text, StringComparison.Ordinal) < 0)
         {
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read == 0)
               Assert.Fail("The simulated server closed the connection before it said '" + text.Trim() + "': " + received);
            received.Append(Encoding.ASCII.GetString(buffer, 0, read));
         }

         return received.ToString();
      }

      private static void Converse(int port)
      {
         using (var client = new TcpClient())
         {
            client.Connect(IPAddress.Loopback, port);
            NetworkStream stream = client.GetStream();
            stream.ReadTimeout = 10000;
            ReadUntil(stream, "220 ready\r\n");
            byte[] quit = Encoding.ASCII.GetBytes("QUIT\r\n");
            stream.Write(quit, 0, quit.Length);
            ReadUntil(stream, "221 bye\r\n");
         }
      }

      // TcpServer keeps its listener to itself, and TcpListener says whether it is
      // listening only to a subclass; a test that needs the answer reads both.
      private static bool IsListening(TcpServer server)
      {
         FieldInfo field = typeof(TcpServer).GetField("_tcpListener", BindingFlags.NonPublic | BindingFlags.Instance);
         Assert.That(field, Is.Not.Null, "TcpServer no longer keeps its listener in _tcpListener; this test reads it there.");
         var listener = (TcpListener) field.GetValue(server);
         Assert.That(listener, Is.Not.Null, "The simulated server never made its listener.");
         PropertyInfo active = typeof(TcpListener).GetProperty("Active", BindingFlags.NonPublic | BindingFlags.Instance);
         Assert.That(active, Is.Not.Null, "TcpListener has no Active property on this runtime.");
         return (bool) active.GetValue(listener);
      }

      [Test]
      [Description("A simulated server has stopped listening by the time WaitForCompletion returns, and is disposed at once without an error - fifty in a row.")]
      public void AServerHasStoppedListeningWhenItSaysItHasFinished()
      {
         for (int i = 0; i < 50; i++)
         {
            int port = FreePort();
            using (var server = new LineServer(port))
            {
               server.StartListen();
               Converse(port);
               server.WaitForCompletion();

               Assert.That(IsListening(server), Is.False,
                  "Server " + (i + 1) + " of 50 was still listening when it said it had finished, so the Dispose that follows at once " +
                  "stops its listener while the listen thread does.");
            }
         }
      }

      [Test]
      [Description("A port the listener refuses fails StartListen with the listener's own exception, and nothing else: no exception on the listen thread, which nothing catches.")]
      public void APortTheListenerRefusesFailsStartListenAlone()
      {
         // An exception the listen thread does not catch reaches this event on its way
         // out, whether it then ends the process (.NET) or the thread alone (NUnit's agent
         // on .NET Framework, which runs with the legacy unhandled-exception policy).
         Exception escaped = null;
         UnhandledExceptionEventHandler watch = (sender, e) =>
         {
            var exception = e.ExceptionObject as Exception;
            if (exception != null && (exception.StackTrace ?? "").Contains("TcpServer"))
               escaped = exception;
         };

         AppDomain.CurrentDomain.UnhandledException += watch;
         try
         {
            var server = new LineServer(70000);
            Assert.That(() => server.StartListen(), Throws.TypeOf<ArgumentOutOfRangeException>());
            server.Dispose();

            // Dispose returns once the listen thread has said it finished; the thread's
            // last statements run after that word, so wait for the thread itself.
            FieldInfo field = typeof(TcpServer).GetField("_serverThread", BindingFlags.NonPublic | BindingFlags.Instance);
            Assert.That(field, Is.Not.Null, "TcpServer no longer keeps its listen thread in _serverThread; this test reads it there.");
            Assert.That(((Thread) field.GetValue(server)).Join(TimeSpan.FromSeconds(15)), Is.True, "The listen thread did not end.");
         }
         finally
         {
            AppDomain.CurrentDomain.UnhandledException -= watch;
         }

         Assert.That(escaped, Is.Null, "An exception escaped the listen thread: " + escaped);
      }

      [Test]
      [Description("A server disposed before any client came stops its listener once, and a second Dispose finds nothing left to do.")]
      public void AServerDisposedBeforeAnyClientCameStopsOnce()
      {
         int port = FreePort();
         var server = new LineServer(port);
         server.StartListen();
         Assert.That(IsListening(server), Is.True, "The simulated server was not listening after StartListen.");

         server.Dispose();
         Assert.That(IsListening(server), Is.False, "The simulated server was still listening after Dispose.");
         Assert.That(() => server.Dispose(), Throws.Nothing);
         Assert.That(server.NumberOfConnectedClients, Is.EqualTo(0));
      }
   }
}
