// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    One HTTP/1.0 request against the REST listener, with the status and the
   ///    body read to the end.
   ///
   ///    Every REST fixture had a private copy of this, each with its own version
   ///    of the connect retry that absorbs the bind race right after a
   ///    reinitialize. The copies are left where they are - rewriting a dozen
   ///    green fixtures to share a helper is a change with no result to show for
   ///    it - and anything new uses this one.
   /// </summary>
   public static class RestRequest
   {
      public static (int status, string body) Send(string host, int port, string method, string path,
                                                   string authorization, string requestBody)
      {
         return Send(host, port, method, path, authorization, requestBody, null);
      }

      /// <summary>
      ///    The same request with one more header line, "Name: value" - the
      ///    one-time code in X-hMailServer-OTP once the administrator's second
      ///    factor is enrolled, which is the only header a fixture has needed
      ///    beside the credential.
      /// </summary>
      public static (int status, string body) Send(string host, int port, string method, string path,
                                                   string authorization, string requestBody, string extraHeader)
      {
         (int status, string headers, string body) answer = Exchange(host, port, method, path, authorization, requestBody, extraHeader);
         return (answer.status, answer.body);
      }

      /// <summary>
      ///    The same request, answered with the response's header block as well
      ///    as its status and body - for a Set-Cookie, such as the session cookie
      ///    POST /api/v1/accounts/{address}/support-session hands back. extraHeader
      ///    may hold more than one line, separated by CRLF.
      /// </summary>
      public static (int status, string headers, string body) Exchange(string host, int port, string method, string path,
                                                                       string authorization, string requestBody, string extraHeader)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect(host, port);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: " + host + "\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               if (!string.IsNullOrEmpty(extraHeader))
                  headers.Append(extraHeader + "\r\n");

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int statusCode = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";
               string head = separator >= 0 ? raw.Substring(0, separator) : raw;

               return (statusCode, head, body);
            }
         }
      }
   }
}
