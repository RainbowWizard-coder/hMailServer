// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
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
      // UseCookies off: the one cookie this layer presents is the administrator's
      // session while a second factor stands (below), set by hand on the requests
      // that are meant to carry it - a cookie jar would put it on every request,
      // the ones made with an account's own credentials included.
      private static readonly HttpClient Client = new HttpClient(new HttpClientHandler { UseCookies = false })
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
      // session here is a browser session: from the shim's
      // EnrolAdministratorTOTP until its DisableAdministratorTOTP is answered, the
      // administrator's requests go with the hmailsession cookie of a session this
      // layer started with the password and one code (POST /api/v1/session), so
      // the fixture's own teardown, the base teardown's queue drain and the
      // restart detector's status read all authenticate while the factor stands.
      //
      // A session, and not the password with a code on every request, because the
      // server takes a code once (SecondFactorGuard): the same code on a second
      // request is refused, so a code a request cannot share with the next. The
      // codes come from OneTimeCodes.Fresh, which the fixtures' own requests use
      // too, so the one this layer takes is never one a fixture then offers. A
      // session the server has dropped (a restart ends them all) is started
      // again with the next code. The fixture's own raw requests (RestRequest,
      // its sockets) carry what the fixture gives them, which is how the password
      // alone is refused where the test expects it to be. Before any of this, the
      // first test that enrolled left the factor standing when its teardown's
      // DELETE was refused, and every fixture after it failed its one-time set-up
      // on the password alone.
      private static string _administratorSecret;
      private static string _administratorSession;

      /// <summary>A factor was enrolled through this layer: its secret, from the otpauth URI.</summary>
      public static void AdministratorFactorEnrolled(string base32Secret)
      {
         _administratorSecret = string.IsNullOrEmpty(base32Secret) ? null : base32Secret;
         _administratorSession = null;
      }

      /// <summary>The server has said the factor is gone.</summary>
      public static void AdministratorFactorDisabled()
      {
         _administratorSecret = null;
         _administratorSession = null;
      }

      // The administrator's session while a factor stands: POST /api/v1/session
      // with the password and the next code, and the cookie out of its answer.
      private static string AdministratorSession(string secret)
      {
         if (_administratorSession != null)
            return _administratorSession;

         string refusal;
         _administratorSession = PostSession(OneTimeCodes.Fresh(secret), out refusal);
         if (_administratorSession == null)
            throw new InvalidOperationException("The administrator's session could not be started with a one-time code: " + refusal);

         return _administratorSession;
      }

      // POST /api/v1/session with the administrator password, as the Control
      // Deck's sign-in sends it - the password once, with X-Requested-With - and
      // with the one-time code when one is given. The hmailsession cookie out of
      // a 201, or null and what the server answered instead; a server that does
      // not answer at all throws, as Client.Send does.
      private static string PostSession(string code, out string refusal)
      {
         using (var request = new HttpRequestMessage(HttpMethod.Post, "/api/v1/session"))
         {
            request.Headers.TryAddWithoutValidation("Authorization", AdminAuthorization);
            request.Headers.TryAddWithoutValidation("X-Requested-With", "hMailServer");
            if (code != null)
               request.Headers.TryAddWithoutValidation("X-hMailServer-OTP", code);

            using (var response = Client.Send(request, HttpCompletionOption.ResponseContentRead))
            {
               var body = response.Content == null ? string.Empty : response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
               if ((int) response.StatusCode != 201)
               {
                  refusal = (int) response.StatusCode + " " + body;
                  return null;
               }

               IEnumerable<string> cookies;
               if (response.Headers.TryGetValues("Set-Cookie", out cookies))
               {
                  foreach (string cookie in cookies)
                  {
                     const string name = "hmailsession=";
                     if (!cookie.StartsWith(name, StringComparison.Ordinal))
                        continue;

                     int end = cookie.IndexOf(';');
                     refusal = null;
                     return end < 0 ? cookie : cookie.Substring(0, end);
                  }
               }

               refusal = "POST /api/v1/session answered 201 without an hmailsession cookie: " + body;
               return null;
            }
         }
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

      // ---- A witness to a restart ----
      //
      // RestApiServer::Stop ends every browser session (ClearBrowserSessions_),
      // so a session started before a restart is answered 401 once the restart
      // has happened, and goes on being answered 401 however late it is next
      // presented. A look at the server's state has no such memory: the server
      // is down for a fraction of a second, and a request the budget below
      // holds back for longer than that sees it running on both sides.
      // SslSetup.Reinitialize watches a restart through these.

      /// <summary>
      ///    An administrator session of the caller's own, started with the
      ///    password: the hmailsession cookie, or null when the server would not
      ///    start one (a server older than the route) or did not answer. Null
      ///    without asking while a second factor stands: the password alone is
      ///    refused then, and the session this layer already signs in with,
      ///    AdministratorSessionInUse, serves instead.
      /// </summary>
      public static string StartAdministratorSession()
      {
         if (_administratorSecret != null)
            return null;

         WaitForBudget(AdminAuthorization);

         try
         {
            string refusal;
            return PostSession(null, out refusal);
         }
         catch (HttpRequestException)
         {
            return null;
         }
      }

      /// <summary>
      ///    The session this layer's own administrator requests go on while a
      ///    second factor stands, as the last of them left it; null while they go
      ///    on the password.
      /// </summary>
      public static string AdministratorSessionInUse
      {
         get { return _administratorSecret != null ? _administratorSession : null; }
      }

      /// <summary>
      ///    A GET with the session's cookie and no password, answered as the server
      ///    answered it. A 401 is returned rather than followed by a new session,
      ///    as this layer's own requests are, because the end of the session is
      ///    what the caller is asking about. Null when the server does not answer
      ///    at all. Counted in the administrator's budget: the server counts an
      ///    administrator session's requests with the password's, under the one
      ///    name.
      /// </summary>
      public static ApiAnswer TryGetWithSession(string session, string path)
      {
         try
         {
            return Send(HttpMethod.Get, path, null, AdminAuthorization, session);
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

      private static ApiAnswer Send(HttpMethod method, string path, string json, string authorization, string givenSession = null)
      {
         for (var attempt = 1; ; attempt++)
         {
            WaitForBudget(authorization);

            // The administrator's session, while a factor this layer enrolled
            // stands (AdministratorFactorEnrolled above), in place of the password;
            // or the session the caller gave (TryGetWithSession), as it is.
            string secret = givenSession == null && authorization == AdminAuthorization ? _administratorSecret : null;
            string session = givenSession ?? (secret != null ? AdministratorSession(secret) : null);

            using (var request = new HttpRequestMessage(method, path))
            {
               if (session != null)
               {
                  request.Headers.TryAddWithoutValidation("Cookie", session);
                  request.Headers.TryAddWithoutValidation("X-Requested-With", "hMailServer");
               }
               else
               {
                  request.Headers.TryAddWithoutValidation("Authorization", authorization);
               }

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

                     // The session is gone - the server restarted, or it idled
                     // out - and the factor still stands: start another with the
                     // next code and send the request again, once. Not when the
                     // caller gave the session: its end is the answer it wants.
                     if ((int) response.StatusCode == 401 && secret != null && attempt == 1)
                     {
                        _administratorSession = null;
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
      ///    Whether this build of the server has the named capability - script_engine
      ///    (see ScriptLanguages for which), argon2id or post_quantum_key_exchange - as
      ///    GET /api/v1/status reports under
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
      ///    The event-script languages this build runs, as GET /api/v1/status reports
      ///    them under build.script_languages: JavaScript on a Linux build, VBScript and
      ///    JScript on a Windows one. The setting accepts all three everywhere; a
      ///    language a build does not run is refused when the script is loaded. A
      ///    server that reports no list is one from before the list: its script_engine
      ///    was true only on Windows, where the two Active Scripting languages ran.
      /// </summary>
      public static IReadOnlyList<string> ScriptLanguages
      {
         get
         {
            var build = Build.Value;
            JsonElement value;
            var languages = new List<string>();

            if (build.HasValue && build.Value.TryGetProperty("script_languages", out value) && value.ValueKind == JsonValueKind.Array)
            {
               foreach (var language in value.EnumerateArray())
               {
                  if (language.ValueKind == JsonValueKind.String)
                     languages.Add(language.GetString());
               }
            }
            else if (Capability("script_engine"))
            {
               languages.Add("VBScript");
               languages.Add("JScript");
            }

            return languages;
         }
      }

      /// <summary>Whether GET /api/v1/status names the languages itself, under build.script_languages.</summary>
      public static bool ReportsScriptLanguages
      {
         get
         {
            var build = Build.Value;
            JsonElement value;
            return build.HasValue && build.Value.TryGetProperty("script_languages", out value) && value.ValueKind == JsonValueKind.Array;
         }
      }

      /// <summary>Whether this build runs event scripts in the language, as the setting spells it.</summary>
      public static bool RunsScriptLanguage(string language)
      {
         foreach (var runs in ScriptLanguages)
         {
            if (string.Equals(runs, language, StringComparison.OrdinalIgnoreCase))
               return true;
         }

         return false;
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

      // Every path's PUT request schema, read once: the properties it lists that
      // are not marked readOnly. A path the document names twice has the keys of
      // both, so the order of its entries decides nothing. Problem is set when the
      // document could not be read at all, so that a caller's failure says that
      // rather than that one path has no schema.
      private sealed class PutSchemas
      {
         public readonly Dictionary<string, HashSet<string>> KeysByPath = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
         public string Problem;
      }

      private static readonly Lazy<PutSchemas> PutKeys = new Lazy<PutSchemas>(() =>
      {
         var schemas = new PutSchemas();
         var keysByPath = schemas.KeysByPath;
         if (OpenApi.Value.Length == 0)
         {
            schemas.Problem = "GET /api/v1/openapi.json did not answer 200.";
            return schemas;
         }

         JsonDocument document;
         try
         {
            document = JsonDocument.Parse(OpenApi.Value, new JsonDocumentOptions { MaxDepth = 256 });
         }
         catch (JsonException ex)
         {
            schemas.Problem = "The server's OpenAPI document (GET /api/v1/openapi.json) is not JSON: " + ex.Message;
            return schemas;
         }

         using (document)
         {
            JsonElement paths;
            if (document.RootElement.ValueKind != JsonValueKind.Object ||
                !document.RootElement.TryGetProperty("paths", out paths) || paths.ValueKind != JsonValueKind.Object)
            {
               schemas.Problem = "The server's OpenAPI document (GET /api/v1/openapi.json) has no paths object.";
               return schemas;
            }

            foreach (var path in paths.EnumerateObject())
            {
               var properties = path.Value;
               foreach (var step in new[] { "put", "requestBody", "content", "application/json", "schema", "properties" })
               {
                  JsonElement next;
                  if (properties.ValueKind != JsonValueKind.Object || !properties.TryGetProperty(step, out next))
                  {
                     properties = default(JsonElement);
                     break;
                  }
                  properties = next;
               }

               if (properties.ValueKind != JsonValueKind.Object)
                  continue;

               HashSet<string> keys;
               if (!keysByPath.TryGetValue(path.Name, out keys))
                  keysByPath[path.Name] = keys = new HashSet<string>(StringComparer.Ordinal);

               foreach (var property in properties.EnumerateObject())
               {
                  if (!FlagOf(property.Value, "readOnly"))
                     keys.Add(property.Name);
               }
            }
         }

         return schemas;
      });

      /// <summary>
      ///    The keys PUT on the path takes, as this server's OpenAPI document lists
      ///    them: every property of the PUT's request schema that is not marked
      ///    readOnly. For a settings group that is exactly what a GET of the group
      ///    answers less what the PUT refuses as read-only - the read-only rows
      ///    (geoip_database_status, the log file names, the cache counters) and the
      ///    &lt;key&gt;_levels and &lt;key&gt;_environment marks, which the schema does
      ///    not list at all. Null when the document gives the path no PUT schema,
      ///    or could not be read - WritableKeysProblem then says which.
      /// </summary>
      public static IReadOnlySet<string> WritableKeys(string path)
      {
         HashSet<string> keys;
         return PutKeys.Value.KeysByPath.TryGetValue(path, out keys) ? keys : null;
      }

      /// <summary>
      ///    Why WritableKeys knows no path at all - the document did not answer, is
      ///    not JSON or has no paths - or null when it was read.
      /// </summary>
      public static string WritableKeysProblem
      {
         get { return PutKeys.Value.Problem; }
      }
   }
}
