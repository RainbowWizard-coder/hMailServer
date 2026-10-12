// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    How the simulator answers a PROCESS request.
   /// </summary>
   public enum SpamdSimulatorMode
   {
      /// <summary>
      ///    As spamd does for a clean message: SPAMD/1.1 0 EX_OK, a Content-length, and
      ///    the message back (through <see cref="SpamdSimulator.Reply" />).
      /// </summary>
      Answer,

      /// <summary>
      ///    The whole request read, then a reply whose Content-length promises more than
      ///    is sent: half the message, and the connection closed in good order.
      /// </summary>
      CutShort,

      /// <summary>
      ///    The whole request read, then no reply: the connection is reset.
      /// </summary>
      ResetAfterRequest,

      /// <summary>
      ///    The request's header and the first <see cref="SpamdSimulator.HangUpAfterBytes" />
      ///    bytes of its message read, then the connection reset while the server is still
      ///    sending.
      /// </summary>
      HangUpMidRequest
   }

   /// <summary>
   ///    A stand-in for spamd that records what the server sends it.
   ///
   ///    The SpamAssassin fixtures run against the real spamd on port 783, which is the
   ///    right thing for verdicts: nothing but SpamAssassin can say what SpamAssassin
   ///    would score. It is the wrong thing for the request, because the real one keeps
   ///    no record of the headers it was sent, and a test that wants to know whether a
   ///    User: header went out has nothing to read. This listens on an ephemeral port,
   ///    accepts one PROCESS request per connection, keeps the request headers and the
   ///    message bytes, and by default answers as spamd would for a clean message:
   ///    SPAMD/1.1 0 EX_OK, a Content-length, and the message handed back unchanged. The
   ///    server then treats the exchange as a completed scan. <see cref="Mode" /> makes it
   ///    fail instead, part-way, and <see cref="Reply" /> changes the message it returns.
   /// </summary>
   public sealed class SpamdSimulator : IDisposable
   {
      private readonly TcpListener _listener;
      private readonly Thread _thread;
      private readonly List<Dictionary<string, string>> _requests = new List<Dictionary<string, string>>();
      private readonly List<byte[]> _messages = new List<byte[]>();
      private readonly object _lock = new object();
      private volatile bool _stopping;

      public SpamdSimulator()
      {
         _listener = new TcpListener(IPAddress.Loopback, 0);
         _listener.Start();
         _thread = new Thread(Serve) { IsBackground = true, Name = "SpamdSimulator" };
         _thread.Start();
      }

      public int Port
      {
         get { return ((IPEndPoint) _listener.LocalEndpoint).Port; }
      }

      /// <summary>
      ///    How a PROCESS request is answered. Set before the server connects.
      /// </summary>
      public SpamdSimulatorMode Mode { get; set; } = SpamdSimulatorMode.Answer;

      /// <summary>
      ///    The message sent back for the message received, as spamd sends the message as
      ///    SpamAssassin left it. Null sends it back unchanged.
      /// </summary>
      public Func<byte[], byte[]> Reply { get; set; }

      /// <summary>
      ///    For <see cref="SpamdSimulatorMode.HangUpMidRequest" />: the bytes of the message
      ///    read before the connection is reset.
      /// </summary>
      public int HangUpAfterBytes { get; set; } = 100;

      /// <summary>
      ///    The headers of every PROCESS request received so far, oldest first, keyed by
      ///    header name as sent (User, Content-length, ...).
      /// </summary>
      public List<Dictionary<string, string>> Requests
      {
         get
         {
            lock (_lock)
               return new List<Dictionary<string, string>>(_requests);
         }
      }

      /// <summary>
      ///    Waits for the given number of requests to have been served, which is how a
      ///    test knows the scan the server was asked to run has actually happened.
      /// </summary>
      public Dictionary<string, string> WaitForRequest(int index, int timeoutSeconds = 30)
      {
         var deadline = DateTime.UtcNow.AddSeconds(timeoutSeconds);
         while (DateTime.UtcNow < deadline)
         {
            lock (_lock)
            {
               if (_requests.Count > index)
                  return _requests[index];
            }
            Thread.Sleep(50);
         }

         throw new TimeoutException("spamd simulator received " + Requests.Count + " request(s); waited for " + (index + 1) + ".");
      }

      /// <summary>
      ///    The bytes of the message of the given request, exactly as they arrived after
      ///    the request's empty line - as many as its Content-length counted, or as the
      ///    simulator read before it hung up.
      /// </summary>
      public byte[] WaitForMessage(int index, int timeoutSeconds = 30)
      {
         WaitForRequest(index, timeoutSeconds);

         lock (_lock)
            return _messages[index];
      }

      public void Dispose()
      {
         _stopping = true;
         _listener.Stop();
         _thread.Join(TimeSpan.FromSeconds(5));
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
               // Dispose stopped the listener under the pending accept.
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
                  stream.ReadTimeout = 30000;
                  if (!HandleRequest(stream))
                  {
                     // A reset rather than an orderly close: no FIN, and whatever the
                     // server is still sending is refused.
                     client.Client.LingerState = new LingerOption(true, 0);
                  }
               }
            }
            catch (Exception ex) when (ex is IOException || ex is SocketException || ex is ObjectDisposedException)
            {
               // A connection the server dropped mid-request is not this simulator's
               // failure to report; the test sees it as a request that never arrived.
            }
         }
      }

      // False when the connection is to be reset rather than closed.
      private bool HandleRequest(NetworkStream stream)
      {
         // Request line and headers, terminated by an empty line.
         string headerText;
         using (var headerBytes = new MemoryStream())
         {
            var window = new byte[4];
            while (true)
            {
               int b = stream.ReadByte();
               if (b < 0)
                  return true;
               headerBytes.WriteByte((byte) b);
               window[0] = window[1]; window[1] = window[2]; window[2] = window[3]; window[3] = (byte) b;
               if (window[0] == '\r' && window[1] == '\n' && window[2] == '\r' && window[3] == '\n')
                  break;
            }

            headerText = Encoding.ASCII.GetString(headerBytes.ToArray());
         }
         var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         var lines = headerText.Split(new[] {"\r\n"}, StringSplitOptions.RemoveEmptyEntries);
         headers["Request"] = lines[0];
         for (int i = 1; i < lines.Length; i++)
         {
            int colon = lines[i].IndexOf(':');
            if (colon > 0)
               headers[lines[i].Substring(0, colon).Trim()] = lines[i].Substring(colon + 1).Trim();
         }

         long contentLength = 0;
         string lengthText;
         if (headers.TryGetValue("Content-length", out lengthText))
            long.TryParse(lengthText, out contentLength);

         bool isTell = headers["Request"].StartsWith("TELL", StringComparison.OrdinalIgnoreCase);
         bool hangUp = !isTell && Mode == SpamdSimulatorMode.HangUpMidRequest;

         long toRead = hangUp ? Math.Min(contentLength, HangUpAfterBytes) : contentLength;
         var body = new byte[toRead];
         int read = 0;
         while (read < toRead)
         {
            int n = stream.Read(body, read, (int) (toRead - read));
            if (n <= 0)
               break;
            read += n;
         }

         var message = new byte[read];
         Array.Copy(body, message, read);

         lock (_lock)
         {
            _messages.Add(message);
            _requests.Add(headers);
         }

         // TELL is acknowledged the way spamd acknowledges it: headers only, no
         // message back, DidSet naming the store that changed.
         if (isTell)
         {
            var acknowledgement = Encoding.ASCII.GetBytes("SPAMD/1.1 0 EX_OK\r\nDidSet: local\r\n\r\n");
            stream.Write(acknowledgement, 0, acknowledgement.Length);
            stream.Flush();
            return true;
         }

         if (hangUp || Mode == SpamdSimulatorMode.ResetAfterRequest)
            return false;

         // The reply spamd gives for PROCESS on a message it did not tag: the message
         // back, behind the headers the server parses.
         var replyMessage = Reply != null ? Reply(message) : message;

         if (Mode == SpamdSimulatorMode.CutShort)
         {
            // Promised whole, sent in half, and the connection closed in good order.
            var promise = Encoding.ASCII.GetBytes("SPAMD/1.1 0 EX_OK\r\nContent-length: " + (replyMessage.Length + 100) + "\r\nSpam: False ; 0.0 / 5.0\r\n\r\n");
            stream.Write(promise, 0, promise.Length);
            stream.Write(replyMessage, 0, replyMessage.Length / 2);
            stream.Flush();
            return true;
         }

         var reply = "SPAMD/1.1 0 EX_OK\r\nContent-length: " + replyMessage.Length + "\r\nSpam: False ; 0.0 / 5.0\r\n\r\n";
         var replyBytes = Encoding.ASCII.GetBytes(reply);
         stream.Write(replyBytes, 0, replyBytes.Length);
         stream.Write(replyMessage, 0, replyMessage.Length);
         stream.Flush();
         return true;
      }
   }
}
