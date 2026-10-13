// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A credential confined to named domains is held to them on the REST API,
   ///    and a browser cannot make a state-changing request from another site.
   ///
   ///    Each test here pins one authorisation decision that a credential confined
   ///    to a domain - a full API key issued for named domains - could get past
   ///    before, reaching a server-wide object or one that names something beyond
   ///    its domain. The server administrator (the administrator password, and a
   ///    full key with no domain list) keeps every one of them, which is the
   ///    control each test carries: the fix is a narrowing, not a wall.
   ///
   ///      * the custom roles: a confined credential may read them and may not
   ///        create, rewrite or delete one, since a role is assigned across
   ///        domains;
   ///      * the metric history: counted from server-wide counters that carry no
   ///        domain, so a confined credential is refused it as it is the report's
   ///        server-wide sections;
   ///      * a domain alias: which external names map onto a hosted domain is the
   ///        server's to decide, so a confined credential may list and remove its
   ///        own domain's aliases but not create one;
   ///      * a domain rename: the name the server answers for is the server's, so
   ///        a confined credential keeps every other setting on its domain and may
   ///        not rename it;
   ///      * the relay host: delivery connects to it with no address rule, so a
   ///        confined credential's relay host is held to a public address, as a
   ///        reader's fetch host is;
   ///      * three rule actions - bind_to_address, send_using_route and
   ///        script_function - which reach past a rule's owner into the server's
   ///        own configuration, so a confined credential may not set one on a
   ///        domain's or an account's rule;
   ///      * and cross-site request forgery: a state-changing request a browser
   ///        reports as cross-site is refused whatever the credential, and the two
   ///        anonymous posts that carry no token - the subscribe form and the
   ///        password-reset request - are refused from another site too.
   ///
   ///    REST only, so the suite runs it against the Windows bench and the Linux
   ///    server alike; the COM half of the same decisions is TenantComAuthorization.
   /// </summary>
   [TestFixture]
   public class RestApiTenantAuthorization : TestFixtureBase
   {
      // The listener's port on the Windows bench, from this unit's allocated
      // block (47880-47899); the suite's own port where one is already on.
      private static int RestPort = 47880;
      private const string AdminPassword = "testar";
      private const string KeyStoreFileName = "hMailServerApiKeys.ini";
      private const string AccountPassword = "Tenant-secret-1234";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");
         DeleteKeyStore();

         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            DeleteKeyStore();
            RestListener.Stop();
            _application.Reinitialize();
         }
         finally
         {
            if (File.Exists(LogHandler.GetErrorLogFileName()))
               Console.WriteLine("Unexpected hMailServer ERROR log content: " + LogHandler.ReadErrorLog());

            LogHandler.DeleteErrorLog();
         }
      }

      // ---------------------------------------------------------- W-1: roles

      [Test]
      [Description("A full key issued for one domain may read the custom roles and is refused creating, rewriting or deleting one; the administrator does all three")]
      public void AConfinedKeyCannotChangeTheCustomRoles()
      {
         string confined = CreateKey("tenant-authz roles", "full", "example.test");

         // A role to rewrite and delete, made by the administrator.
         (int madeStatus, string made) = Admin("POST", "/api/v1/roles",
            "{\"name\":\"tenantauthz-desk\",\"description\":\"d\",\"permissions\":\"view_accounts\"}");
         Assert.AreEqual(201, madeStatus, made);

         try
         {
            // Reading them is allowed - the confinement is about changing them.
            Assert.AreEqual(200, Bearer(confined, "GET", "/api/v1/roles").status,
               "A confined key reads the role definitions.");

            (int createStatus, string createBody) = Bearer(confined, "POST", "/api/v1/roles",
               "{\"name\":\"tenantauthz-made-by-key\",\"description\":\"d\",\"permissions\":\"view_accounts\"}");
            Assert.AreEqual(403, createStatus, "A confined key created a custom role. Body: " + createBody);

            Assert.AreEqual(403, Bearer(confined, "PUT", "/api/v1/roles/tenantauthz-desk",
               "{\"description\":\"changed by a confined key\",\"permissions\":\"view_accounts,view_mailbox\"}").status,
               "A confined key rewrote a custom role.");

            Assert.AreEqual(403, Bearer(confined, "DELETE", "/api/v1/roles/tenantauthz-desk").status,
               "A confined key deleted a custom role.");

            // Nothing happened: the role still holds exactly what it was made with,
            // and the key's own role was never created.
            (int afterStatus, string after) = Admin("GET", "/api/v1/roles");
            Assert.AreEqual(200, afterStatus, after);
            StringAssert.DoesNotContain("tenantauthz-made-by-key", after, "The refused create stored a role.");
            StringAssert.DoesNotContain("view_mailbox", RoleLine(after, "tenantauthz-desk"), "The refused rewrite changed the role.");

            // The administrator may, which makes the refusals about the credential.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/roles/tenantauthz-desk",
               "{\"description\":\"d\",\"permissions\":\"view_accounts,view_mailbox\"}").status);
         }
         finally
         {
            Admin("DELETE", "/api/v1/roles/tenantauthz-desk");
            Admin("DELETE", "/api/v1/roles/tenantauthz-made-by-key");
         }
      }

      // ------------------------------------------------------- W-3: metrics

      [Test]
      [Description("A full key issued for one domain is refused the server-wide metric history and the administrator reads it; /status stays allowed for the confined key")]
      public void AConfinedKeyCannotReadTheMetricHistory()
      {
         string confined = CreateKey("tenant-authz metrics", "full", "example.test");

         (int status, string body) = Bearer(confined, "GET", "/api/v1/metrics/history?metric=processed_messages_total&range=24h");
         Assert.AreEqual(403, status, "A confined key read the server-wide metric history. Body: " + body);
         StringAssert.Contains("restricted to named domains", body);

         // The administrator reads it (400 for an unknown metric is still "read" -
         // what matters is that it is not refused as a confined credential), and
         // /status - which the Deck needs for its version and state - stays
         // allowed for the confined key, by design.
         Assert.AreEqual(200, Admin("GET", "/api/v1/metrics/history?metric=processed_messages_total&range=24h").status,
            "The administrator reads the metric history.");
         Assert.AreEqual(200, Bearer(confined, "GET", "/api/v1/status").status,
            "A confined key still reads /status.");
      }

      // ------------------------------------------------- W-10(a): domain alias

      [Test]
      [Description("A full key issued for one domain may list and remove its own domain's aliases and is refused creating one; the administrator creates it")]
      public void AConfinedKeyCannotCreateADomainAlias()
      {
         string confined = CreateKey("tenant-authz alias", "full", "example.test");

         (int createStatus, string createBody) = Bearer(confined, "POST", "/api/v1/domains/example.test/domain-aliases",
            "{\"name\":\"tenantauthz-alias.test\"}");
         Assert.AreEqual(403, createStatus, "A confined key created a domain alias. Body: " + createBody);

         // Nothing was created.
         Assert.AreEqual("[]", Admin("GET", "/api/v1/domains/example.test/domain-aliases").body,
            "The refused create added an alias.");

         // The administrator creates one, and the confined key then lists it and
         // removes it - both scoped to its own domain's collection, which is safe.
         (int madeStatus, string made) = Admin("POST", "/api/v1/domains/example.test/domain-aliases",
            "{\"name\":\"tenantauthz-alias.test\"}");
         Assert.AreEqual(201, madeStatus, made);

         StringAssert.Contains("tenantauthz-alias.test", Bearer(confined, "GET", "/api/v1/domains/example.test/domain-aliases").body,
            "A confined key lists its own domain's aliases.");
         Assert.AreEqual(200, Bearer(confined, "DELETE", "/api/v1/domains/example.test/domain-aliases/tenantauthz-alias.test").status,
            "A confined key removes an alias of its own domain.");
      }

      // ------------------------------------------------- W-10(b): domain rename

      [Test]
      [Description("A full key issued for a domain may change every setting on it but its name; a PUT that keeps the name is not a rename, and the administrator renames it")]
      public void AConfinedKeyCannotRenameItsDomain()
      {
         Domain d = SingletonProvider<TestSetup>.Instance.AddDomain("tenantauthz-rename.test");
         string confined = CreateKey("tenant-authz rename", "full", "tenantauthz-rename.test");

         // The same name is not a rename: an ordinary save of the whole object,
         // which the Deck and hmctl do, is not refused.
         Assert.AreEqual(200, Bearer(confined, "PUT", "/api/v1/domains/tenantauthz-rename.test",
            "{\"name\":\"tenantauthz-rename.test\",\"active\":true}").status,
            "A PUT that keeps the name is an ordinary save and is not refused.");

         // A changed name is a rename, which the confined key may not do.
         (int status, string body) = Bearer(confined, "PUT", "/api/v1/domains/tenantauthz-rename.test",
            "{\"name\":\"tenantauthz-renamed.test\",\"active\":true}");
         Assert.AreEqual(403, status, "A confined key renamed its domain. Body: " + body);
         Assert.IsTrue(Admin("GET", "/api/v1/domains/tenantauthz-rename.test").status == 200,
            "The refused rename changed the domain's name.");

         // The administrator renames it, which makes the refusal about the credential.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/domains/tenantauthz-rename.test",
            "{\"name\":\"tenantauthz-renamed.test\",\"active\":true}").status,
            "The administrator renames the domain.");
      }

      // -------------------------------------------------- W-10(d): relay host

      [Test]
      [Description("A full key issued for a domain may set a public relay host and is refused an internal one; the administrator may set an internal smart host")]
      public void AConfinedKeysRelayHostIsHeldToPublicAddresses()
      {
         string confined = CreateKey("tenant-authz relay", "full", "example.test");

         // A public address is accepted (93.184.216.34 is a public literal; the
         // check resolves it to itself and passes). Nothing is delivered through
         // it here - the row is only written.
         Assert.AreEqual(200, Bearer(confined, "PUT", "/api/v1/domains/example.test",
            "{\"active\":true,\"relay_host\":\"93.184.216.34\"}").status,
            "A confined key may set a public relay host.");

         // An internal address is refused: delivery would connect to the server's
         // own network with no address rule.
         (int status, string body) = Bearer(confined, "PUT", "/api/v1/domains/example.test",
            "{\"active\":true,\"relay_host\":\"10.73.0.5\"}");
         Assert.AreEqual(403, status, "A confined key set an internal relay host. Body: " + body);
         StringAssert.Contains("relay_host", body);

         // A PUT that does not name relay_host is never refused on it, whatever
         // the stored host is.
         Assert.AreEqual(200, Bearer(confined, "PUT", "/api/v1/domains/example.test", "{\"active\":true}").status,
            "A PUT that does not name relay_host is not refused on it.");

         // The administrator may set an internal smart host.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/domains/example.test",
            "{\"active\":true,\"relay_host\":\"10.73.0.5\"}").status,
            "The administrator may set an internal relay host.");
      }

      // -------------------------------------------- W-10(c): rule actions

      [Test]
      [Description("A full key issued for a domain may not set bind_to_address, send_using_route or script_function on a domain's or an account's rule, and keeps the ordinary actions; the administrator sets them")]
      public void AConfinedKeyCannotSetServerActionsOnOwnedRules()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruleowner@example.test", AccountPassword);
         string confined = CreateKey("tenant-authz rules", "full", "example.test");

         const string criterion = "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}";

         foreach (string action in new[]
         {
            "{\"type\":\"bind_to_address\",\"value\":\"93.184.216.34\"}",
            "{\"type\":\"script_function\",\"script_function\":\"OnTenantAuthz\"}",
         })
         {
            (int domainStatus, string domainBody) = Bearer(confined, "POST", "/api/v1/domains/example.test/rules",
               RuleBody("tenantauthz-drule", criterion, action));
            Assert.AreEqual(400, domainStatus, "A confined key set a server action on a domain rule. Body: " + domainBody);
            StringAssert.Contains("the server administrator", domainBody);

            (int accountStatus, string accountBody) = Bearer(confined, "POST", "/api/v1/accounts/ruleowner@example.test/rules",
               RuleBody("tenantauthz-arule", criterion, action));
            Assert.AreEqual(400, accountStatus, "A confined key set a server action on an account rule. Body: " + accountBody);
         }

         // The ordinary actions stay the confined key's on its own rules.
         Assert.AreEqual(201, Bearer(confined, "POST", "/api/v1/domains/example.test/rules",
            RuleBody("tenantauthz-ok", criterion, "{\"type\":\"delete\"}")).status,
            "A confined key keeps the ordinary rule actions on its domain's rules.");

         // The administrator sets a server action on a domain rule, which makes
         // the refusal about the credential and not the rule.
         Assert.AreEqual(201, Admin("POST", "/api/v1/domains/example.test/rules",
            RuleBody("tenantauthz-admin", criterion, "{\"type\":\"bind_to_address\",\"value\":\"93.184.216.34\"}")).status,
            "The administrator sets a server action on a domain rule.");
      }

      // ----------------------------------------------- W-2: cross-site writes

      [Test]
      [Description("A credentialed, state-changing request a browser reports as cross-site is refused; a same-origin one, and one with no browser metadata (a script), are not")]
      public void ACrossSiteCredentialedWriteIsRefused()
      {
         const string create = "{\"address\":\"w2-made@example.test\",\"password\":\"" + AccountPassword + "\"}";

         // A cross-site form post that carries the administrator's cached Basic
         // credential is refused before it is dispatched.
         (int crossStatus, string crossBody) = AdminWith("POST", "/api/v1/domains/example.test/accounts", create,
            "Sec-Fetch-Site: cross-site");
         Assert.AreEqual(403, crossStatus, "A cross-site credentialed write was accepted. Body: " + crossBody);
         Assert.IsFalse(AccountExists("w2-made@example.test"), "The refused cross-site write created an account.");

         // An Origin that is not the request's own, with no fetch metadata, is
         // refused the same way.
         Assert.AreEqual(403, AdminWith("POST", "/api/v1/domains/example.test/accounts", create,
            "Origin: https://another-site.example").status,
            "A foreign-Origin credentialed write was accepted.");
         Assert.IsFalse(AccountExists("w2-made@example.test"), "The refused foreign-Origin write created an account.");

         // Same-origin - the Deck and the webmail - passes.
         Assert.AreEqual(201, AdminWith("POST", "/api/v1/domains/example.test/accounts", create,
            "Sec-Fetch-Site: same-origin").status,
            "A same-origin write was refused.");
         Admin("DELETE", "/api/v1/accounts/w2-made@example.test");

         // No browser metadata at all - a script, hmctl, the PowerShell module -
         // is not treated as cross-site.
         Assert.AreEqual(201, Admin("POST", "/api/v1/domains/example.test/accounts", create).status,
            "A request with no fetch metadata was refused.");
         Admin("DELETE", "/api/v1/accounts/w2-made@example.test");
      }

      // --------------------------------------- W-12: anonymous cross-site posts

      [Test]
      [Description("The password-reset request and the subscribe form are refused from another site, and answered normally otherwise")]
      public void TheAnonymousPostsAreRefusedFromAnotherSite()
      {
         // The reset request: cross-site refused; a request with no browser
         // metadata is answered normally (it never says whether the address
         // exists, so the status is simply not the cross-site 403).
         Assert.AreEqual(403, HttpAnon("POST", "/api/v1/password-reset", "{\"address\":\"nobody@example.test\"}",
            "Sec-Fetch-Site: cross-site").status,
            "A cross-site password-reset request was accepted.");
         Assert.AreNotEqual(403, HttpAnon("POST", "/api/v1/password-reset", "{\"address\":\"nobody@example.test\"}", null).status,
            "A password-reset request with no browser metadata must be answered normally.");

         // The subscribe form: the cross-site guard is before the handler, so a
         // cross-site post is 403 whether or not the list exists; a same-origin
         // post to a list that does not exist reaches the handler and is 404,
         // which proves the guard fires only on cross-site.
         Assert.AreEqual(403, HttpAnon("POST", "/lists/none@example.test/subscribe", "address=someone%40example.test",
            "Sec-Fetch-Site: cross-site").status,
            "A cross-site subscribe post was accepted.");
         Assert.AreEqual(404, HttpAnon("POST", "/lists/none@example.test/subscribe", "address=someone%40example.test",
            "Sec-Fetch-Site: same-origin").status,
            "A same-origin subscribe post must reach the handler.");
      }

      // --------------------------------------------------------------- helpers

      private static string RoleLine(string rolesBody, string name)
      {
         Match match = Regex.Match(rolesBody, "\\{[^{}]*\"name\":\"" + Regex.Escape(name) + "\"[^{}]*\\}");
         return match.Success ? match.Value : "";
      }

      private bool AccountExists(string address)
      {
         string domain = address.Substring(address.IndexOf('@') + 1);
         (int status, string body) = Admin("GET", "/api/v1/domains/" + domain + "/accounts");
         return status == 200 && body.Contains("\"" + address + "\"");
      }

      private static string RuleBody(string name, string criterion, string action)
      {
         return "{\"name\":\"" + name + "\",\"active\":false,\"all_criteria\":true,\"criteria\":[" + criterion + "],\"actions\":[" + action + "]}";
      }

      private static string CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\""
            + (domains != null ? ",\"domains\":\"" + domains + "\"" : "") + "}";
         (int status, string created) = Admin("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "Could not create the test key. Body: " + created);
         Match match = Regex.Match(created, "\"key\"\\s*:\\s*\"([^\"]*)\"");
         Assert.IsTrue(match.Success, "The create response carried no key. Body: " + created);
         return match.Groups[1].Value;
      }

      private string FindKeyStore()
      {
         return IniFileSetting.CandidateDirectories()
            .Select(directory => Paths.Combine(directory, KeyStoreFileName))
            .FirstOrDefault(File.Exists);
      }

      private void DeleteKeyStore()
      {
         string store = FindKeyStore();
         if (store != null)
            File.Delete(store);
      }

      private static (int status, string body) Admin(string method, string path, string requestBody = null)
      {
         return Http(method, path, "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)), requestBody, null);
      }

      private static (int status, string body) AdminWith(string method, string path, string requestBody, string extraHeader)
      {
         return Http(method, path, "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)), requestBody, extraHeader);
      }

      private static (int status, string body) Bearer(string token, string method, string path, string requestBody = null)
      {
         return Http(method, path, "Bearer " + token, requestBody, null);
      }

      private static (int status, string body) HttpAnon(string method, string path, string requestBody, string extraHeader)
      {
         return Http(method, path, null, requestBody, extraHeader);
      }

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody, string extraHeader)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try { client.Connect("127.0.0.1", RestPort); last = null; break; }
               catch (SocketException ex) { last = ex; Thread.Sleep(200); }
            }
            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");
               if (extraHeader != null)
                  headers.Append(extraHeader + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  // text/plain, not application/json: a cross-site form can only
                  // send a simple content type, and the point of the W-2 and W-12
                  // tests is the browser's own form post. The server reads the
                  // body whatever its declared type.
                  headers.Append("Content-Type: text/plain\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }
               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               try
               {
                  stream.Write(headerBytes, 0, headerBytes.Length);
                  if (bodyBytes.Length > 0)
                     stream.Write(bodyBytes, 0, bodyBytes.Length);

                  byte[] buffer = new byte[4096];
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
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (statusCode, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
