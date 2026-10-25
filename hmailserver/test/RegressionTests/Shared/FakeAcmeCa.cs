// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    An ACME CA (RFC 8555) on the loopback address, over plain HTTP, which is the
   ///    one form the server's ACME client takes besides HTTPS: a URL on this machine
   ///    has no network path to protect (AcmeClient.cpp, ParseAcmeUrl). It answers the
   ///    steps the client takes - the directory, a nonce, the account, the order, an
   ///    authorization that is already valid, the finalization, the order's poll and
   ///    the certificate - and does no cryptography at all: the client's signatures are
   ///    not checked, and the certificate it hands out is whatever the test gave it.
   ///
   ///    The order is where a test makes it misbehave: answered with a rate limit
   ///    (urn:ietf:params:acme:error:rateLimited and a Retry-After), refused with
   ///    another problem (urn:ietf:params:acme:error:rejectedIdentifier, 400), or held
   ///    open for some seconds and closed with no answer, which is a CA that stopped
   ///    answering. The directory can be held so once (HoldNextDirectory), which holds
   ///    a check that finds nothing due by the names where it asks the CA for its
   ///    renewal window. Every response carries a fresh Replay-Nonce, as a CA's do. A
   ///    raw socket, as FakeHttpEndpoint is, for the reason it gives.
   /// </summary>
   public sealed class FakeAcmeCa : IDisposable
   {
      public enum OrderMode
      {
         Issue,
         RateLimited,
         Rejected,
         Stall
      }

      private readonly TcpListener _listener;
      private readonly object _lock = new object();
      private readonly List<string> _requests = new List<string>();
      private volatile bool _stopping;
      private int _nonce;
      private int _holdNextDirectorySeconds;
      private int _directoriesHeld;

      public FakeAcmeCa(int port)
      {
         _listener = new TcpListener(IPAddress.Loopback, port);
         _listener.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
         _listener.Start();
         Port = ((IPEndPoint) _listener.LocalEndpoint).Port;
         Mode = OrderMode.Issue;
         RetryAfter = "3600";
         StallSeconds = 10;
         new Thread(Serve) { IsBackground = true, Name = "FakeAcmeCa" }.Start();
      }

      public int Port { get; }

      public string Base => "http://127.0.0.1:" + Port;

      public string DirectoryUrl => Base + "/directory";

      /// <summary>What the order is answered with.</summary>
      public OrderMode Mode { get; set; }

      /// <summary>The Retry-After a rate limit is answered with; null for none.</summary>
      public string RetryAfter { get; set; }

      /// <summary>How long a stalled order is held before the connection is closed.</summary>
      public int StallSeconds { get; set; }

      /// <summary>The PEM chain the certificate URL answers with.</summary>
      public string CertificatePem { get; set; }

      /// <summary>The host the authorization says it is for.</summary>
      public string AuthorizedName { get; set; } = "new.example.test";

      /// <summary>Every request line received, oldest first.</summary>
      public List<string> Requests
      {
         get
         {
            lock (_lock)
               return new List<string>(_requests);
         }
      }

      public int Count(string method, string path)
      {
         lock (_lock)
            return _requests.Count(line => line == method + " " + path);
      }

      /// <summary>
      ///    The next request for the directory, and that one alone, is held this many
      ///    seconds and closed with no answer; those after it are answered.
      /// </summary>
      public void HoldNextDirectory(int seconds)
      {
         Interlocked.Exchange(ref _holdNextDirectorySeconds, seconds);
      }

      /// <summary>How many requests for the directory have been held so far.</summary>
      public int DirectoriesHeld => Volatile.Read(ref _directoriesHeld);

      public void Dispose()
      {
         _stopping = true;
         _listener.Stop();
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
            catch (SocketException)
            {
               return;
            }
            catch (ObjectDisposedException)
            {
               return;
            }

            // Each on a thread of its own, so that a stalled order does not hold the
            // status reads and the nonces of anything else that asks meanwhile.
            new Thread(() =>
            {
               try
               {
                  using (client)
                  using (NetworkStream stream = client.GetStream())
                  {
                     stream.ReadTimeout = 10000;
                     Handle(stream);
                  }
               }
               catch (Exception ex) when (ex is IOException || ex is SocketException || ex is ObjectDisposedException)
               {
                  // A request the client abandoned is not the CA's failure to report.
               }
            }) { IsBackground = true, Name = "FakeAcmeCa connection" }.Start();
         }
      }

      private void Handle(NetworkStream stream)
      {
         var text = new StringBuilder();
         var buffer = new byte[4096];
         int headerEnd = -1;
         while (headerEnd < 0)
         {
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read <= 0)
               return;
            text.Append(Encoding.ASCII.GetString(buffer, 0, read));
            headerEnd = text.ToString().IndexOf("\r\n\r\n", StringComparison.Ordinal);
         }

         int contentLength = 0;
         foreach (string line in text.ToString(0, headerEnd).Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
               int.TryParse(line.Substring(15).Trim(), out contentLength);
         }

         while (text.Length - (headerEnd + 4) < contentLength)
         {
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read <= 0)
               break;
            text.Append(Encoding.ASCII.GetString(buffer, 0, read));
         }

         string requestLine = text.ToString().Split(new[] { "\r\n" }, StringSplitOptions.None)[0];
         string[] parts = requestLine.Split(' ');
         string method = parts.Length > 0 ? parts[0] : "";
         string path = parts.Length > 1 ? parts[1] : "";

         lock (_lock)
            _requests.Add(method + " " + path);

         int status = 404;
         string body = "{\"type\":\"urn:ietf:params:acme:error:malformed\",\"detail\":\"no such resource\"}";
         string contentType = "application/problem+json";
         var headers = new List<string>();

         switch (path)
         {
         case "/directory":
            int hold = Interlocked.Exchange(ref _holdNextDirectorySeconds, 0);
            if (hold > 0)
            {
               Interlocked.Increment(ref _directoriesHeld);
               DateTime heldUntil = DateTime.UtcNow.AddSeconds(hold);
               while (DateTime.UtcNow < heldUntil && !_stopping)
                  Thread.Sleep(100);
               return;
            }

            status = 200;
            contentType = "application/json";
            body = "{\"newNonce\":\"" + Base + "/nonce\",\"newAccount\":\"" + Base + "/account\",\"newOrder\":\"" + Base + "/order\"}";
            break;

         case "/nonce":
            status = 200;
            contentType = "text/plain";
            body = "";
            break;

         case "/account":
            status = 201;
            contentType = "application/json";
            headers.Add("Location: " + Base + "/acct/1");
            body = "{\"status\":\"valid\"}";
            break;

         case "/order":
            if (Mode == OrderMode.Stall)
            {
               DateTime until = DateTime.UtcNow.AddSeconds(StallSeconds);
               while (DateTime.UtcNow < until && !_stopping)
                  Thread.Sleep(100);
               return;
            }

            if (Mode == OrderMode.RateLimited)
            {
               status = 429;
               contentType = "application/problem+json";
               if (RetryAfter != null)
                  headers.Add("Retry-After: " + RetryAfter);
               body = "{\n  \"type\": \"urn:ietf:params:acme:error:rateLimited\",\n  \"detail\": \"too many certificates (5) already issued for this exact set of identifiers in the last 168h0m0s\",\n  \"status\": 429\n}";
               break;
            }

            if (Mode == OrderMode.Rejected)
            {
               // A refusal that is not a rate limit, with a Retry-After beside it that
               // a client must not read as one.
               status = 400;
               contentType = "application/problem+json";
               headers.Add("Retry-After: 86400");
               body = "{\n  \"type\": \"urn:ietf:params:acme:error:rejectedIdentifier\",\n  \"detail\": \"Error creating new order :: Cannot issue for \\\"new.example.test\\\": Domain name does not end with a valid public suffix (TLD)\",\n  \"status\": 400\n}";
               break;
            }

            status = 201;
            contentType = "application/json";
            headers.Add("Location: " + Base + "/order/1");
            body = "{\"status\":\"pending\",\"authorizations\":[\"" + Base + "/authz/1\"],\"finalize\":\"" + Base + "/finalize/1\"}";
            break;

         case "/authz/1":
            status = 200;
            contentType = "application/json";
            body = "{\"identifier\":{\"type\":\"dns\",\"value\":\"" + AuthorizedName + "\"},\"status\":\"valid\",\"challenges\":[]}";
            break;

         case "/finalize/1":
            status = 200;
            contentType = "application/json";
            body = "{\"status\":\"processing\"}";
            break;

         case "/order/1":
            status = 200;
            contentType = "application/json";
            body = "{\"status\":\"valid\",\"certificate\":\"" + Base + "/cert/1\"}";
            break;

         case "/cert/1":
            status = CertificatePem == null ? 404 : 200;
            contentType = "application/pem-certificate-chain";
            body = CertificatePem ?? "";
            break;
         }

         int nonce = Interlocked.Increment(ref _nonce);
         headers.Add("Replay-Nonce: fake-nonce-" + nonce);

         byte[] bodyBytes = Encoding.ASCII.GetBytes(body);
         string reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 429 ? "Too Many Requests" : status == 400 ? "Bad Request" : "Not Found";
         var head = new StringBuilder();
         head.Append("HTTP/1.0 " + status + " " + reason + "\r\n");
         head.Append("Content-Type: " + contentType + "\r\n");
         foreach (string header in headers)
            head.Append(header + "\r\n");
         head.Append("Content-Length: " + bodyBytes.Length + "\r\nConnection: close\r\n\r\n");

         byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
         stream.Write(headBytes, 0, headBytes.Length);
         stream.Write(bodyBytes, 0, bodyBytes.Length);
         stream.Flush();
      }
   }
}
