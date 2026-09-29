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
   ///    An HTTP endpoint on the loopback address that answers with whatever the test
   ///    told it to and records every request it was sent.
   ///
   ///    A raw socket rather than HttpListener, for the reason FakeFilterEngine gives:
   ///    HttpListener needs a URL ACL reservation an unelevated run does not have, and
   ///    reading one request and answering it is thirty lines. It stands in for an
   ///    identity provider's JWK Set and introspection endpoints, which is why the
   ///    response can be changed between requests - a key rotation is the provider
   ///    publishing a different document at the same URL.
   /// </summary>
   public sealed class FakeHttpEndpoint : IDisposable
   {
      private readonly TcpListener _listener;
      private readonly List<string> _requests = new List<string>();
      private readonly List<byte[]> _bodies = new List<byte[]>();
      private readonly object _lock = new object();
      private volatile bool _stopping;
      private int _statusCode;
      private string _body;
      private string _contentType;
      private readonly Dictionary<string, Route> _routes = new Dictionary<string, Route>(StringComparer.Ordinal);
      private readonly Dictionary<string, Queue<Route>> _sequences = new Dictionary<string, Queue<Route>>(StringComparer.Ordinal);
      private readonly Dictionary<string, int> _stalls = new Dictionary<string, int>(StringComparer.Ordinal);
      private readonly Dictionary<string, byte[]> _raw = new Dictionary<string, byte[]>(StringComparer.Ordinal);

      private sealed class Route
      {
         public int Status;
         public byte[] Body;
         public string ContentType;
         public string Location;
      }

      public FakeHttpEndpoint(int statusCode, string body, string contentType = "application/json")
      {
         _statusCode = statusCode;
         _body = body;
         _contentType = contentType;
         _listener = new TcpListener(IPAddress.Loopback, 0);
         _listener.Start();
         Port = ((IPEndPoint) _listener.LocalEndpoint).Port;
         new Thread(Serve) {IsBackground = true, Name = "FakeHttpEndpoint"}.Start();
      }

      /// <summary>
      ///    The same endpoint on a port the test names, for a fixture whose port was
      ///    allocated to it rather than taken from the ephemeral range - so that the
      ///    address a setting stores, and the address nothing is listening on once the
      ///    endpoint is disposed, is known before the endpoint exists. The content type
      ///    is required here so that this overload and the ephemeral one can never be
      ///    confused by an int where a status was meant.
      /// </summary>
      public FakeHttpEndpoint(int port, int statusCode, string body, string contentType)
         : this(IPAddress.Loopback, port, statusCode, body, contentType)
      {
      }

      /// <summary>
      ///    The same on a loopback address the test names - IPAddress.IPv6Loopback, for an
      ///    endpoint the server must reach as http://[::1]:port/.
      /// </summary>
      public FakeHttpEndpoint(IPAddress address, int port, int statusCode, string body, string contentType)
      {
         _statusCode = statusCode;
         _body = body;
         _contentType = contentType;
         _listener = new TcpListener(address, port);
         // A previous listener's connections may still be in TIME_WAIT on this port.
         _listener.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
         _listener.Start();
         Port = ((IPEndPoint) _listener.LocalEndpoint).Port;
         new Thread(Serve) {IsBackground = true, Name = "FakeHttpEndpoint"}.Start();
      }

      public int Port { get; }

      public string UrlFor(string path)
      {
         return "http://127.0.0.1:" + Port + path;
      }

      /// <summary>Every request received - request line, headers and body - oldest first.</summary>
      public List<string> Requests
      {
         get
         {
            lock (_lock)
               return new List<string>(_requests);
         }
      }

      /// <summary>
      ///    The body of every request, as the bytes that arrived, in the same order
      ///    as Requests. Requests decodes as ASCII, which is what a signature over a
      ///    JSON body needs and what an encrypted Web Push body is destroyed by.
      /// </summary>
      public List<byte[]> RequestBodies
      {
         get
         {
            lock (_lock)
               return new List<byte[]>(_bodies);
         }
      }

      /// <summary>
      ///    What requests for one exact path are answered with, in bytes: a release's
      ///    installer and its bundle live at different paths on the same host. Paths
      ///    without a route get the default response.
      /// </summary>
      public void SetResponse(string path, int statusCode, byte[] body, string contentType = "application/octet-stream")
      {
         lock (_lock)
            _routes[path] = new Route {Status = statusCode, Body = body, ContentType = contentType};
      }

      /// <summary>A 302 from one path to a location, as a release asset download is.</summary>
      public void SetRedirect(string path, string location)
      {
         lock (_lock)
            _routes[path] = new Route {Status = 302, Body = new byte[0], ContentType = "text/plain", Location = location};
      }

      /// <summary>
      ///    What requests for one exact path are answered with, one after another: the
      ///    first request gets the first answer, the second the second, and every request
      ///    after the last gets the last again. A sandbox's verdict route answering
      ///    pending, pending and then done is what it is for. Wins over SetResponse for
      ///    the same path; the query is not part of the path.
      /// </summary>
      public void SetSequence(string path, params (int status, string body)[] answers)
      {
         lock (_lock)
         {
            var queue = new Queue<Route>();
            foreach (var answer in answers)
               queue.Enqueue(new Route {Status = answer.status, Body = Encoding.UTF8.GetBytes(answer.body ?? ""), ContentType = "application/json"});
            _sequences[path] = queue;
         }
      }

      /// <summary>
      ///    Requests for one exact path are read, recorded and then left unanswered
      ///    for so many seconds, after which the connection is closed with no answer
      ///    at all: a service that accepts a connection and then hangs. The endpoint
      ///    serves one connection at a time, so nothing else is answered meanwhile
      ///    either - which is what a stalled service looks like from outside. 0
      ///    answers the path again.
      /// </summary>
      public void SetStall(string path, int seconds)
      {
         lock (_lock)
         {
            if (seconds > 0)
               _stalls[path] = seconds;
            else
               _stalls.Remove(path);
         }
      }

      /// <summary>
      ///    Requests for one exact path are read, recorded and answered with exactly
      ///    these bytes, whatever they are - a status line and headers cut short,
      ///    say - and then the connection is closed: a service that ends its answer
      ///    part-way. null answers the path again.
      /// </summary>
      public void SetRawAnswer(string path, byte[] bytes)
      {
         lock (_lock)
         {
            if (bytes != null)
               _raw[path] = bytes;
            else
               _raw.Remove(path);
         }
      }

      /// <summary>How many requests with this method were made for one exact path.</summary>
      public int CountRequests(string method, string path)
      {
         lock (_lock)
            return _requests.Count(request => RequestMethod(request) == method && RequestPath(request) == path);
      }

      public void ClearRoutes()
      {
         lock (_lock)
         {
            _routes.Clear();
            _sequences.Clear();
            _stalls.Clear();
            _raw.Clear();
         }
      }

      /// <summary>What the next requests are answered with.</summary>
      public void SetResponse(int statusCode, string body, string contentType = "application/json")
      {
         lock (_lock)
         {
            _statusCode = statusCode;
            _body = body;
            _contentType = contentType;
         }
      }

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

            try
            {
               using (client)
               using (var stream = client.GetStream())
               {
                  stream.ReadTimeout = 10000;
                  Handle(stream);
               }
            }
            catch (Exception ex) when (ex is IOException || ex is SocketException || ex is ObjectDisposedException)
            {
               // A request the server abandoned is not this endpoint's failure to report.
            }
         }
      }

      private void Handle(NetworkStream stream)
      {
         var request = new StringBuilder();
         var raw = new MemoryStream();
         var buffer = new byte[4096];
         int headerEnd = -1;
         while (headerEnd < 0)
         {
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read <= 0)
               return;
            request.Append(Encoding.ASCII.GetString(buffer, 0, read));
            raw.Write(buffer, 0, read);
            headerEnd = request.ToString().IndexOf("\r\n\r\n", StringComparison.Ordinal);
         }

         // The body, when the headers announce one.
         int contentLength = 0;
         var headerLines = request.ToString(0, headerEnd).Split(new[] {"\r\n"}, StringSplitOptions.None)
            .Where(line => line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase));
         foreach (var line in headerLines)
            int.TryParse(line.Substring(15).Trim(), out contentLength);

         while (request.Length - (headerEnd + 4) < contentLength)
         {
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read <= 0)
               break;
            request.Append(Encoding.ASCII.GetString(buffer, 0, read));
            raw.Write(buffer, 0, read);
         }
         var text = request.ToString();

         // ASCII decoding is one char per byte, so the header's end in the text is
         // its end in the bytes.
         byte[] rawBytes = raw.ToArray();
         int bodyStart = Math.Min(headerEnd + 4, rawBytes.Length);
         var bodyBytes2 = new byte[rawBytes.Length - bodyStart];
         Array.Copy(rawBytes, bodyStart, bodyBytes2, 0, bodyBytes2.Length);

         int status;
         byte[] bodyBytes;
         string contentType;
         string location = null;
         string requestPath = RequestPath(text);
         int stall = 0;
         byte[] rawAnswer = null;
         lock (_lock)
         {
            _requests.Add(text);
            _bodies.Add(bodyBytes2);
            if (requestPath != null)
            {
               _stalls.TryGetValue(requestPath, out stall);
               _raw.TryGetValue(requestPath, out rawAnswer);
            }
         }

         if (stall > 0)
         {
            DateTime until = DateTime.UtcNow.AddSeconds(stall);
            while (DateTime.UtcNow < until && !_stopping)
               Thread.Sleep(100);
            return;
         }

         if (rawAnswer != null)
         {
            stream.Write(rawAnswer, 0, rawAnswer.Length);
            stream.Flush();
            return;
         }

         lock (_lock)
         {
            Route route;
            Queue<Route> sequence;
            if (requestPath != null && _sequences.TryGetValue(requestPath, out sequence) && sequence.Count > 0)
            {
               route = sequence.Count > 1 ? sequence.Dequeue() : sequence.Peek();
               status = route.Status;
               bodyBytes = route.Body;
               contentType = route.ContentType;
            }
            else if (requestPath != null && _routes.TryGetValue(requestPath, out route))
            {
               status = route.Status;
               bodyBytes = route.Body;
               contentType = route.ContentType;
               location = route.Location;
            }
            else
            {
               status = _statusCode;
               bodyBytes = Encoding.UTF8.GetBytes(_body ?? "");
               contentType = _contentType;
            }
         }

         var reason = status == 200 ? "OK" : status == 302 ? "Found" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : "Error";
         var head = "HTTP/1.0 " + status + " " + reason + "\r\nContent-Type: " + contentType +
                    (location == null ? "" : "\r\nLocation: " + location) +
                    "\r\nContent-Length: " + bodyBytes.Length + "\r\nConnection: close\r\n\r\n";
         var headBytes = Encoding.ASCII.GetBytes(head);
         stream.Write(headBytes, 0, headBytes.Length);
         stream.Write(bodyBytes, 0, bodyBytes.Length);
         stream.Flush();
      }

      // The method of the request line.
      private static string RequestMethod(string request)
      {
         int space = request.IndexOf(' ');
         return space < 0 ? null : request.Substring(0, space);
      }

      // The path of the request line, without any query.
      private static string RequestPath(string request)
      {
         int lineEnd = request.IndexOf("\r\n", StringComparison.Ordinal);
         string[] parts = (lineEnd < 0 ? request : request.Substring(0, lineEnd)).Split(' ');
         if (parts.Length < 2)
            return null;
         int query = parts[1].IndexOf('?');
         return query < 0 ? parts[1] : parts[1].Substring(0, query);
      }
   }
}
