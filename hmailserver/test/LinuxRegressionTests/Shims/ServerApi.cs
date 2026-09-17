// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using NUnit.Framework;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    One answer from the REST API: the status and the body, with the body's
   ///    JSON on demand. A body that is not JSON is kept as text and Json is null.
   /// </summary>
   public sealed class ApiAnswer
   {
      public int Status { get; }
      public string Body { get; }
      public JsonElement? Json { get; }

      public ApiAnswer(int status, string body)
      {
         Status = status;
         Body = body ?? string.Empty;

         try
         {
            using (var document = JsonDocument.Parse(Body))
               Json = document.RootElement.Clone();
         }
         catch (JsonException)
         {
            Json = null;
         }
      }

      public bool Ok => Status >= 200 && Status < 300;

      /// <summary>The "error" the API puts in a refusal, or the raw body when there is none.</summary>
      public string Error
      {
         get
         {
            JsonElement error;
            if (Json.HasValue && Json.Value.ValueKind == JsonValueKind.Object && Json.Value.TryGetProperty("error", out error))
               return error.GetString();
            return Body;
         }
      }

      public ApiAnswer Expect(int status, string doing)
      {
         if (Status != status)
            Assert.Fail("The REST API answered " + Status + " to " + doing + " (expected " + status + "): " + Body);
         return this;
      }
   }

   /// <summary>
   ///    The REST API of the server under test, as the fixture layer sees it. This is
   ///    what stands where the Windows suite has a COM Application object: every
   ///    domain, account and list the shims make, every queue drain and every log
   ///    read goes through here, with the administrator password, or with an
   ///    account's own credentials for the /api/v1/me routes.
   ///
   ///    Synchronous on purpose. The fixtures are synchronous, the API has a single
   ///    worker thread, and a test that wants two requests in flight is a test of the
   ///    HTTP server, which this project does not carry.
   /// </summary>
   public static class ServerApi
   {
      private static readonly HttpClient Client = new HttpClient
      {
         BaseAddress = new Uri(TestTarget.RestBaseUrl),
         Timeout = TimeSpan.FromSeconds(60)
      };

      private static readonly string AdminAuthorization = Basic("Administrator", TestTarget.AdminPassword);

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      // ---- The administrator's second factor, on this side of the wire ----
      //
      // Once a factor is enrolled on the administrator password, the password
      // alone authenticates no request, and every request this class sends
      // carries the password alone. On Windows a fixture that enrols goes on
      // working through the COM objects it authenticated before the enrolment:
      // Security/AdministratorTwoFactor disables the factor in its teardown on
      // the Settings object it already held. The equivalent of that still-open
      // session here is the secret the enrolment answered, kept from the shim's
      // EnrolAdministratorTOTP until its DisableAdministratorTOTP is answered and
      // turned into the current code in X-hMailServer-OTP on every administrator
      // request in between - so the fixture's own teardown, the base teardown's
      // queue drain and the restart detector's status read all authenticate while
      // the factor stands. The server reads the header only while a factor is
      // enrolled, so a code sent once the factor is gone costs nothing; and the
      // fixture's own raw requests (RestRequest, its sockets) carry what the
      // fixture gives them, which is how the password alone is refused where the
      // test expects it to be. Before this, the first test that enrolled left the
      // factor standing when its teardown's DELETE was refused, and every fixture
      // after it failed its one-time set-up on the password alone.
      private static string _administratorSecret;

      /// <summary>A factor was enrolled through this layer: its secret, from the otpauth URI.</summary>
      public static void AdministratorFactorEnrolled(string base32Secret)
      {
         _administratorSecret = string.IsNullOrEmpty(base32Secret) ? null : base32Secret;
      }

      /// <summary>The server has said the factor is gone.</summary>
      public static void AdministratorFactorDisabled()
      {
         _administratorSecret = null;
      }

      // RFC 6238 with the server's defaults - HMAC-SHA1, thirty-second steps, six
      // digits - the same arithmetic as the two fixtures that compute a code of
      // their own, and kept apart from them on purpose: they are the test.
      private static string OneTimeCode(string base32Secret)
      {
         byte[] key = Base32Decode(base32Secret);
         long counter = DateTimeOffset.UtcNow.ToUnixTimeSeconds() / 30;

         byte[] counterBytes = BitConverter.GetBytes(counter);
         if (BitConverter.IsLittleEndian)
            System.Array.Reverse(counterBytes);

         byte[] hash;
         using (var hmac = new HMACSHA1(key))
            hash = hmac.ComputeHash(counterBytes);

         int offset = hash[hash.Length - 1] & 0x0F;
         int binary =
            ((hash[offset] & 0x7F) << 24) |
            ((hash[offset + 1] & 0xFF) << 16) |
            ((hash[offset + 2] & 0xFF) << 8) |
            (hash[offset + 3] & 0xFF);

         return (binary % 1000000).ToString("000000");
      }

      private static byte[] Base32Decode(string input)
      {
         const string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
         var output = new List<byte>();
         int buffer = 0, bitsLeft = 0;

         foreach (char c in input)
         {
            int index = alphabet.IndexOf(char.ToUpperInvariant(c));
            if (index < 0)
               continue;

            buffer = (buffer << 5) | index;
            bitsLeft += 5;
            if (bitsLeft >= 8)
            {
               bitsLeft -= 8;
               output.Add((byte) ((buffer >> bitsLeft) & 0xFF));
            }
         }

         return output.ToArray();
      }

      public static ApiAnswer Get(string path)
      {
         return Send(HttpMethod.Get, path, null, AdminAuthorization);
      }

      public static ApiAnswer Post(string path, string json)
      {
         return Send(HttpMethod.Post, path, json, AdminAuthorization);
      }

      public static ApiAnswer Put(string path, string json)
      {
         return Send(HttpMethod.Put, path, json, AdminAuthorization);
      }

      public static ApiAnswer Delete(string path)
      {
         return Send(HttpMethod.Delete, path, null, AdminAuthorization);
      }

      /// <summary>
      ///    A GET that answers null instead of failing the test when the server
      ///    does not answer at all - for watching a listener go away and return.
      /// </summary>
      public static ApiAnswer TryGet(string path)
      {
         try
         {
            return Send(HttpMethod.Get, path, null, AdminAuthorization);
         }
         catch (InvalidOperationException)
         {
            return null;
         }
      }

      /// <summary>The /api/v1/me routes: an account's own credentials, never the administrator's.</summary>
      public static ApiAnswer AsAccount(string address, string password, HttpMethod method, string path, string json = null)
      {
         return Send(method, path, json, Basic(address, password));
      }

      // ---- The server's rate limit, kept on this side of the wire ----
      //
      // RestApiServer refuses a credential's 201st request in any ten-second window
      // with 429, and counts the refused ones too, so a caller that keeps pushing
      // stays refused until the window closes. A fixture layer that makes a dozen
      // calls per test and runs several tests a second reaches that on its own, and
      // the first run of this project did: every fixture's OneTimeSetUp failed on
      // "too many requests". So each credential's requests are counted here, in the
      // same window, and a call that would be the one over the line waits for the
      // window to move instead of being sent. The budget is below the server's so
      // that clock skew between the two counts cannot put a request over.
      private const int RequestsPerWindow = 180;
      private static readonly TimeSpan Window = TimeSpan.FromSeconds(10);
      private static readonly Dictionary<string, Queue<DateTime>> Recent = new Dictionary<string, Queue<DateTime>>();

      private static void WaitForBudget(string authorization)
      {
         while (true)
         {
            TimeSpan wait;

            lock (Recent)
            {
               Queue<DateTime> sent;
               if (!Recent.TryGetValue(authorization, out sent))
                  Recent[authorization] = sent = new Queue<DateTime>();

               var now = DateTime.UtcNow;
               while (sent.Count > 0 && now - sent.Peek() >= Window)
                  sent.Dequeue();

               if (sent.Count < RequestsPerWindow)
               {
                  sent.Enqueue(now);
                  return;
               }

               wait = sent.Peek() + Window - now + TimeSpan.FromMilliseconds(100);
            }

            System.Threading.Thread.Sleep(wait);
         }
      }

      private static ApiAnswer Send(HttpMethod method, string path, string json, string authorization)
      {
         for (var attempt = 1; ; attempt++)
         {
            WaitForBudget(authorization);

            using (var request = new HttpRequestMessage(method, path))
            {
               request.Headers.TryAddWithoutValidation("Authorization", authorization);

               // The administrator's one-time code, while a factor this layer
               // enrolled stands (AdministratorFactorEnrolled above).
               string secret = authorization == AdminAuthorization ? _administratorSecret : null;
               if (secret != null)
                  request.Headers.TryAddWithoutValidation("X-hMailServer-OTP", OneTimeCode(secret));

               if (json != null)
                  request.Content = new StringContent(json, Encoding.UTF8, "application/json");

               try
               {
                  using (var response = Client.Send(request, HttpCompletionOption.ResponseContentRead))
                  {
                     var body = response.Content == null ? string.Empty : response.Content.ReadAsStringAsync().GetAwaiter().GetResult();

                     // Refused by the server's count after all (another client on
                     // the same credential, or a window that started earlier than
                     // this side saw). Wait the whole window out, once; a second
                     // refusal is reported as what it is.
                     if ((int) response.StatusCode == 429 && attempt == 1)
                     {
                        var retryAfter = response.Headers.RetryAfter != null && response.Headers.RetryAfter.Delta.HasValue
                           ? response.Headers.RetryAfter.Delta.Value
                           : Window;

                        lock (Recent)
                           Recent.Remove(authorization);

                        System.Threading.Thread.Sleep(retryAfter + TimeSpan.FromMilliseconds(250));
                        continue;
                     }

                     return new ApiAnswer((int) response.StatusCode, body);
                  }
               }
               catch (HttpRequestException ex)
               {
                  throw new InvalidOperationException(
                     "The REST API at " + TestTarget.RestBaseUrl + " did not answer " + method + " " + path +
                     ". Is the server running, and is HMTEST_REST_PORT right? " + ex.Message, ex);
               }
            }
         }
      }

      /// <summary>A JSON string literal, escaped the way RFC 8259 asks.</summary>
      public static string Quote(string value)
      {
         return JsonSerializer.Serialize(value ?? string.Empty);
      }

      /// <summary>The elements of a JSON array answer, or of the named array property of an object answer.</summary>
      public static List<JsonElement> Array(ApiAnswer answer, string property = null)
      {
         var items = new List<JsonElement>();

         if (!answer.Json.HasValue)
            return items;

         var root = answer.Json.Value;

         if (property != null)
         {
            JsonElement inner;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty(property, out inner))
               return items;
            root = inner;
         }

         if (root.ValueKind != JsonValueKind.Array)
            return items;

         foreach (var element in root.EnumerateArray())
            items.Add(element);

         return items;
      }

      /// <summary>The elements of the named array property of an object, or nothing.</summary>
      public static List<JsonElement> Array(JsonElement element, string property)
      {
         var items = new List<JsonElement>();

         JsonElement inner;
         if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(property, out inner) ||
             inner.ValueKind != JsonValueKind.Array)
            return items;

         foreach (var item in inner.EnumerateArray())
            items.Add(item);

         return items;
      }

      public static string StringOf(JsonElement element, string property)
      {
         JsonElement value;
         if (element.ValueKind == JsonValueKind.Object && element.TryGetProperty(property, out value))
            return value.ValueKind == JsonValueKind.String ? value.GetString() : value.ToString();
         return null;
      }

      /// <summary>
      ///    A JSON true/false, or the fallback when the property is absent or is
      ///    not a boolean. There was no such helper: everything read until now was
      ///    a string, a number or an array.
      /// </summary>
      public static bool FlagOf(JsonElement element, string property, bool fallback = false)
      {
         JsonElement value;
         if (element.ValueKind == JsonValueKind.Object && element.TryGetProperty(property, out value))
         {
            if (value.ValueKind == JsonValueKind.True)
               return true;
            if (value.ValueKind == JsonValueKind.False)
               return false;
         }

         return fallback;
      }

      public static long LongOf(JsonElement element, string property, long fallback = 0)
      {
         JsonElement value;
         long parsed;
         if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(property, out value))
            return fallback;
         if (value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out parsed))
            return parsed;
         if (value.ValueKind == JsonValueKind.String && long.TryParse(value.GetString(), out parsed))
            return parsed;
         return fallback;
      }

      // ---- What this server's API can do, learned once from its own description ----

      private static readonly Lazy<string> OpenApi = new Lazy<string>(() =>
      {
         var answer = Get("/api/v1/openapi.json");
         return answer.Ok ? answer.Body : string.Empty;
      });

      // The "build" object of GET /api/v1/status, read once: what this build of
      // the server can do, settled when it was compiled.
      private static readonly Lazy<JsonElement?> Build = new Lazy<JsonElement?>(() =>
      {
         var answer = TryGet("/api/v1/status");
         JsonElement build;

         if (answer == null || answer.Status != 200 || !answer.Json.HasValue ||
             answer.Json.Value.ValueKind != JsonValueKind.Object ||
             !answer.Json.Value.TryGetProperty("build", out build))
            return null;

         return build;
      });

      /// <summary>
      ///    Whether this build of the server has the named capability - script_engine,
      ///    argon2id or post_quantum_key_exchange - as GET /api/v1/status reports under
      ///    "build". A server that reports no build object (one built before 6.3.3)
      ///    is taken to have none of them.
      /// </summary>
      public static bool Capability(string name)
      {
         var build = Build.Value;
         JsonElement value;
         return build.HasValue && build.Value.TryGetProperty(name, out value) && value.ValueKind == JsonValueKind.True;
      }

      /// <summary>
      ///    Whether POST /api/v1/domains and DELETE /api/v1/domains/{domain} exist on this
      ///    server. They were added after the account routes and a server built from an
      ///    older tree has only the listing; the fixture layer then works with a domain
      ///    the environment provisioned (docs/RegressionEnvironment.md says how) instead
      ///    of making and deleting its own.
      /// </summary>
      public static bool HasDomainWriteRoutes
      {
         get { return HasRoute("/api/v1/domains", "post"); }
      }

      // The write surface of wave 162, each probed the same way: the path's
      // entry in the OpenAPI document names the verb. A server without the
      // route is an older one, and the shim that needs it skips with the
      // reason that names it.
      public static bool HasSettingsWriteRoutes
      {
         get { return HasRoute("/api/v1/settings", "put"); }
      }

      public static bool HasAliasWriteRoutes
      {
         get { return HasRoute("/api/v1/domains/{domain}/aliases", "post"); }
      }

      public static bool HasRouteWriteRoutes
      {
         get { return HasRoute("/api/v1/routes", "post"); }
      }

      public static bool HasAccountUpdateRoute
      {
         get { return HasRoute("/api/v1/accounts/{address}", "put"); }
      }

      /// <summary>
      ///    Whether POST /api/v1/domains/{domain}/accounts stores first_name, last_name
      ///    and admin_level: its schema names admin_level from the change that made the
      ///    create read its body as a typed document. Before it, the create took the
      ///    names and dropped a level without a word, and refuses nothing it does not
      ///    know - so a shim that sent the level there would be believed and ignored.
      /// </summary>
      public static bool AccountCreateTakesNamesAndLevel
      {
         get
         {
            const string path = "/api/v1/domains/{domain}/accounts";
            var document = OpenApi.Value;
            var start = document.IndexOf("\"" + path + "\":{", StringComparison.Ordinal);
            if (start < 0)
               return false;
            var end = document.IndexOf("\"/api/v1/", start + path.Length + 4, StringComparison.Ordinal);
            var entry = end < 0 ? document.Substring(start) : document.Substring(start, end - start);
            return entry.Contains("\"admin_level\":{");
         }
      }

      public static bool HasRoute(string path, string verb)
      {
         var document = OpenApi.Value;
         var start = document.IndexOf("\"" + path + "\":{", StringComparison.Ordinal);
         if (start < 0)
            return false;
         var end = document.IndexOf("\"/api/v1/", start + path.Length + 4, StringComparison.Ordinal);
         var entry = end < 0 ? document.Substring(start) : document.Substring(start, end - start);
         return entry.Contains("\"" + verb + "\":");
      }
   }
}
