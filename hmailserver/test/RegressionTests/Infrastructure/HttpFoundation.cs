// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    The HTTP/1.1 server the REST API and the web services run on
   ///    (Common/Util/HttpServer.cpp): keep-alive, pipelining, Expect:
   ///    100-continue, the refusals it makes on its own account - and the point
   ///    of it, that a slow or idle client no longer holds the listener for
   ///    everyone else, and is closed when its absolute deadline passes.
   ///
   ///    The requests here are written by hand on a TcpClient rather than sent
   ///    through HttpClient, because what is being tested is the framing: which
   ///    version answers, whether the connection stays open, and what arrives on
   ///    it next.
   /// </summary>
   [TestFixture]
   public class HttpFoundation : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 11431;
      private const int WebPort = 11432;
      private const string AdminPassword = "testar";

      // RestApiServer.cpp: RequestSeconds. WebServicesServer.cpp: RequestSeconds.
      private const int RestRequestSeconds = 30;
      private const int WebRequestSeconds = 15;

      [OneTimeSetUp]
      public void HttpFoundationFixtureSetUp()
      {
         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebPort.ToString());

         _application.Reinitialize();
      }

      [OneTimeTearDown]
      public void HttpFoundationFixtureTearDown()
      {
         RestListener.Stop();
         IniFileSetting.Write("WebServicesBindAddress", "0.0.0.0");
         IniFileSetting.Write("WebServicesHttpPort", "0");

         _application.Reinitialize();
      }

      [Test]
      [Description("An HTTP/1.1 connection carries several requests, and is closed when the client asks")]
      public void KeepAliveAnswersSeveralRequestsOnOneConnection()
      {
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            var reader = new HttpReader(stream);

            Send(stream, Request("GET", "/api/v1/status", "HTTP/1.1", Basic()));
            HttpReply first = reader.Read();
            Assert.AreEqual(200, first.Status, "Body: " + first.Body);
            Assert.AreEqual("keep-alive", first.Header("connection"));
            StringAssert.StartsWith("HTTP/1.1 ", first.StatusLine);
            StringAssert.Contains("\"version\"", first.Body);

            // The API's answers are the server's state at that moment, given
            // to a credential; nothing in between may keep one.
            Assert.AreEqual("no-store", first.Header("cache-control"));

            // Same socket, no reconnect.
            Send(stream, Request("GET", "/api/v1/status", "HTTP/1.1", Basic()));
            HttpReply second = reader.Read();
            Assert.AreEqual(200, second.Status, "Body: " + second.Body);
            Assert.AreEqual("keep-alive", second.Header("connection"));

            Send(stream, Request("GET", "/api/v1/status", "HTTP/1.1", Basic() + "Connection: close\r\n"));
            HttpReply last = reader.Read();
            Assert.AreEqual(200, last.Status, "Body: " + last.Body);
            Assert.AreEqual("close", last.Header("connection"));

            Assert.IsTrue(reader.AtEnd(), "The server must close the connection after a response it marked Connection: close.");
         }
      }

      [Test]
      [Description("An HTTP/1.0 request is answered as before: one response, Connection: close, then end of stream")]
      public void AnHttp10RequestIsAnsweredAndClosed()
      {
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            var reader = new HttpReader(stream);

            Send(stream, Request("GET", "/api/v1/status", "HTTP/1.0", Basic() + "Connection: close\r\n"));
            HttpReply reply = reader.Read();

            Assert.AreEqual(200, reply.Status, "Body: " + reply.Body);
            Assert.AreEqual("close", reply.Header("connection"));
            Assert.IsTrue(reader.AtEnd(), "An HTTP/1.0 client that did not ask for keep-alive must be closed after its response.");
         }
      }

      [Test]
      [Description("Two requests written before either is read are answered in order on the same connection")]
      public void PipelinedRequestsAreAnsweredInOrder()
      {
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            var reader = new HttpReader(stream);

            Send(stream, Request("GET", "/api/v1/status", "HTTP/1.1", Basic()) +
                         Request("GET", "/api/v1/no-such-route", "HTTP/1.1", Basic()));

            HttpReply first = reader.Read();
            HttpReply second = reader.Read();

            Assert.AreEqual(200, first.Status, "Body: " + first.Body);
            Assert.AreEqual(404, second.Status, "Body: " + second.Body);
         }
      }

      [Test]
      [Description("Expect: 100-continue is answered before the body is sent, and the body is then read")]
      public void ExpectContinueIsAnsweredBeforeTheBodyIsSent()
      {
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            var reader = new HttpReader(stream);

            // A body the API refuses (no label), so nothing is created.
            string body = "{}";
            Send(stream, Request("POST", "/api/v1/apikeys", "HTTP/1.1",
               Basic() + "Content-Type: application/json\r\nContent-Length: " + body.Length + "\r\nExpect: 100-continue\r\n"));

            HttpReply interim = reader.Read();
            Assert.AreEqual(100, interim.Status, "The declared body must be invited before it is sent. Got: " + interim.StatusLine);

            Send(stream, body);

            HttpReply final = reader.Read();
            Assert.AreEqual(400, final.Status, "Body: " + final.Body);
            StringAssert.Contains("label is required", final.Body);
         }
      }

      [Test]
      [Description("A chunked request body is refused with 411, in the API's own JSON shape, and the connection is closed")]
      public void AChunkedBodyIsRefusedWith411()
      {
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            var reader = new HttpReader(stream);

            Send(stream, Request("POST", "/api/v1/apikeys", "HTTP/1.1",
               Basic() + "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n"));

            HttpReply reply = reader.Read();
            Assert.AreEqual(411, reply.Status, "Body: " + reply.Body);
            StringAssert.Contains("\"error\"", reply.Body);
            Assert.AreEqual("close", reply.Header("connection"));
            Assert.IsTrue(reader.AtEnd(), "The framing after a refused body cannot be trusted, so the connection must close.");
         }
      }

      [Test]
      [Description("A head that never ends is refused with 413 once it is over the cap, rather than buffered forever")]
      public void AnOversizedHeadIsRefusedWith413()
      {
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            var reader = new HttpReader(stream);

            var head = new StringBuilder();
            head.Append("GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n");
            string padding = new string('p', 1000);
            for (int i = 0; i < 80; i++)
               head.Append("X-Padding-").Append(i).Append(": ").Append(padding).Append("\r\n");

            // The server answers and closes while the client may still be
            // sending, so the write can fail with a reset. That is the refusal
            // doing its job; what matters is what the client was told, if it was
            // told anything, and that it was not a success.
            int status;
            try
            {
               Send(stream, head.ToString());
               status = reader.Read().Status;
            }
            catch (IOException)
            {
               status = 0;
            }

            if (status != 0)
               Assert.AreEqual(413, status);
         }
      }

      [Test]
      [Description("A head that never ends is refused as soon as it is over the cap, not held until the request deadline")]
      public void AHeadThatNeverEndsIsRefusedAtTheCapNotAtTheDeadline()
      {
         // The negative control for HttpHeadMatch. Until 26 September 2026 the
         // head was read into a buffer sized for the largest body a route takes
         // (16 MB here), and the 64 KB cap on a head was asked only once its
         // blank line had arrived: 80 KB of head with no end was read, kept, and
         // answered by nothing but the request timer closing the connection
         // RestRequestSeconds later.
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            var head = new StringBuilder();
            head.Append("GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n");
            string padding = new string('p', 1000);
            for (int i = 0; i < 80; i++)
               head.Append("X-Padding-").Append(i).Append(": ").Append(padding).Append("\r\n");

            var stopwatch = Stopwatch.StartNew();

            // A reset while the rest of the head is still being written, or
            // while the answer is read, is the refusal too; what is measured is
            // how long the server took to end the request.
            int status = 0;
            try
            {
               Send(stream, head.ToString());
               var reader = new HttpReader(stream);
               stream.ReadTimeout = (RestRequestSeconds + 20) * 1000;
               status = reader.Read().Status;
            }
            catch (IOException)
            {
            }

            stopwatch.Stop();

            if (status != 0)
               Assert.AreEqual(413, status);

            Assert.Less(stopwatch.ElapsedMilliseconds, 10000,
               "A head over the cap must be refused when it passes the cap, not when the request deadline closes the connection.");
         }
      }

      [Test]
      [Description("A response a reader takes longer than the request deadline to read, but keeps reading, arrives whole")]
      public void ALargeResponseReadSlowlyArrivesWhole()
      {
         // The negative control for the response written in pieces. Until 26
         // September 2026 the request timer armed when the request began covered
         // writing the response too, so a download the reader could not take in
         // within RestRequestSeconds - a file link of 100 MB below 27 Mbit/s -
         // was cut off part way, every time. Here a 12 MB message's source is
         // read at about 250 KB a second, well past the deadline, through a small
         // receive window so that neither end's buffers can hold what is left.
         const int MessageBytes = LargeMessageBytes;
         string id = CreateLargeMessage("slow-reader");

         var client = new TcpClient();
         client.ReceiveBufferSize = 8192;
         client.Connect("127.0.0.1", RestPort);

         using (client)
         using (NetworkStream stream = client.GetStream())
         {
            Send(stream, "GET /api/v1/accounts/slow-reader@" + _domain.Name + "/messages/" + id + "/source HTTP/1.1\r\n" +
                         "Host: 127.0.0.1\r\n" + Basic() + "Connection: close\r\n\r\n");

            stream.ReadTimeout = (RestRequestSeconds + 20) * 1000;

            var stopwatch = Stopwatch.StartNew();
            var received = new MemoryStream();
            byte[] buffer = new byte[16 * 1024];
            try
            {
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
               {
                  received.Write(buffer, 0, read);
                  Thread.Sleep(read * 1000 / (250 * 1024));
               }
            }
            catch (IOException)
            {
               // A reset is the connection ended; what arrived is judged below.
            }

            stopwatch.Stop();

            string text = Encoding.ASCII.GetString(received.ToArray());
            int headEnd = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
            Assert.IsTrue(headEnd > 0, "No response head arrived.");
            StringAssert.StartsWith("HTTP/1.1 200", text, text.Substring(0, headEnd));

            int declared = 0;
            foreach (string header in text.Substring(0, headEnd).Split(new[] { "\r\n" }, StringSplitOptions.None))
            {
               if (header.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
                  declared = int.Parse(header.Substring(15).Trim());
            }

            Assert.Greater(declared, MessageBytes - 1, "The whole message was declared.");
            Assert.Greater(stopwatch.ElapsedMilliseconds, RestRequestSeconds * 1000L,
               "The read has to outlast the request deadline for this test to say anything.");
            Assert.AreEqual(declared, text.Length - headEnd - 4,
               "The response was cut off after " + stopwatch.ElapsedMilliseconds + " ms.");
         }
      }

      [Test]
      [Description("Only so many large downloads are written at once; one more is answered 503 with Retry-After, and a slot freed is taken again")]
      public void OnlySoManyLargeDownloadsAreWrittenAtOnce()
      {
         // The negative control for HttpLimits::max_large_responses. Until 26
         // September 2026 every connection could hold a large response - a 500
         // MB file link, three copies of it - for as long as its reader cared
         // to take, and 64 slow readers held 64 of them. Here sixteen readers
         // that take the head of a 12 MB message's source and then stop hold
         // the sixteen slots, and a seventeenth is refused at once; against the
         // old code it was answered 200 and given the message too.
         const int Slots = 16;   // HttpServer.h: HttpLimits::max_large_responses
         string id = CreateLargeMessage("large-downloads");
         string request = "GET /api/v1/accounts/large-downloads@" + _domain.Name + "/messages/" + id + "/source HTTP/1.1\r\n" +
                          "Host: 127.0.0.1\r\n" + Basic() + "Connection: close\r\n\r\n";

         var holders = new List<TcpClient>();
         try
         {
            for (int i = 0; i < Slots; i++)
            {
               var client = new TcpClient();
               client.ReceiveBufferSize = 8192;
               client.Connect("127.0.0.1", RestPort);
               holders.Add(client);

               NetworkStream stream = client.GetStream();
               Send(stream, request);

               // The head, and no more: the rest waits in a window of 8 KB,
               // which is what a reader that has stopped looks like.
               stream.ReadTimeout = 30000;
               var head = new StringBuilder();
               byte[] one = new byte[1];
               while (!head.ToString().EndsWith("\r\n\r\n", StringComparison.Ordinal))
               {
                  Assert.AreEqual(1, stream.Read(one, 0, 1), "Download " + i + " ended before its head: " + head);
                  head.Append((char) one[0]);
               }

               StringAssert.StartsWith("HTTP/1.1 200", head.ToString(), "Download " + i + " was not started.");
            }

            using (TcpClient extra = Connect(RestPort))
            using (NetworkStream stream = extra.GetStream())
            {
               Send(stream, request);
               HttpReply refused = new HttpReader(stream).Read();
               Assert.AreEqual(503, refused.Status, "A seventeenth large download was written beside the sixteen: " + refused.StatusLine);
               Assert.AreEqual("30", refused.Header("Retry-After"));
            }
         }
         finally
         {
            foreach (TcpClient holder in holders)
               holder.Close();
         }

         // The slots come back as the server sees the sixteen go.
         int status = 0;
         var stopwatch = Stopwatch.StartNew();
         while (stopwatch.ElapsedMilliseconds < 20000)
         {
            using (TcpClient again = Connect(RestPort))
            using (NetworkStream stream = again.GetStream())
            {
               Send(stream, request);
               var reader = new HttpReader(stream);
               stream.ReadTimeout = 60000;
               HttpReply reply = reader.Read();
               status = reply.Status;
               if (status == 200)
               {
                  Assert.Greater(reply.Body.Length, LargeMessageBytes - 1, "The whole message was sent.");
                  break;
               }
            }

            Thread.Sleep(500);
         }

         Assert.AreEqual(200, status, "The slots the sixteen held were not given back.");
      }

      [Test]
      [Description("Connections that send half a request do not delay a complete request from someone else")]
      public void ASlowClientDoesNotHoldTheListener()
      {
         var slow = new List<TcpClient>();
         try
         {
            // Six connections, each stuck half-way through its request line.
            // Against the serial listener this replaces, the seventh request
            // below would have waited behind the first of them for the whole
            // read timeout.
            for (int i = 0; i < 6; i++)
            {
               TcpClient client = Connect(RestPort);
               slow.Add(client);
               Send(client.GetStream(), "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n");
            }

            var stopwatch = Stopwatch.StartNew();

            using (TcpClient client = Connect(RestPort))
            using (NetworkStream stream = client.GetStream())
            {
               var reader = new HttpReader(stream);
               Send(stream, Request("GET", "/api/v1/status", "HTTP/1.1", Basic()));
               HttpReply reply = reader.Read();

               Assert.AreEqual(200, reply.Status, "Body: " + reply.Body);
            }

            stopwatch.Stop();
            Assert.Less(stopwatch.ElapsedMilliseconds, 10000,
               "A complete request must be answered while other connections are still mid-request, not after their deadline.");
         }
         finally
         {
            foreach (TcpClient client in slow)
               client.Close();
         }
      }

      [Test]
      [Description("A connection that never finishes its request is closed when the absolute request deadline passes")]
      public void ASlowRequestIsClosedAtTheDeadline()
      {
         using (TcpClient client = Connect(WebPort))
         using (NetworkStream stream = client.GetStream())
         {
            Send(stream, "GET /.well-known/security.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n");

            var stopwatch = Stopwatch.StartNew();
            stream.ReadTimeout = (WebRequestSeconds + 20) * 1000;

            bool closed;
            try
            {
               closed = stream.Read(new byte[1], 0, 1) == 0;
            }
            catch (IOException)
            {
               // A reset is a close too.
               closed = true;
            }

            stopwatch.Stop();

            Assert.IsTrue(closed, "The server must close a connection whose request never completes.");
            Assert.Less(stopwatch.ElapsedMilliseconds, (WebRequestSeconds + 15) * 1000,
               "The close must come from the request deadline, not from the client's own read timeout.");
         }
      }

      [Test]
      [Description("The web services listener keeps a connection alive as well")]
      public void TheWebServicesListenerKeepsAliveToo()
      {
         using (TcpClient client = Connect(WebPort))
         using (NetworkStream stream = client.GetStream())
         {
            var reader = new HttpReader(stream);

            Send(stream, Request("GET", "/no-such-path", "HTTP/1.1", ""));
            HttpReply first = reader.Read();
            Assert.AreEqual(404, first.Status, "Body: " + first.Body);
            Assert.AreEqual("keep-alive", first.Header("connection"));

            // The web services listener's default: not cacheable unless a
            // handler says so. A refusal never says so.
            Assert.AreEqual("no-store", first.Header("cache-control"));

            Send(stream, Request("GET", "/no-such-path", "HTTP/1.1", ""));
            HttpReply second = reader.Read();
            Assert.AreEqual(404, second.Status, "Body: " + second.Body);
         }
      }

      [Test]
      [Description("Several clients issuing many requests each are all answered, concurrently")]
      public void ConcurrentClientsAreAllServed()
      {
         const int clients = 8;
         const int requestsPerClient = 20;

         var failures = new List<string>();
         var threads = new List<Thread>();
         var stopwatch = Stopwatch.StartNew();

         for (int c = 0; c < clients; c++)
         {
            int clientIndex = c;
            var thread = new Thread(() =>
            {
               try
               {
                  using (TcpClient client = Connect(RestPort))
                  using (NetworkStream stream = client.GetStream())
                  {
                     var reader = new HttpReader(stream);
                     for (int r = 0; r < requestsPerClient; r++)
                     {
                        Send(stream, Request("GET", "/api/v1/status", "HTTP/1.1", Basic()));
                        HttpReply reply = reader.Read();
                        if (reply.Status != 200)
                        {
                           lock (failures)
                              failures.Add("client " + clientIndex + " request " + r + ": " + reply.StatusLine);
                           return;
                        }
                     }
                  }
               }
               catch (Exception ex) when (ex is IOException || ex is SocketException)
               {
                  lock (failures)
                     failures.Add("client " + clientIndex + ": " + ex.Message);
               }
            });

            threads.Add(thread);
            thread.Start();
         }

         foreach (Thread thread in threads)
            thread.Join();

         stopwatch.Stop();

         Assert.IsEmpty(failures, string.Join("\n", failures));
         Assert.Less(stopwatch.ElapsedMilliseconds, 60000);
      }

      // ------------------------------------------------------------ helpers ---

      // A message of LargeMessageBytes in a folder of a new account
      // <localPart>@<the test domain>, made through the API; its id.
      private const int LargeMessageBytes = 12 * 1024 * 1024;

      private string CreateLargeMessage(string localPart)
      {
         string address = localPart + "@" + _domain.Name;
         hMailServer.Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, "Large-Message-1234");
         var folder = account.IMAPFolders.Add("Big");
         folder.Save();

         var raw = new StringBuilder(LargeMessageBytes + 4096);
         raw.Append("From: sender@example.com\r\nTo: ").Append(address).Append("\r\nSubject: Big\r\n")
            .Append("Date: Sat, 26 Sep 2026 10:00:00 +0000\r\n\r\n");
         string line = new string('b', 998) + "\r\n";
         while (raw.Length < LargeMessageBytes)
            raw.Append(line);

         string created = Exchange("POST", "/api/v1/accounts/" + address + "/folders/" + folder.ID + "/messages", raw.ToString());
         StringAssert.StartsWith("HTTP/1.1 201", created, "The message was not created.");
         int idAt = created.IndexOf("\"id\":", StringComparison.Ordinal);
         Assert.IsTrue(idAt > 0, created);
         int idEnd = idAt + 5;
         while (idEnd < created.Length && char.IsDigit(created[idEnd]))
            idEnd++;
         return created.Substring(idAt + 5, idEnd - idAt - 5);
      }

      private static string Basic()
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return "Authorization: Basic " + credentials + "\r\n";
      }

      private static string Request(string method, string path, string version, string extraHeaders)
      {
         return method + " " + path + " " + version + "\r\nHost: 127.0.0.1\r\n" + extraHeaders + "\r\n";
      }

      // One request as the administrator, with a body, on a connection of its
      // own; the whole of what came back, read to the server's close.
      private static string Exchange(string method, string path, string body)
      {
         using (TcpClient client = Connect(RestPort))
         using (NetworkStream stream = client.GetStream())
         {
            byte[] bodyBytes = Encoding.ASCII.GetBytes(body);
            Send(stream, method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + Basic() +
                         "Content-Type: application/json\r\nContent-Length: " + bodyBytes.Length + "\r\nConnection: close\r\n\r\n");
            stream.Write(bodyBytes, 0, bodyBytes.Length);

            stream.ReadTimeout = 60000;
            var received = new MemoryStream();
            byte[] buffer = new byte[16 * 1024];
            int read;
            while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
               received.Write(buffer, 0, read);

            return Encoding.UTF8.GetString(received.ToArray());
         }
      }

      private static TcpClient Connect(int port)
      {
         var client = new TcpClient();
         Exception last = null;

         for (int attempt = 0; attempt < 25; attempt++)
         {
            try
            {
               client.Connect("127.0.0.1", port);
               return client;
            }
            catch (SocketException ex)
            {
               last = ex;
               Thread.Sleep(200);
            }
         }

         client.Close();
         throw last;
      }

      private static void Send(NetworkStream stream, string text)
      {
         byte[] bytes = Encoding.ASCII.GetBytes(text);
         stream.Write(bytes, 0, bytes.Length);
      }

      private sealed class HttpReply
      {
         public string StatusLine = "";
         public int Status;
         public readonly Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         public string Body = "";

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : "";
         }
      }

      /// <summary>
      ///    Reads responses one at a time from a stream that may carry several,
      ///    framing each by its Content-Length - which is what keep-alive and
      ///    pipelining require of a client, and what an EOF-framed reader cannot
      ///    do.
      /// </summary>
      private sealed class HttpReader
      {
         private readonly NetworkStream stream_;
         private readonly List<byte> buffered_ = new List<byte>();
         private bool eof_;

         public HttpReader(NetworkStream stream)
         {
            stream_ = stream;
            stream_.ReadTimeout = 15000;
         }

         public HttpReply Read()
         {
            var reply = new HttpReply();

            byte[] head = ReadUntil(Encoding.ASCII.GetBytes("\r\n\r\n"));
            if (head == null)
            {
               reply.StatusLine = "(connection closed before a response)";
               return reply;
            }

            string headText = Encoding.ASCII.GetString(head);
            string[] lines = headText.Split(new[] { "\r\n" }, StringSplitOptions.None);

            reply.StatusLine = lines[0];
            string[] parts = lines[0].Split(' ');
            if (parts.Length >= 2)
               int.TryParse(parts[1], out reply.Status);

            for (int i = 1; i < lines.Length; i++)
            {
               int colon = lines[i].IndexOf(':');
               if (colon > 0)
                  reply.Headers[lines[i].Substring(0, colon).Trim()] = lines[i].Substring(colon + 1).Trim();
            }

            // A response with no Content-Length, or one that is not a number, is read
            // as a response with no body - which is what the framing tests here expect
            // of a 204 or a HEAD.
            int length = 0;
            string declaredLength;
            if (reply.Headers.TryGetValue("Content-Length", out declaredLength))
               int.TryParse(declaredLength, out length);

            byte[] body = ReadExactly(length);
            reply.Body = Encoding.UTF8.GetString(body);

            return reply;
         }

         // True when the server has closed the connection and nothing is left
         // to read.
         public bool AtEnd()
         {
            if (buffered_.Count > 0)
               return false;

            Fill();
            return eof_ && buffered_.Count == 0;
         }

         private byte[] ReadUntil(byte[] delimiter)
         {
            for (;;)
            {
               int at = IndexOf(delimiter);
               if (at >= 0)
               {
                  int end = at + delimiter.Length;
                  byte[] result = buffered_.GetRange(0, end).ToArray();
                  buffered_.RemoveRange(0, end);
                  return result;
               }

               if (eof_)
                  return null;

               Fill();
            }
         }

         private byte[] ReadExactly(int count)
         {
            while (buffered_.Count < count && !eof_)
               Fill();

            int take = Math.Min(count, buffered_.Count);
            byte[] result = buffered_.GetRange(0, take).ToArray();
            buffered_.RemoveRange(0, take);
            return result;
         }

         private void Fill()
         {
            if (eof_)
               return;

            byte[] chunk = new byte[8192];
            int read;
            try
            {
               read = stream_.Read(chunk, 0, chunk.Length);
            }
            catch (IOException)
            {
               // A reset or a timeout: nothing more will come.
               read = 0;
            }

            if (read <= 0)
            {
               eof_ = true;
               return;
            }

            for (int i = 0; i < read; i++)
               buffered_.Add(chunk[i]);
         }

         private int IndexOf(byte[] delimiter)
         {
            for (int i = 0; i + delimiter.Length <= buffered_.Count; i++)
            {
               bool match = true;
               for (int j = 0; j < delimiter.Length; j++)
               {
                  if (buffered_[i + j] != delimiter[j])
                  {
                     match = false;
                     break;
                  }
               }

               if (match)
                  return i;
            }

            return -1;
         }
      }
   }
}
