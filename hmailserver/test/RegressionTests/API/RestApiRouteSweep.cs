// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Every operation the server's own OpenAPI document describes, asked by the
   ///    two credentials that must not be able to change the server: an ordinary
   ///    account, and an account that holds the auditor role.
   ///
   ///    The REST API decides every request in one place (RestApiServer's
   ///    Authorize_, after ParseRoute_ has named the route), and the promise that
   ///    makes it one place is that no handler is reachable except through that
   ///    decision. The fixtures beside this one each prove it for the routes they
   ///    are about; nothing proved it for the routes nobody wrote a fixture for,
   ///    and the document grows with every feature. So the document itself is the
   ///    list here. What this catches, the day a route is documented: one an
   ///    account without a role is let through to - served before the credential
   ///    is read, classified as the account's own (IsSelfServiceRoute_) or the
   ///    credential's own (IsSessionRoute_), or documented and not routed at all,
   ///    which answers 404 - and a read that fails on the server's side. What it
   ///    does not catch is a route left out of PermissionFor_: Authorize_ refuses
   ///    an account without a role before it asks, and gives an unclassified route
   ///    the permission only the server administrator holds, so the auditor is
   ///    refused it too; build/check-role-choke-point.py is what fails on that.
   ///
   ///    <list type="bullet">
   ///      <item>An ordinary account - no role - is refused every administrative
   ///      operation, reads included, with 403 and before any handler runs: its
   ///      credentials reach its own mailbox under /api/v1/me and nothing else.</item>
   ///      <item>An auditor reads every administrative GET without the server
   ///      failing: no 5xx, no dropped connection. A read that throws on an
   ///      identifier that does not exist is a defect whoever asks, and the auditor
   ///      is the credential that asks for everything.</item>
   ///    </list>
   ///
   ///    Left out, each for its reason: the account's own routes (/api/v1/me and
   ///    what is under it - not /api/v1/metrics or /api/v1/message-trace, which
   ///    are the server's), which the account is meant to reach; the credential's
   ///    own routes (the session and the passkeys), which answer every account by
   ///    design (IsSessionRoute_); the routes answered before any credential is
   ///    read (the documentation, the OpenAPI document, the webmail's branding,
   ///    the password-protected message, the sign-in methods, the passkey
   ///    challenge and the password reset), which ProcessRequest_ serves to
   ///    anyone; and whatever the document describes outside /api/v1/ - the
   ///    pages, the links a message carries, and SCIM, which answers an API key
   ///    alone and refuses every other credential with 401.
   /// </summary>
   [TestFixture]
   public class RestApiRouteSweep : TestFixtureBase
   {
      // The REST listener on 10251 on the Windows bench - 10250 to 10259 are this
      // fixture's, and otherwise unused - or the suite's own where RestListener
      // finds one already on.
      private static int RestPort = 10251;
      private const string AdminPassword = "testar";
      private const string AccountPassword = "Sweep-secret-5813";
      private const string Plain = "sweep-plain@example.test";
      private const string Auditor = "sweep-auditor@example.test";
      private const string Target = "sweep-target@example.test";

      // Answered before Authenticate_ (ProcessRequest_), so no credential decides them.
      private static readonly string[] BeforeAnyCredential =
      {
         "/api/v1/help", "/api/v1/openapi.json", "/api/v1/portal/branding", "/api/v1/protected/",
         "/api/v1/signin-methods", "/api/v1/passkeys/challenge", "/api/v1/password-reset"
      };

      // The credential's own (IsSessionRoute_): allowed to every account.
      private static readonly string[] TheCredentialsOwn =
      {
         "/api/v1/session", "/api/v1/passkeys"
      };

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "The REST API did not answer /api/v1/status. Body: " + probe.body);

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Plain, AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Auditor, AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Target, AccountPassword);
      }

      [TearDown]
      public void StopRestApi()
      {
         Admin("DELETE", "/api/v1/role-assignments/" + Auditor);
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("An account without a role is refused every administrative operation the OpenAPI document describes, reads included, with 403")]
      public void AnOrdinaryAccountIsRefusedEveryAdministrativeOperation()
      {
         var tried = new List<string>();
         var notRefused = new List<string>();

         foreach ((string method, string path) in AdministrativeOperations(null))
         {
            (int status, string body) answer = WithinTheBudget(() => AsAccount(Plain, method, path, method == "GET" || method == "DELETE" ? null : "{}"));
            tried.Add(method + " " + path + " -> " + answer.status);

            if (answer.status != 403)
               notRefused.Add(method + " " + path + " -> " + answer.status + ": " + Shorten(answer.body));
         }

         Assert.IsEmpty(notRefused, "An account without a role was not refused these:\r\n" + string.Join("\r\n", notRefused));
         Assert.GreaterOrEqual(tried.Count, 300, "Too few operations were asked for the sweep to mean anything:\r\n" + string.Join("\r\n", tried));

         // Nothing changed: the one account the paths name is still there, with its password.
         (int status, string body) target = Admin("GET", "/api/v1/accounts/" + Target);
         Assert.AreEqual(200, target.status, target.body);
      }

      [Test]
      [Description("An auditor reads every administrative GET the OpenAPI document describes without a server error or a dropped connection")]
      public void AnAuditorReadsEveryAdministrativeGetWithoutAServerError()
      {
         (int status, string body) assigned = Admin("PUT", "/api/v1/role-assignments/" + Auditor, "{\"role\":\"auditor\",\"domains\":\"\"}");
         Assert.AreEqual(200, assigned.status, "The administrator could not make " + Auditor + " an auditor. Body: " + assigned.body);

         var tried = new List<string>();
         var failed = new List<string>();
         int answered = 0;

         foreach ((string method, string path) in AdministrativeOperations("get"))
         {
            (int status, string body) answer = WithinTheBudget(() => AsAccount(Auditor, method, path));
            tried.Add(method + " " + path + " -> " + answer.status);

            if (answer.status == 0 || answer.status >= 500)
               failed.Add(method + " " + path + " -> " + answer.status + ": " + Shorten(answer.body));
            else if (answer.status == 200)
               answered++;
         }

         Assert.IsEmpty(failed, "These reads failed on the server's side:\r\n" + string.Join("\r\n", failed));
         Assert.GreaterOrEqual(tried.Count, 100, "Too few reads were asked for the sweep to mean anything:\r\n" + string.Join("\r\n", tried));
         Assert.GreaterOrEqual(answered, 50, "Too few reads were answered for the sweep to mean anything - is the auditor refused them all?\r\n" + string.Join("\r\n", tried));
      }

      /// <summary>
      ///    Every administrative operation of the server's OpenAPI document, its path
      ///    made concrete: the test domain, the target account, and 1 for a number.
      ///    onlyMethod, when given, keeps that method alone.
      /// </summary>
      private static IEnumerable<(string method, string path)> AdministrativeOperations(string onlyMethod)
      {
         (int status, string body) document = Admin("GET", "/api/v1/openapi.json");
         Assert.AreEqual(200, document.status, document.body);

         var operations = new List<(string, string)>();
         using (JsonDocument parsed = JsonDocument.Parse(document.body))
         {
            foreach (JsonProperty path in parsed.RootElement.GetProperty("paths").EnumerateObject())
            {
               string name = path.Name;
               // "/api/v1/me" whole or as a directory: a prefix alone would take
               // /api/v1/metrics/history and the three /api/v1/message-trace routes
               // for the account's own and never ask them.
               if (!name.StartsWith("/api/v1/") || name == "/api/v1/me" || name.StartsWith("/api/v1/me/") ||
                   BeforeAnyCredential.Any(prefix => name == prefix || name.StartsWith(prefix + "/") || (prefix.EndsWith("/") && name.StartsWith(prefix))) ||
                   TheCredentialsOwn.Any(prefix => name == prefix || name.StartsWith(prefix + "/")))
                  continue;

               string concrete = Concrete(name);

               foreach (JsonProperty operation in path.Value.EnumerateObject())
               {
                  string method = operation.Name.ToLowerInvariant();
                  if (method != "get" && method != "post" && method != "put" && method != "delete" && method != "patch")
                     continue;
                  if (onlyMethod != null && method != onlyMethod)
                     continue;

                  operations.Add((method.ToUpperInvariant(), concrete));
               }
            }
         }

         return operations;
      }

      private static string Concrete(string template)
      {
         var values = new Dictionary<string, string>
         {
            { "address", Target }, { "target", Target }, { "member", Target }, { "destination", Target },
            { "domain", "example.test" }, { "name", "example.test" },
            { "section", "delivery" }, { "page", "RestApi" }, { "code", "en" }, { "fingerprint", "00" },
            { "token", "sweep" },
         };

         var builder = new StringBuilder();
         int at = 0;
         while (at < template.Length)
         {
            int open = template.IndexOf('{', at);
            if (open < 0)
            {
               builder.Append(template, at, template.Length - at);
               break;
            }

            int close = template.IndexOf('}', open);
            builder.Append(template, at, open - at);
            string parameter = template.Substring(open + 1, close - open - 1);
            builder.Append(values.TryGetValue(parameter, out string value) ? value : "1");
            at = close + 1;
         }

         return builder.ToString();
      }

      // 200 requests in ten seconds per credential (RestApiServer's budget): a
      // refusal for that reason is waited out and the request asked again.
      private static (int status, string body) WithinTheBudget(Func<(int status, string body)> request)
      {
         (int status, string body) answer = request();
         for (int attempt = 0; answer.status == 429 && attempt < 3; attempt++)
         {
            Thread.Sleep(11000);
            answer = request();
         }

         return answer;
      }

      private static string Shorten(string body)
      {
         return body == null ? "" : body.Length > 200 ? body.Substring(0, 200) + "..." : body;
      }

      private static (int status, string body) Admin(string method, string path, string requestBody = null)
      {
         return Http(method, path, "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)), requestBody);
      }

      private static (int status, string body) AsAccount(string address, string method, string path, string requestBody = null)
      {
         return Http(method, path, "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + AccountPassword)), requestBody);
      }

      // One HTTP/1.0 request against the REST listener: the status and the body,
      // with the connect retried briefly for the bind race after a reinitialise.
      // A connection the server closed without an answer is status 0.
      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
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

            client.ReceiveTimeout = 120000;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               headers.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               headers.Append("Connection: close\r\n\r\n");
               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());

               try
               {
                  stream.Write(headerBytes, 0, headerBytes.Length);
                  if (bodyBytes.Length > 0)
                     stream.Write(bodyBytes, 0, bodyBytes.Length);

                  byte[] buffer = new byte[8192];
                  int read;
                  while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                     memory.Write(buffer, 0, read);
               }
               catch (IOException)
               {
                  return (0, "");
               }

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int statusCode = 0;
               int lineEnd = raw.IndexOf("\r\n", StringComparison.Ordinal);
               string statusLine = lineEnd >= 0 ? raw.Substring(0, lineEnd) : raw;
               string[] parts = statusLine.Split(' ');
               if (parts.Length >= 2)
                  int.TryParse(parts[1], out statusCode);

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";
               return (statusCode, body);
            }
         }
      }
   }
}
