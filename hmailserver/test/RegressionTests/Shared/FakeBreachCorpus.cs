// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    A range endpoint of the breach corpus, on the loopback address, for the password
   ///    breach check's fixtures: GET /range/&lt;five hex characters&gt; answers every
   ///    suffix the test put in the corpus under that prefix, with its count, one per
   ///    line with CRLF as the public service writes them - and three padding entries
   ///    with a count of zero, as the service adds when asked for Add-Padding, so that a
   ///    check which matched a padding entry would be caught. A prefix nothing was put
   ///    under answers the padding alone.
   ///
   ///    On port 9551, which is this feature's alone in the suite, so the fixtures can
   ///    also name the address with NOTHING listening on it: dispose the corpus and the
   ///    same URL is an endpoint that cannot be reached.
   ///
   ///    Hashes are computed here, independently of the server: SHA-1 over the UTF-8
   ///    bytes, upper case. The fixtures pin that against published and hand-computed
   ///    vectors rather than trusting this helper to agree with the server by accident.
   /// </summary>
   public sealed class FakeBreachCorpus : IDisposable
   {
      public const int Port = 9551;

      private readonly FakeHttpEndpoint _endpoint;
      private readonly Dictionary<string, List<string>> _lines = new Dictionary<string, List<string>>(StringComparer.Ordinal);

      public FakeBreachCorpus() : this(IPAddress.Loopback)
      {
      }

      /// <summary>
      ///    The corpus on another loopback address - IPAddress.IPv6Loopback, for Ipv6Url -
      ///    on the same port. Throws SocketException where the machine cannot listen there.
      /// </summary>
      public FakeBreachCorpus(IPAddress address)
      {
         _endpoint = new FakeHttpEndpoint(address, Port, 200, PaddingOnly(), "text/plain");
      }

      /// <summary>The endpoint as BreachCheckUrl takes it: the prefix is appended.</summary>
      public static string Url
      {
         get { return "http://127.0.0.1:" + Port + "/range/"; }
      }

      /// <summary>The same endpoint on the IPv6 loopback address, bracketed as a URL writes it.</summary>
      public static string Ipv6Url
      {
         get { return "http://[::1]:" + Port + "/range/"; }
      }

      public static string Sha1Hex(byte[] bytes)
      {
         using (var sha1 = SHA1.Create())
            return string.Concat(sha1.ComputeHash(bytes).Select(b => b.ToString("X2")));
      }

      public static string Sha1HexUtf8(string password)
      {
         return Sha1Hex(Encoding.UTF8.GetBytes(password));
      }

      /// <summary>Puts a password's UTF-8 hash in the corpus, seen count times.</summary>
      public void Add(string password, long count)
      {
         AddHash(Sha1HexUtf8(password), count);
      }

      /// <summary>Puts a forty-character hash in the corpus, seen count times.</summary>
      public void AddHash(string hash40, long count)
      {
         string prefix = hash40.Substring(0, 5).ToUpperInvariant();
         string suffix = hash40.Substring(5).ToUpperInvariant();

         List<string> lines;
         if (!_lines.TryGetValue(prefix, out lines))
         {
            lines = new List<string>();
            _lines[prefix] = lines;
         }

         lines.RemoveAll(line => line.StartsWith(suffix + ":", StringComparison.Ordinal));
         lines.Add(suffix + ":" + count);

         _endpoint.SetResponse("/range/" + prefix, 200, Encoding.ASCII.GetBytes(Body(lines)), "text/plain");
      }

      /// <summary>What every request is answered with when no route matches.</summary>
      public void SetDefaultResponse(int status, string body, string contentType)
      {
         _endpoint.SetResponse(status, body, contentType);
      }

      /// <summary>Every range route answered with this instead, as an endpoint that has broken.</summary>
      public void BreakEveryRoute(int status, string body, string contentType)
      {
         _endpoint.ClearRoutes();
         _endpoint.SetResponse(status, body, contentType);
      }

      public List<string> Requests
      {
         get { return _endpoint.Requests; }
      }

      public void Dispose()
      {
         _endpoint.Dispose();
      }

      private static string PaddingOnly()
      {
         return Body(new List<string>());
      }

      private static string Body(List<string> lines)
      {
         // Padding that sorts among the real entries, as the service's does.
         var all = new List<string>(lines)
         {
            "0018A45C4D1DEF81644B54AB7F969B88D65:0",
            "7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF:0",
            "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF:0"
         };
         all.Sort(StringComparer.Ordinal);
         return string.Join("\r\n", all) + "\r\n";
      }
   }
}
