// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The settings pages' REST session against a listener this test runs: what the
   /// client says, and what it sends, when the listener goes away between two
   /// requests, when it takes the connection and never answers, and when its
   /// certificate names a host other than the address asked.
   /// </summary>
   public class RestSettingsClientTests
   {
      private const string Authorization = "Basic YWRtaW5pc3RyYXRvcjpzZWNyZXQ="; // administrator:secret

      [Fact]
      public void ARequestAfterTheListenerHasGone_SaysWhy_AndThrowsNothing()
      {
         // A session is made; then every request is refused as the server refuses an
         // idled-out session.
         using var server = new Listener_((method, path) => method == "POST" ? (200, "{}") : (401, "{\"error\":\"unauthorized\"}"));

         const string off = "The REST API is off (RestApiPort is 0).";
         bool gone = false;
         var client = new RestSettingsClient(() => Authorization,
            () => gone ? new RestSettingsClient.Listener(null, off, null) : new RestSettingsClient.Listener(server.Http, null, null));

         Assert.True(client.Open(null, false, out string opened), opened);

         // RestApiPort saved as 0 - which the listener takes at the next reinitialize,
         // not now - and the session idled out: the first group's GET is refused, and
         // the session cannot be made again, because the listener is no longer where
         // the settings say. That reason is what the card says, not "the credential
         // was refused".
         gone = true;
         RestSettingsClient.Reply first = client.Get(RestSettings.ServerGroup, null);
         Assert.False(first.Ok);
         Assert.Equal(off, client.RefusalFor(first));

         // The page's next group: this threw ArgumentNullException out of the Uri the
         // request was built on, outside the request's own try, and out of the save.
         RestSettingsClient.Reply second = client.Get(RestSettings.PushGroup, null);
         Assert.False(second.Ok);
         Assert.Equal(off, client.RefusalFor(second));

         RestSettingsClient.Reply put = client.Put(RestSettings.OpenPgpGroup, "{}", null);
         Assert.False(put.Ok);
         Assert.Equal(off, client.RefusalFor(put));
      }

      [Fact]
      public void AListenerThatSaysNothing_IsWaitedForSeconds_AndTheReasonSaysItTimedOut()
      {
         // The page waits for these on the dispatcher: a few seconds, not fifteen.
         Assert.True(RestSettingsClient.ConnectTimeout <= TimeSpan.FromSeconds(5));
         Assert.True(RestSettingsClient.RequestTimeout <= TimeSpan.FromSeconds(5));

         using var server = new Listener_((method, path) => null);
         var client = new RestSettingsClient(() => Authorization,
            () => new RestSettingsClient.Listener(server.Http, null, null), requestTimeout: TimeSpan.FromMilliseconds(500));

         var clock = Stopwatch.StartNew();
         Assert.False(client.Open(null, false, out string refusal));
         Assert.True(clock.Elapsed < TimeSpan.FromSeconds(4), clock.Elapsed.ToString());

         // The innermost message was the socket's "The I/O operation has been
         // aborted ...", which does not say that the request timed out.
         Assert.Contains(server.Http.ToString(), refusal);
         Assert.Contains("Timeout", refusal);
         Assert.DoesNotContain("aborted", refusal);
      }

      [Fact]
      public void FailureOf_SaysATimeoutInTheWordsThatSayIt_AndIsOtherwiseTheInnermostMessage()
      {
         // HttpClient.Timeout, as .NET throws it: the TimeoutException repeats the
         // cancellation it wraps, and the words are one level up.
         var request = new TaskCanceledException("The request was canceled due to the configured HttpClient.Timeout of 5 seconds elapsing.",
            new TimeoutException("The operation was canceled.",
               new TaskCanceledException("The operation was canceled.",
                  new IOException("Unable to read data from the transport connection: The I/O operation has been aborted.",
                     new SocketException((int)SocketError.OperationAborted)))));
         Assert.Equal("The request was canceled due to the configured HttpClient.Timeout of 5 seconds elapsing.", RestSettingsClient.FailureOf(request));

         // SocketsHttpHandler.ConnectTimeout: the TimeoutException has the words.
         var connect = new TaskCanceledException("The operation was canceled.",
            new TimeoutException("A connection could not be established within the configured ConnectTimeout."));
         Assert.Equal("A connection could not be established within the configured ConnectTimeout.", RestSettingsClient.FailureOf(connect));

         var refused = new System.Net.Http.HttpRequestException("An error occurred.",
            new SocketException((int)SocketError.ConnectionRefused));
         Assert.Equal(new SocketException((int)SocketError.ConnectionRefused).Message, RestSettingsClient.FailureOf(refused));
      }

      [Fact]
      public void OnTheServerItself_TheCertificateTheListenerServes_IsTrusted_AndNoOther()
      {
         // The ACME certificate names the server's host names; the Control Panel on
         // the server asks https://127.0.0.1, which it does not name.
         using X509Certificate2 served = RestSettingsTests.SelfSigned("mail.example.com");
         using X509Certificate2 other = RestSettingsTests.SelfSigned("mail.example.com");
         using var server = new Listener_((method, path) => (200, "{}"), served);

         string folder = Path.Combine(Path.GetTempPath(), "hm-cp-rest-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(folder);
         try
         {
            string servedFile = Path.Combine(folder, "fullchain.pem");
            string otherFile = Path.Combine(folder, "other.pem");
            File.WriteAllText(servedFile, served.ExportCertificatePem() + "\n" + other.ExportCertificatePem() + "\n");
            File.WriteAllText(otherFile, other.ExportCertificatePem() + "\n");

            var pinned = new RestSettingsClient(() => Authorization,
               () => new RestSettingsClient.Listener(server.Https, null, servedFile));
            Assert.True(pinned.Open(null, false, out string refusal), refusal);
            Assert.True(pinned.Get(RestSettings.ServerGroup, null).Ok);
            int sent = server.Requests;
            Assert.Equal(2, sent);

            // Held to another certificate, or to nothing, it is refused as before -
            // and nothing, the password least of all, is sent past it.
            var wrong = new RestSettingsClient(() => Authorization,
               () => new RestSettingsClient.Listener(server.Https, null, otherFile));
            Assert.False(wrong.Open(null, false, out string mismatch));
            Assert.Contains(server.Https.ToString(), mismatch);

            var unheld = new RestSettingsClient(() => Authorization,
               () => new RestSettingsClient.Listener(server.Https, null, null));
            Assert.False(unheld.Open(null, false, out _));

            var unreadable = new RestSettingsClient(() => Authorization,
               () => new RestSettingsClient.Listener(server.Https, null, Path.Combine(folder, "missing.pem")));
            Assert.False(unreadable.Open(null, false, out _));

            Assert.Equal(sent, server.Requests);
         }
         finally
         {
            Directory.Delete(folder, true);
         }
      }

      /// <summary>
      /// A listener on 127.0.0.1 that answers each request with what answer says -
      /// null to take the request and never answer - over TLS with certificate when
      /// one is given, and counts the requests it read.
      /// </summary>
      private sealed class Listener_ : IDisposable
      {
         private readonly TcpListener listener_ = new(IPAddress.Loopback, 0);
         private readonly Func<string, string, (int Status, string Body)?> answer_;
         private readonly X509Certificate2 certificate_;
         private readonly CancellationTokenSource stop_ = new();
         private int requests_;

         public Listener_(Func<string, string, (int Status, string Body)?> answer, X509Certificate2 certificate = null)
         {
            answer_ = answer;
            certificate_ = certificate;
            listener_.Start();
            _ = Task.Run(Serve_);
         }

         private int Port => ((IPEndPoint)listener_.LocalEndpoint).Port;

         public Uri Http => new("http://127.0.0.1:" + Port + "/");

         public Uri Https => new("https://127.0.0.1:" + Port + "/");

         public int Requests => Volatile.Read(ref requests_);

         public void Dispose()
         {
            stop_.Cancel();
            listener_.Stop();
         }

         private async Task Serve_()
         {
            while (!stop_.IsCancellationRequested)
            {
               TcpClient connection;
               try
               {
                  connection = await listener_.AcceptTcpClientAsync(stop_.Token);
               }
               catch (Exception)
               {
                  return;
               }

               _ = Task.Run(() => Answer_(connection));
            }
         }

         private async Task Answer_(TcpClient connection)
         {
            using (connection)
            {
               try
               {
                  Stream stream = connection.GetStream();
                  if (certificate_ != null)
                  {
                     var tls = new SslStream(stream, false);
                     await tls.AuthenticateAsServerAsync(new SslServerAuthenticationOptions { ServerCertificate = certificate_ }, stop_.Token);
                     stream = tls;
                  }

                  string head = await ReadHead_(stream);
                  string[] requestLine = head.Split("\r\n")[0].Split(' ');
                  int length = 0;
                  foreach (string line in head.Split("\r\n"))
                  {
                     if (line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
                        length = int.Parse(line.Substring("Content-Length:".Length).Trim());
                  }

                  byte[] body = new byte[length];
                  for (int read = 0; read < length;)
                     read += await stream.ReadAsync(body.AsMemory(read), stop_.Token);

                  Interlocked.Increment(ref requests_);

                  (int Status, string Body)? answer = answer_(requestLine[0], requestLine[1]);
                  if (answer == null)
                  {
                     await Task.Delay(Timeout.Infinite, stop_.Token);
                     return;
                  }

                  byte[] content = Encoding.UTF8.GetBytes(answer.Value.Body);
                  byte[] response = Encoding.ASCII.GetBytes(
                     "HTTP/1.1 " + answer.Value.Status + " Answer\r\nContent-Type: application/json\r\nContent-Length: " +
                     content.Length + "\r\nConnection: close\r\n\r\n");
                  await stream.WriteAsync(response, stop_.Token);
                  await stream.WriteAsync(content, stop_.Token);
                  await stream.FlushAsync(stop_.Token);
               }
               catch (Exception)
               {
                  // A handshake the client refused, a request abandoned at its
                  // timeout, the test ending: nothing to answer.
               }
            }
         }

         private async Task<string> ReadHead_(Stream stream)
         {
            var head = new StringBuilder();
            byte[] one = new byte[1];
            while (!head.ToString().EndsWith("\r\n\r\n", StringComparison.Ordinal))
            {
               if (await stream.ReadAsync(one, stop_.Token) == 0)
                  throw new EndOfStreamException();
               head.Append((char)one[0]);
            }

            return head.ToString();
         }
      }
   }
}
