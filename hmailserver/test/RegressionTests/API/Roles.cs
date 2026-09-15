// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
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
   ///    Who may administer what: the named roles, over the REST API and over COM.
   ///
   ///    Until September 2026 there were two answers - the administrator password
   ///    and an account with an admin level - and a read-only or domain-restricted
   ///    API key. The roles are the named version of the same idea, decided in one
   ///    place (RoleManager::Allows) and proved by build/check-role-choke-point.py.
   ///    What that script cannot prove is that the server behaves as the code
   ///    says, which is what these tests are for:
   ///
   ///      * each built-in role against a representative route and COM call,
   ///        allowed and refused;
   ///      * a domain-confined role refused another domain's account by name, and
   ///        told nothing about whether it exists;
   ///      * an auditor refused every write the API documents, and reading what
   ///        an auditor exists to read;
   ///      * a help desk allowed a password reset and refused a settings change;
   ///      * the two historical levels exactly as they were, with no row written;
   ///      * and the COM path that let any account reach another account's full
   ///        object through a permission on a folder of its own, closed.
   /// </summary>
   [TestFixture]
   public class Roles : TestFixtureBase
   {
      private static int RestPort = 9548;
      private const string AdminPassword = "testar";
      private const string KeyStoreFileName = "hMailServerApiKeys.ini";
      private const string OtherDomain = "other.example.test";
      private const string AccountPassword = "Roles-secret-1234";
      private const string AccessDenied = "You do not have access to this property / method.";

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

         DeleteTestRoles();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            DeleteTestRoles();
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

      // ------------------------------------------------------------------ REST

      [Test]
      [Description("The four built-in roles are listed with exactly the permissions the code gives them, and the vocabulary comes with them")]
      public void BuiltInRolesAreListedWithThePermissionsTheCodeGivesThem()
      {
         (int status, string body) roles = Admin("GET", "/api/v1/roles");
         Assert.AreEqual(200, roles.status, roles.body);

         using (JsonDocument document = JsonDocument.Parse(roles.body))
         {
            var byName = document.RootElement.GetProperty("roles").EnumerateArray()
               .ToDictionary(r => r.GetProperty("name").GetString(), r => r);

            foreach (string name in new[] { "serveradmin", "domainadmin", "helpdesk", "auditor" })
            {
               Assert.IsTrue(byName.ContainsKey(name), "The role " + name + " is missing. Body: " + roles.body);
               Assert.IsTrue(byName[name].GetProperty("builtin").GetBoolean(), name + " is not marked built in.");
            }

            CollectionAssert.AreEquivalent(
               new[] { "view_accounts", "view_message_trace", "reset_password", "unlock_account" },
               Permissions(byName["helpdesk"]),
               "A help desk resets a password, unlocks an account, reads a message trace and finds the account - and nothing else.");

            Assert.IsFalse(Permissions(byName["auditor"]).Any(p => !p.StartsWith("view_")),
               "An auditor holds a permission that is not a read: " + byName["auditor"].GetProperty("permissions").GetString());

            Assert.AreEqual(22, document.RootElement.GetProperty("permissions").GetArrayLength());
            Assert.AreEqual(22, Permissions(byName["serveradmin"]).Length);
         }
      }

      [Test]
      [Description("GET /api/v1/session says the administrator password is the server administrator, with every permission and every domain")]
      public void TheAdministratorPasswordIsTheServerAdministrator()
      {
         (int status, string body) who = Admin("GET", "/api/v1/session");
         Assert.AreEqual(200, who.status, who.body);
         StringAssert.Contains("\"credential\":\"administrator\"", who.body);
         StringAssert.Contains("\"role\":\"serveradmin\"", who.body);
         StringAssert.Contains("\"administrative\":true", who.body);
         StringAssert.Contains("\"domains\":[]", who.body);
      }

      [Test]
      [Description("An account with an admin level holds the role that level has always given it, with no assignment row written; an ordinary account still reaches no administration route and is told exactly what it was told before")]
      public void ExistingAdministratorsKeepTheirAuthorityWithNoRowWritten()
      {
         Account domainAdmin = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "domainadmin@example.test", AccountPassword);
         domainAdmin.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         domainAdmin.Save();

         Account serverAdmin = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "serveradmin@example.test", AccountPassword);
         serverAdmin.AdminLevel = eAdminLevel.hAdminLevelServerAdmin;
         serverAdmin.Save();

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ordinary@example.test", AccountPassword);

         (int status, string body) asDomainAdmin = AsAccount("domainadmin@example.test", "GET", "/api/v1/session");
         Assert.AreEqual(200, asDomainAdmin.status, asDomainAdmin.body);
         StringAssert.Contains("\"role\":\"domainadmin\"", asDomainAdmin.body);
         StringAssert.Contains("\"domains\":[\"example.test\"]", asDomainAdmin.body, "A domain administrator is confined to its own domain, as it always was.");

         (int status, string body) asServerAdmin = AsAccount("serveradmin@example.test", "GET", "/api/v1/session");
         Assert.AreEqual(200, asServerAdmin.status, asServerAdmin.body);
         StringAssert.Contains("\"role\":\"serveradmin\"", asServerAdmin.body);

         (int status, string body) assignments = Admin("GET", "/api/v1/role-assignments");
         Assert.AreEqual(200, assignments.status, assignments.body);
         StringAssert.Contains("\"count\":0", assignments.body, "No upgrade and no admin level writes an assignment row.");

         (int status, string body) ordinary = AsAccount("ordinary@example.test", "GET", "/api/v1/status");
         Assert.AreEqual(403, ordinary.status, ordinary.body);
         StringAssert.Contains("an account's credentials reach only the account's own endpoints", ordinary.body);

         // Over COM, the domain administrator creates an account in its domain and
         // may not reach the settings, exactly as Permissions.cs has always held.
         var asDomainAdminOverCom = new Application();
         Assert.IsNotNull(asDomainAdminOverCom.Authenticate("domainadmin@example.test", AccountPassword));
         Assert.AreEqual("domainadmin", asDomainAdminOverCom.CurrentRole);
         var ex = Assert.Throws<COMException>(() => { var settings = asDomainAdminOverCom.Settings; });
         StringAssert.Contains(AccessDenied, ex.Message);

         var asServerAdminOverCom = new Application();
         Assert.IsNotNull(asServerAdminOverCom.Authenticate("serveradmin@example.test", AccountPassword));
         Assert.AreEqual("serveradmin", asServerAdminOverCom.CurrentRole);
         Assert.IsNotNull(asServerAdminOverCom.Settings);

         var asOrdinaryOverCom = new Application();
         Assert.IsNotNull(asOrdinaryOverCom.Authenticate("ordinary@example.test", AccountPassword));
         Assert.AreEqual("", asOrdinaryOverCom.CurrentRole);
      }

      [Test]
      [Description("A help desk resets a password and unlocks an account over REST, the new password works, and a settings change is refused with the role and the permission named")]
      public void HelpdeskResetsAPasswordAndIsRefusedASettingsChange()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         Account user = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);
         Assign("helen@example.test", "helpdesk", "");

         (int status, string body) reset = AsAccount("helen@example.test", "POST", "/api/v1/accounts/user@example.test/password",
            "{\"password\":\"Reset-by-the-desk-99\"}");
         Assert.AreEqual(200, reset.status, reset.body);
         Assert.IsTrue(AccountOverCom("user@example.test").ValidatePassword("Reset-by-the-desk-99"), "The reset password is the one in force.");

         (int status, string body) unlock = AsAccount("helen@example.test", "POST", "/api/v1/accounts/user@example.test/unlock");
         Assert.AreEqual(200, unlock.status, unlock.body);

         (int status, string body) settings = AsAccount("helen@example.test", "PUT", "/api/v1/settings", "{\"welcome_smtp\":\"changed by the desk\"}");
         Assert.AreEqual(403, settings.status, settings.body);
         StringAssert.Contains("the role 'helpdesk' does not hold the permission 'manage_settings'", settings.body);

         (int status, string body) configuration = AsAccount("helen@example.test", "GET", "/api/v1/status");
         Assert.AreEqual(403, configuration.status, "A help desk does not read the server's configuration. Body: " + configuration.body);

         (int status, string body) assign = AsAccount("helen@example.test", "PUT", "/api/v1/role-assignments/user@example.test", "{\"role\":\"serveradmin\"}");
         Assert.AreEqual(403, assign.status, "A help desk does not hand out roles. Body: " + assign.body);
         StringAssert.Contains("manage_roles", assign.body);

         (int status, string body) update = AsAccount("helen@example.test", "PUT", "/api/v1/accounts/user@example.test", "{\"max_size_mb\":1}");
         Assert.AreEqual(403, update.status, "The account update is every other field of the account; the reset is its own route. Body: " + update.body);
      }

      [Test]
      [Description("A role confined to one domain is refused an account in another by name, and the answer is the same whether that account exists or not")]
      public void ADomainConfinedRoleIsRefusedAnotherDomainsAccountAndToldNothing()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);
         SingletonProvider<TestSetup>.Instance.AddAccount(other, "exists@" + OtherDomain, AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         Assign("helen@example.test", "helpdesk", "");

         const string resetBody = "{\"password\":\"Nobody-should-set-this-1\"}";

         (int status, string body) exists = AsAccount("helen@example.test", "POST", "/api/v1/accounts/exists@" + OtherDomain + "/password", resetBody);
         (int status, string body) missing = AsAccount("helen@example.test", "POST", "/api/v1/accounts/missing@" + OtherDomain + "/password", resetBody);

         Assert.AreEqual(403, exists.status, exists.body);
         Assert.AreEqual(403, missing.status, missing.body);
         Assert.AreEqual(exists.body, missing.body, "\"Not yours\" and \"no such account\" must read the same, or the refusal enumerates mailboxes.");
         StringAssert.DoesNotContain("exists@", exists.body);

         Assert.IsTrue(AccountOverCom("exists@" + OtherDomain).ValidatePassword(AccountPassword), "The refused reset changed nothing.");

         (int status, string body) ownMissing = AsAccount("helen@example.test", "POST", "/api/v1/accounts/missing@example.test/password", body);
         Assert.AreEqual(404, ownMissing.status, "Inside its own domain, a missing account is simply missing. Body: " + ownMissing.body);

         // Widening the assignment by naming the other domain is how an
         // administrator lets the role reach it.
         Assign("helen@example.test", "helpdesk", "example.test," + OtherDomain);
         (int status, string body) widened = AsAccount("helen@example.test", "POST", "/api/v1/accounts/exists@" + OtherDomain + "/password", resetBody);
         Assert.AreEqual(200, widened.status, widened.body);
      }

      [Test]
      [Description("An auditor reads the configuration, the accounts, the logs, the queue and who holds which role")]
      public void AnAuditorReadsWhatAnAuditorIsFor()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@example.test", AccountPassword);
         Assign("audrey@example.test", "auditor", "");

         (int status, string body) who = AsAccount("audrey@example.test", "GET", "/api/v1/session");
         Assert.AreEqual(200, who.status, who.body);
         StringAssert.Contains("\"role\":\"auditor\"", who.body);
         StringAssert.Contains("\"domains\":[]", who.body, "An auditor with no list reaches every domain; confined to its own it could not read what is server-wide.");

         foreach (string path in new[]
         {
            "/api/v1/status", "/api/v1/settings", "/api/v1/domains", "/api/v1/domains/example.test/accounts",
            "/api/v1/queue", "/api/v1/logs", "/api/v1/roles", "/api/v1/role-assignments", "/api/v1/openapi.json"
         })
         {
            (int status, string body) read = AsAccount("audrey@example.test", "GET", path);
            Assert.AreEqual(200, read.status, "An auditor must be able to read " + path + ". Body: " + read.body);
         }

         (int status, string body) keys = AsAccount("audrey@example.test", "GET", "/api/v1/apikeys");
         Assert.AreEqual(403, keys.status, "The key store is the administrator password's, and a 403 rather than a 401 does not sign a Deck session out. Body: " + keys.body);
      }

      [Test]
      [Description("An auditor is refused every write the OpenAPI document describes, and nothing on the server changes")]
      public void AnAuditorIsRefusedEveryWriteTheApiDocuments()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "target@example.test", AccountPassword);
         Assign("audrey@example.test", "auditor", "");

         (int status, string body) doc = Admin("GET", "/api/v1/openapi.json");
         Assert.AreEqual(200, doc.status, doc.body);

         // Four POSTs compute an answer and change nothing - the server classifies
         // them as reads, and an auditor is allowed them. The account's own
         // endpoints and the session are the auditor's own mailbox and its own
         // credential, not the server's administration.
         var computesOnly = new HashSet<string>
         {
            "/api/v1/sieve/evaluate", "/api/v1/rules/match", "/api/v1/calendar/expand", "/api/v1/settings/scripting/check"
         };

         var tried = new List<string>();
         var answeredWithSuccess = new List<string>();
         int refused = 0;

         using (JsonDocument document = JsonDocument.Parse(doc.body))
         {
            foreach (JsonProperty path in document.RootElement.GetProperty("paths").EnumerateObject())
            {
               if (!path.Name.StartsWith("/api/v1/") || path.Name.StartsWith("/api/v1/me") ||
                   path.Name.StartsWith("/api/v1/session") || computesOnly.Contains(path.Name))
                  continue;

               string concrete = path.Name
                  .Replace("{address}", "target@example.test").Replace("{domain}", "example.test")
                  .Replace("{account_id}", "1").Replace("{pid}", "1").Replace("{id}", "1").Replace("{name}", "rolestest-probe");

               foreach (JsonProperty operation in path.Value.EnumerateObject())
               {
                  string method = operation.Name.ToUpperInvariant();
                  if (method != "POST" && method != "PUT" && method != "DELETE" && method != "PATCH")
                     continue;

                  (int status, string body) answer = AsAccount("audrey@example.test", method, concrete, "{}");

                  if (answer.status == 429)
                  {
                     Thread.Sleep(11000);
                     answer = AsAccount("audrey@example.test", method, concrete, "{}");
                  }

                  tried.Add(method + " " + concrete + " -> " + answer.status);

                  if (answer.status >= 200 && answer.status < 300)
                     answeredWithSuccess.Add(method + " " + concrete + ": " + answer.body);
                  if (answer.status == 403)
                     refused++;
               }
            }
         }

         Assert.IsEmpty(answeredWithSuccess, "An auditor changed something:\r\n" + string.Join("\r\n", answeredWithSuccess));
         Assert.GreaterOrEqual(refused, 50, "Too few writes were refused for the sweep to mean anything:\r\n" + string.Join("\r\n", tried));

         Assert.IsNotNull(_application.Domains.get_ItemByName("example.test"), "The domain is still there.");
         Assert.IsTrue(AccountOverCom("target@example.test").ValidatePassword(AccountPassword), "The target's password is unchanged.");
      }

      [Test]
      [Description("An API key with a Role line is that role; a key without one is its scope exactly as before, and a role that does not exist is refused when the key is made")]
      public void AKeyWithARoleIsThatRole()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);

         (int status, string body) helpdeskKey = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest desk\",\"role\":\"helpdesk\",\"domains\":\"example.test\"}");
         Assert.AreEqual(201, helpdeskKey.status, helpdeskKey.body);
         StringAssert.Contains("\"role\":\"helpdesk\"", helpdeskKey.body);
         string deskToken = JsonValue(helpdeskKey.body, "key");

         (int status, string body) reset = Bearer(deskToken, "POST", "/api/v1/accounts/user@example.test/password", "{\"password\":\"Reset-by-a-key-77\"}");
         Assert.AreEqual(200, reset.status, reset.body);

         (int status, string body) settings = Bearer(deskToken, "PUT", "/api/v1/settings", "{}");
         Assert.AreEqual(403, settings.status, settings.body);
         StringAssert.Contains("the role 'helpdesk'", settings.body, "A key with a role hears about its role, not about a read-only scope it was never given.");

         (int status, string body) readOnlyKey = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest readonly\",\"scope\":\"readonly\"}");
         Assert.AreEqual(201, readOnlyKey.status, readOnlyKey.body);
         string readOnlyToken = JsonValue(readOnlyKey.body, "key");

         (int status, string body) refused = Bearer(readOnlyToken, "DELETE", "/api/v1/accounts/user@example.test");
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("read-only", refused.body, "A read-only key hears the sentence it has always heard.");

         (int status, string body) who = Bearer(readOnlyToken, "GET", "/api/v1/session");
         Assert.AreEqual(200, who.status, who.body);
         StringAssert.Contains("\"role\":\"auditor\"", who.body, "A read-only scope is the auditor.");

         (int status, string body) nosuch = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest nosuch\",\"role\":\"no-such-role\"}");
         Assert.AreEqual(400, nosuch.status, nosuch.body);
      }

      [Test]
      [Description("A key issued for the domain administrator's role with no domain list cannot make an account a server administrator or lift a domain's limits")]
      public void ADomainadminKeyWithNoDomainsCannotMakeAServerAdministrator()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);

         (int status, string body) key = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest domainadmin\",\"role\":\"domainadmin\"}");
         Assert.AreEqual(201, key.status, key.body);
         string token = JsonValue(key.body, "key");

         (int status, string body) promoted = Bearer(token, "PUT", "/api/v1/accounts/user@example.test", "{\"admin_level\":\"server\"}");
         Assert.AreEqual(403, promoted.status, "An empty domain list is not the server administrator's authority on its own. Body: " + promoted.body);
         Assert.AreEqual(eAdminLevel.hAdminLevelNormal, AccountOverCom("user@example.test").AdminLevel);

         (int status, string body) limits = Bearer(token, "PUT", "/api/v1/domains/example.test", "{\"active\":true,\"max_accounts\":5}");
         Assert.AreEqual(403, limits.status, limits.body);
      }

      [Test]
      [Description("A custom role never holds the authority that hands out authority; a built-in role cannot be edited or deleted; deleting a custom role takes its assignments with it")]
      public void ACustomRoleCannotHoldTheAuthorityThatHandsOutAuthority()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);

         (int status, string body) created = Admin("POST", "/api/v1/roles",
            "{\"name\":\"rolestest-reader\",\"description\":\"reads logs\",\"permissions\":\"view_logs,manage_roles,administer_server,administer_domain,not_a_permission\"}");
         Assert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("\"permissions\":\"view_logs\"", created.body, "Only the one permission a custom role may hold survives.");

         Assert.AreEqual(400, Admin("PUT", "/api/v1/roles/helpdesk", "{\"permissions\":\"manage_settings\"}").status);
         Assert.AreEqual(400, Admin("DELETE", "/api/v1/roles/auditor").status);
         Assert.AreEqual(409, Admin("POST", "/api/v1/roles", "{\"name\":\"rolestest-reader\",\"permissions\":\"view_logs\"}").status);

         Assign("user@example.test", "rolestest-reader", "");
         StringAssert.Contains("rolestest-reader", Admin("GET", "/api/v1/role-assignments").body);

         Assert.AreEqual(200, Admin("DELETE", "/api/v1/roles/rolestest-reader").status);
         StringAssert.DoesNotContain("rolestest-reader", Admin("GET", "/api/v1/role-assignments").body);

         (int status, string body) who = AsAccount("user@example.test", "GET", "/api/v1/session");
         StringAssert.Contains("\"administrative\":false", who.body, "An account whose role was deleted falls back to what its admin level gives it.");
      }

      [Test]
      [Description("A help desk reads the trace of one address in its own domain - exactly that address, where the search itself matches a substring - and nothing server-wide, over REST and over COM; an auditor reads the whole trace")]
      public void AHelpDeskTracesOneAddressInItsDomainAndNothingServerWide()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "otheruser@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@example.test", AccountPassword);
         Assign("helen@example.test", "helpdesk", "");
         Assign("audrey@example.test", "auditor", "");

         _application.Database.ExecuteSQL("delete from hm_messagetrace");
         ServerIniFile.SetSetting("MessageTraceEnabled", "1");
         _application.Reinitialize();

         try
         {
            SmtpClientSimulator.StaticSend("sender@example.com", "user@example.test", "traced", "body");
            SmtpClientSimulator.StaticSend("sender@example.com", "otheruser@example.test", "traced", "body");
            Pop3ClientSimulator.AssertMessageCount("user@example.test", AccountPassword, 1);
            Pop3ClientSimulator.AssertMessageCount("otheruser@example.test", AccountPassword, 1);

            // The search matches a substring, so the administrator's answer for
            // user@example.test carries otheruser@example.test too. That is what
            // makes the help desk's answer below a test of the filter.
            (int status, string body) asAdministrator = Admin("GET", "/api/v1/message-trace/address/user@example.test");
            Assert.AreEqual(200, asAdministrator.status, asAdministrator.body);
            StringAssert.Contains("otheruser@example.test", asAdministrator.body);

            (int status, string body) own = AsAccount("helen@example.test", "GET", "/api/v1/message-trace/address/user@example.test");
            Assert.AreEqual(200, own.status, own.body);
            StringAssert.Contains("\"recipient\":\"user@example.test\"", own.body);
            StringAssert.DoesNotContain("otheruser@example.test", own.body, "A confined role sees the events that name the address exactly.");

            Assert.AreEqual(403, AsAccount("helen@example.test", "GET", "/api/v1/message-trace/address/someone@" + OtherDomain).status);
            Assert.AreEqual(403, AsAccount("helen@example.test", "GET", "/api/v1/message-trace").status, "The recent events are anybody's.");
            Assert.AreEqual(403, AsAccount("helen@example.test", "GET", "/api/v1/message-trace/queue/1").status, "One message carries recipients in any domain.");

            (int status, string body) asAuditor = AsAccount("audrey@example.test", "GET", "/api/v1/message-trace");
            Assert.AreEqual(200, asAuditor.status, asAuditor.body);
            StringAssert.Contains("otheruser@example.test", asAuditor.body);

            var desk = new Application();
            Assert.IsNotNull(desk.Authenticate("helen@example.test", AccountPassword));
            var trace = desk.GlobalObjects.MessageTrace;
            trace.Search("user@example.test");
            Assert.GreaterOrEqual(trace.Count, 1);
            for (int i = 0; i < trace.Count; i++)
            {
               Assert.IsTrue(trace[i].Recipient == "user@example.test" || trace[i].Sender == "user@example.test",
                  "An event that does not name the address was shown: " + trace[i].Sender + " -> " + trace[i].Recipient);
            }

            StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => trace.Search("")).Message);
            StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => trace.SearchByQueueID(1)).Message);
            StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => trace.Search("someone@" + OtherDomain)).Message);
         }
         finally
         {
            ServerIniFile.SetSetting("MessageTraceEnabled", null);
            _application.Reinitialize();
            _application.Database.ExecuteSQL("delete from hm_messagetrace");
         }
      }

      // ------------------------------------------------------------------- COM

      [Test]
      [Description("Over COM a help desk resets a password by address, the new one works, and the settings are refused")]
      public void HelpdeskOverComResetsAPasswordAndIsRefusedTheSettings()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);
         _application.AssignRole("helen@example.test", "helpdesk", "");

         var desk = new Application();
         Assert.IsNotNull(desk.Authenticate("helen@example.test", AccountPassword));
         Assert.AreEqual("helpdesk", desk.CurrentRole);
         StringAssert.Contains("reset_password", desk.CurrentPermissions);

         desk.ResetPassword("user@example.test", "Reset-over-com-55");
         desk.UnlockAccount("user@example.test");
         Assert.IsTrue(AccountOverCom("user@example.test").ValidatePassword("Reset-over-com-55"));

         var ex = Assert.Throws<COMException>(() => { var settings = desk.Settings; });
         StringAssert.Contains(AccessDenied, ex.Message);

         ex = Assert.Throws<COMException>(() => desk.AssignRole("user@example.test", "serveradmin", ""));
         StringAssert.Contains(AccessDenied, ex.Message);
      }

      [Test]
      [Description("Over COM a role confined to one domain gets the same answer for an account in another domain whether it exists or not")]
      public void ADomainConfinedRoleOverComIsToldTheSameAboutEveryAddressItMayNotReach()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);
         SingletonProvider<TestSetup>.Instance.AddAccount(other, "exists@" + OtherDomain, AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         _application.AssignRole("helen@example.test", "helpdesk", "");

         var desk = new Application();
         Assert.IsNotNull(desk.Authenticate("helen@example.test", AccountPassword));

         var exists = Assert.Throws<COMException>(() => desk.ResetPassword("exists@" + OtherDomain, "Nobody-sets-this-2"));
         var missing = Assert.Throws<COMException>(() => desk.ResetPassword("missing@" + OtherDomain, "Nobody-sets-this-2"));
         Assert.AreEqual(exists.Message, missing.Message);
         Assert.IsTrue(AccountOverCom("exists@" + OtherDomain).ValidatePassword(AccountPassword));
      }

      [Test]
      [Description("Over COM an auditor may read the roles and is refused every administrative write: the settings, a new account, a reset, an unlock and a role")]
      public void AnAuditorOverComIsRefusedEveryAdministrativeWrite()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);
         _application.AssignRole("audrey@example.test", "auditor", "");

         var auditor = new Application();
         Assert.IsNotNull(auditor.Authenticate("audrey@example.test", AccountPassword));
         Assert.AreEqual("auditor", auditor.CurrentRole);
         StringAssert.Contains("auditor", auditor.Roles);
         StringAssert.Contains("audrey@example.test", auditor.RoleAssignments);

         StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => { var s = auditor.Settings; }).Message);
         StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => auditor.Domains[0].Accounts.Add()).Message);
         StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => auditor.ResetPassword("user@example.test", "Changed-2")).Message);
         StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => auditor.UnlockAccount("user@example.test")).Message);
         StringAssert.Contains(AccessDenied, Assert.Throws<COMException>(() => auditor.AssignRole("user@example.test", "auditor", "")).Message);

         Assert.AreEqual(1, auditor.Domains[0].Accounts.Count, "Over COM an auditor still reaches only its own account object.");
         Assert.IsTrue(AccountOverCom("user@example.test").ValidatePassword(AccountPassword));
      }

      [Test]
      [Description("An account cannot reach another account's object through a permission on a folder of its own, and so cannot set that account's password")]
      public void AnAccountCannotReachAnotherAccountThroughAPermissionOnItsOwnFolder()
      {
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "owner@example.test", AccountPassword);
         Account victim = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "victim@example.test", AccountPassword);

         IMAPFolderPermission grant = owner.IMAPFolders.get_ItemByName("INBOX").Permissions.Add();
         grant.PermissionType = eACLPermissionType.ePermissionTypeUser;
         grant.PermissionAccountID = victim.ID;
         grant.set_Permission(eACLPermission.ePermissionRead, true);
         grant.Save();

         Assert.AreEqual("victim@example.test", owner.IMAPFolders.get_ItemByName("INBOX").Permissions[0].Account.Address,
            "The server administrator reaches the account a permission names, as it always has.");

         var asOwner = new Application();
         Assert.IsNotNull(asOwner.Authenticate("owner@example.test", AccountPassword));

         Account ownAccount = asOwner.Domains[0].Accounts.get_ItemByAddress("owner@example.test");
         IMAPFolderPermission seen = ownAccount.IMAPFolders.get_ItemByName("INBOX").Permissions[0];
         Assert.AreEqual(victim.ID, seen.PermissionAccountID, "The owner still sees whom it shared the folder with.");

         var ex = Assert.Throws<COMException>(() => { var reached = seen.Account; });
         StringAssert.Contains(AccessDenied, ex.Message);

         Assert.IsTrue(AccountOverCom("victim@example.test").ValidatePassword(AccountPassword));
      }

      [Test]
      [Description("A role given over COM reads back on the account, in the COM listing and over REST, and taking it away puts the account back where its admin level leaves it")]
      public void ARoleGivenOverComReadsBackEverywhere()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);

         _application.AssignRole("helen@example.test", "helpdesk", "example.test");

         Account helen = AccountOverCom("helen@example.test");
         Assert.AreEqual("helpdesk", helen.RoleName);
         Assert.AreEqual("example.test", helen.RoleDomains);
         StringAssert.Contains("reset_password", helen.RolePermissions);
         StringAssert.Contains("helen@example.test\thelpdesk\texample.test\t0", _application.RoleAssignments);
         StringAssert.Contains("\"address\":\"helen@example.test\",\"role\":\"helpdesk\"", Admin("GET", "/api/v1/role-assignments").body);

         _application.AssignRole("helen@example.test", "", "");
         Assert.AreEqual("", AccountOverCom("helen@example.test").RoleName);
         Assert.AreEqual("", AccountOverCom("helen@example.test").RolePermissions);
      }

      // --------------------------------------------------------------- helpers

      private static string[] Permissions(JsonElement role)
      {
         return role.GetProperty("permissions").GetString()
            .Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries).Select(p => p.Trim()).ToArray();
      }

      private Account AccountOverCom(string address)
      {
         string domainName = address.Substring(address.IndexOf('@') + 1);
         return _application.Domains.get_ItemByName(domainName).Accounts.get_ItemByAddress(address);
      }

      private static void Assign(string address, string role, string domains)
      {
         (int status, string body) assigned = Admin("PUT", "/api/v1/role-assignments/" + address,
            "{\"role\":\"" + role + "\",\"domains\":\"" + domains + "\"}");
         Assert.AreEqual(200, assigned.status, "The administrator could not give " + address + " the role " + role + ". Body: " + assigned.body);
      }

      private static void DeleteTestRoles()
      {
         (int status, string body) roles = Admin("GET", "/api/v1/roles");
         if (roles.status != 200)
            return;

         foreach (Match match in Regex.Matches(roles.body, "\"name\":\"(rolestest-[a-z0-9_-]+)\""))
            Admin("DELETE", "/api/v1/roles/" + match.Groups[1].Value);
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

      private static string JsonValue(string json, string name)
      {
         Match match = Regex.Match(json, "\"" + name + "\"\\s*:\\s*\"([^\"]*)\"");
         return match.Success ? match.Groups[1].Value : null;
      }

      private static (int status, string body) Admin(string method, string path, string requestBody = null)
      {
         return Http(method, path, "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)), requestBody);
      }

      private static (int status, string body) AsAccount(string address, string method, string path, string requestBody = null)
      {
         return Http(method, path, "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + AccountPassword)), requestBody);
      }

      private static (int status, string body) Bearer(string token, string method, string path, string requestBody = null)
      {
         return Http(method, path, "Bearer " + token, requestBody);
      }

      // One HTTP/1.0 request against the REST listener, as RestApiAuthorization
      // makes them: the status and the body, with the connect retried briefly for
      // the bind race after a reinitialise.
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

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");

               if (authorization != null)
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
