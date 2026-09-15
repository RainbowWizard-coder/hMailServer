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
   ///      * a help desk allowed a password reset and refused a settings change,
   ///        and refused the password of anybody who holds more than it does;
   ///      * the two historical levels exactly as they were, with no row written;
   ///      * the reports, which need the configuration and the message trace both;
   ///      * the COM path that let any account reach another account's full
   ///        object through a permission on a folder of its own, closed;
   ///      * and the audit trail, which records a change made under a role with
   ///        the kind the role gives it and the role's name, over REST and over
   ///        COM, and which the auditor reads and no API key does.
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
      [Description("A domain administrator sees who holds a role in its own domain and nobody in another, over REST and over COM, and is refused the server-wide settings")]
      public void ADomainAdministratorSeesOnlyItsOwnDomainsAssignments()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);
         Account dora = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dora@example.test", AccountPassword);
         dora.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         dora.Save();

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(other, "olaf@" + OtherDomain, AccountPassword);
         Assign("helen@example.test", "helpdesk", "");
         Assign("olaf@" + OtherDomain, "helpdesk", "");

         (int status, string body) mine = AsAccount("dora@example.test", "GET", "/api/v1/role-assignments");
         Assert.AreEqual(200, mine.status, mine.body);
         StringAssert.Contains("helen@example.test", mine.body);
         StringAssert.DoesNotContain("olaf@", mine.body, "Who administers another domain is not a domain administrator's to know.");

         var overCom = new Application();
         Assert.IsNotNull(overCom.Authenticate("dora@example.test", AccountPassword));
         StringAssert.Contains("helen@example.test", overCom.RoleAssignments);
         StringAssert.DoesNotContain("olaf@", overCom.RoleAssignments);

         (int status, string body) settings = AsAccount("dora@example.test", "GET", "/api/v1/settings");
         Assert.AreEqual(403, settings.status, "The settings are server-wide. Body: " + settings.body);
         StringAssert.Contains("restricted to named domains", settings.body);
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
      [Description("A credential may not take over an account whose authority it does not hold: a help desk cannot reset a domain administrator's or an auditor's password in its own domain, over REST or COM, and a domain administrator cannot save an auditor's account")]
      public void ARoleCannotTakeOverAnAccountWithMoreAuthority()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         Account boss = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "boss@example.test", AccountPassword);
         boss.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         boss.Save();
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);
         Assign("helen@example.test", "helpdesk", "");
         Assign("audrey@example.test", "auditor", "");

         foreach (string target in new[] { "boss@example.test", "audrey@example.test" })
         {
            (int status, string body) reset = AsAccount("helen@example.test", "POST", "/api/v1/accounts/" + target + "/password",
               "{\"password\":\"Taken-over-3\"}");
            Assert.AreEqual(403, reset.status, "A help desk reset the password of " + target + " and could sign in as it. Body: " + reset.body);
            StringAssert.Contains("holds authority this credential does not", reset.body);
            Assert.IsTrue(AccountOverCom(target).ValidatePassword(AccountPassword), target + "'s password is unchanged.");
         }

         var desk = new Application();
         Assert.IsNotNull(desk.Authenticate("helen@example.test", AccountPassword));
         var refused = Assert.Throws<COMException>(() => desk.ResetPassword("boss@example.test", "Taken-over-3"));
         StringAssert.Contains("holds authority this credential does not", refused.Message);
         desk.ResetPassword("user@example.test", "An-ordinary-reset-4");

         // Over COM a domain administrator reaches every account of its domain, as
         // it always has, and saves an ordinary one - but not the auditor's.
         var dora = new Application();
         Assert.IsNotNull(dora.Authenticate("boss@example.test", AccountPassword));
         Account ordinary = dora.Domains[0].Accounts.get_ItemByAddress("user@example.test");
         ordinary.PersonFirstName = "Ordinary";
         ordinary.Save();

         Account auditor = dora.Domains[0].Accounts.get_ItemByAddress("audrey@example.test");
         auditor.Password = "Taken-over-5";
         var saveRefused = Assert.Throws<COMException>(() => auditor.Save());
         StringAssert.Contains("holds administrative authority that this credential does not", saveRefused.Message);
         Assert.IsTrue(AccountOverCom("audrey@example.test").ValidatePassword(AccountPassword));
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
      [Description("A custom role holding a server-wide permission is confined to the account's own domain when the assignment names none, and so refused the settings; given * it reaches every domain and reads them")]
      public void AStarGivesAConfinedRoleEveryDomain()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sam@example.test", AccountPassword);

         Assert.AreEqual(201, Admin("POST", "/api/v1/roles",
            "{\"name\":\"rolestest-settings\",\"permissions\":\"view_configuration,manage_settings\"}").status);

         Assign("sam@example.test", "rolestest-settings", "");
         (int status, string body) confined = AsAccount("sam@example.test", "GET", "/api/v1/settings");
         Assert.AreEqual(403, confined.status, "Unlisted, a custom role reaches the account's own domain, and the settings are server-wide. Body: " + confined.body);
         StringAssert.Contains("\"every_domain\":false", Admin("GET", "/api/v1/role-assignments").body);

         Assign("sam@example.test", "rolestest-settings", "*");
         (int status, string body) everywhere = AsAccount("sam@example.test", "GET", "/api/v1/settings");
         Assert.AreEqual(200, everywhere.status, everywhere.body);
         StringAssert.Contains("\"every_domain\":true", Admin("GET", "/api/v1/role-assignments").body);
         StringAssert.Contains("\"domains\":[]", AsAccount("sam@example.test", "GET", "/api/v1/session").body);
      }

      [Test]
      [Description("The reports need view_configuration and view_message_trace, both, because their senders and recipients are who mails whom: a custom role holding view_configuration alone is refused every report and told what it lacks, holding both it reads them, a domain administrator and an auditor read them as before, and a help desk is refused as before")]
      public void TheReportsNeedTheConfigurationAndTheMessageTraceBoth()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rita@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dora@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);

         Assert.AreEqual(201, Admin("POST", "/api/v1/roles",
            "{\"name\":\"rolestest-config\",\"permissions\":\"view_configuration\"}").status);
         Assert.AreEqual(201, Admin("POST", "/api/v1/roles",
            "{\"name\":\"rolestest-config-trace\",\"permissions\":\"view_configuration,view_message_trace\"}").status);

         // * so that the confinement is not what refuses it: this is about the
         // permission, and the settings below prove the role reaches the server.
         Assign("rita@example.test", "rolestest-config", "*");
         Assign("dora@example.test", "domainadmin", "");
         Assign("audrey@example.test", "auditor", "");
         Assign("helen@example.test", "helpdesk", "");

         (int status, string body) settings = AsAccount("rita@example.test", "GET", "/api/v1/settings");
         Assert.AreEqual(200, settings.status, "The role reads the configuration. Body: " + settings.body);

         foreach (string path in new[] { "/api/v1/reports", "/api/v1/reports/senders", "/api/v1/reports/recipients", "/api/v1/reports/traffic", "/api/v1/reports/summary" })
         {
            (int status, string body) refused = AsAccount("rita@example.test", "GET", path);
            Assert.AreEqual(403, refused.status, "A role that reads the configuration and not the message trace read " + path + ". Body: " + refused.body);
            StringAssert.Contains("the role 'rolestest-config' does not hold the permission 'view_message_trace'", refused.body);
         }

         Assign("rita@example.test", "rolestest-config-trace", "*");
         (int status, string body) both = AsAccount("rita@example.test", "GET", "/api/v1/reports/senders");
         Assert.AreEqual(200, both.status, "Holding both, the role reads who mails whom. Body: " + both.body);

         // The built-in outcomes do not move.
         (int status, string body) domainAdministrator = AsAccount("dora@example.test", "GET", "/api/v1/reports/senders?domain=example.test");
         Assert.AreEqual(200, domainAdministrator.status, domainAdministrator.body);

         (int status, string body) auditor = AsAccount("audrey@example.test", "GET", "/api/v1/reports/recipients");
         Assert.AreEqual(200, auditor.status, auditor.body);

         (int status, string body) desk = AsAccount("helen@example.test", "GET", "/api/v1/reports/senders?domain=example.test");
         Assert.AreEqual(403, desk.status, "A help desk reads one address's trace, not a domain's traffic. Body: " + desk.body);
         StringAssert.Contains("'view_configuration'", desk.body);
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

      // ------------------------------------------------------- the audit trail

      [Test]
      [Description("A change made under a role is on the audit trail with the role's name and the kind the role gives it: a help desk whose admin level says domain administrator is an account, over REST and over COM, and a domain administrator by assignment is an administrator")]
      public void AChangeMadeUnderARoleIsRecordedWithTheRoleAndTheKindItGives()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dora@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);

         _application.Database.ExecuteSQL("delete from hm_audit");

         // The admin level says domain administrator; the role says help desk. The
         // record must follow the role - the authority every decision was made on.
         Account helen = AccountOverCom("helen@example.test");
         helen.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         helen.Save();
         Assign("helen@example.test", "helpdesk", "");
         Assign("dora@example.test", "domainadmin", "");

         (int status, string body) reset = AsAccount("helen@example.test", "POST", "/api/v1/accounts/user@example.test/password",
            "{\"password\":\"Reset-on-the-record-31\"}");
         Assert.AreEqual(200, reset.status, reset.body);

         var desk = new Application();
         Assert.IsNotNull(desk.Authenticate("helen@example.test", AccountPassword));
         desk.ResetPassword("user@example.test", "Reset-on-the-record-over-com-32");

         (int status, string body) update = AsAccount("dora@example.test", "PUT", "/api/v1/accounts/user@example.test", "{\"max_size_mb\":7}");
         Assert.AreEqual(200, update.status, update.body);

         (int status, string body) trail = Admin("GET", "/api/v1/audit?object_type=account&limit=100");
         Assert.AreEqual(200, trail.status, trail.body);

         JsonElement overRest = AuditRow(trail.body, "account:helen@example.test", "REST");
         Assert.AreEqual("account", overRest.GetProperty("actor_type").GetString(),
            "A help desk is recorded as an administrator because its admin level says domain administrator. Row: " + overRest);
         StringAssert.Contains("[role: helpdesk]", overRest.GetProperty("detail").GetString());
         Assert.AreEqual("user@example.test", overRest.GetProperty("object").GetString());

         JsonElement overCom = AuditRow(trail.body, "helen@example.test", "COM");
         Assert.AreEqual("account", overCom.GetProperty("actor_type").GetString(), "Row: " + overCom);
         StringAssert.Contains("[role: helpdesk]", overCom.GetProperty("detail").GetString(),
            "A password a help desk resets over COM - Application.ResetPassword asks only the role question - is not on the record as the help desk's. Row: " + overCom);

         JsonElement domainAdministrator = AuditRow(trail.body, "account:dora@example.test", "REST");
         Assert.AreEqual("administrator", domainAdministrator.GetProperty("actor_type").GetString(), "Row: " + domainAdministrator);
         StringAssert.Contains("[role: domainadmin]", domainAdministrator.GetProperty("detail").GetString());

         // And the assignments themselves are on the record, made by the
         // administrator password - whose role, too, is written down.
         (int status, string body) everything = Admin("GET", "/api/v1/audit?limit=200");
         Assert.AreEqual(200, everything.status, everything.body);

         using (JsonDocument document = JsonDocument.Parse(everything.body))
         {
            List<JsonElement> given = document.RootElement.GetProperty("entries").EnumerateArray()
               .Where(e => e.GetProperty("object_type").GetString() == "role assignment").Select(e => e.Clone()).ToList();

            Assert.AreEqual(2, given.Count, "Two roles were given, so two role assignments are on the record. Trail: " + everything.body);
            Assert.IsTrue(given.All(e => e.GetProperty("actor").GetString() == "administrator" &&
                                         e.GetProperty("actor_type").GetString() == "administrator" &&
                                         e.GetProperty("action").GetString() == "created" &&
                                         e.GetProperty("detail").GetString().Contains("[role: serveradmin]")),
               "Trail: " + everything.body);

            // In words: the account by its address and the role by its name, never
            // the ids the row is made of.
            JsonElement helensRole = given.Single(e => e.GetProperty("object").GetString() == "helen@example.test");
            StringAssert.StartsWith("role: helpdesk; domains: the account's own domain", helensRole.GetProperty("detail").GetString());
            JsonElement dorasRole = given.Single(e => e.GetProperty("object").GetString() == "dora@example.test");
            StringAssert.StartsWith("role: domainadmin; domains: the account's own domain", dorasRole.GetProperty("detail").GetString());
            Assert.IsFalse(given.Any(e => e.GetProperty("detail").GetString().Contains("raaccountid") ||
                                          e.GetProperty("detail").GetString().Contains("raroleid")),
               "A role assignment is on the record as its ids. Trail: " + everything.body);
         }

         (int status, string body) verified = Admin("GET", "/api/v1/audit/verify");
         Assert.AreEqual(200, verified.status, verified.body);
         StringAssert.Contains("\"intact\":true", verified.body, "The role in the detail is part of the hashed row.");
      }

      [Test]
      [Description("The audit trail is the auditor's to read, and the server administrator's; a domain administrator, a confined auditor and every API key are refused it - a key of any scope or role on all seven audit and alert routes, with a 401")]
      public void TheAuditorReadsTheAuditTrailAndNoKeyDoes()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "narrow@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dora@example.test", AccountPassword);
         Assign("audrey@example.test", "auditor", "");
         Assign("narrow@example.test", "auditor", "example.test");
         Assign("dora@example.test", "domainadmin", "");

         foreach (string path in new[] { "/api/v1/audit", "/api/v1/audit/verify", "/api/v1/alerts/rules", "/api/v1/alerts/events" })
         {
            (int status, string body) read = AsAccount("audrey@example.test", "GET", path);
            Assert.AreEqual(200, read.status, "An auditor reaching every domain must be able to read " + path + ". Body: " + read.body);
         }

         (int status, string body) run = AsAccount("audrey@example.test", "POST", "/api/v1/alerts/run", "{}");
         Assert.AreEqual(403, run.status, "An auditor does not run the alerting. Body: " + run.body);

         (int status, string body) domainAdmin = AsAccount("dora@example.test", "GET", "/api/v1/audit");
         Assert.AreEqual(403, domainAdmin.status, domainAdmin.body);
         StringAssert.Contains("view_audit", domainAdmin.body);

         (int status, string body) confined = AsAccount("narrow@example.test", "GET", "/api/v1/audit");
         Assert.AreEqual(403, confined.status, "The trail records every domain's changes. Body: " + confined.body);
         StringAssert.Contains("server-wide", confined.body);

         (int status, string body) auditorKey = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest audit\",\"role\":\"auditor\"}");
         Assert.AreEqual(201, auditorKey.status, auditorKey.body);
         (int status, string body) byKey = Bearer(JsonValue(auditorKey.body, "key"), "GET", "/api/v1/audit");
         Assert.AreEqual(401, byKey.status, "A key of any role or scope is refused the audit trail, as a 401. Body: " + byKey.body);

         (int status, string body) readOnlyKey = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest audit readonly\",\"scope\":\"readonly\"}");
         Assert.AreEqual(201, readOnlyKey.status, readOnlyKey.body);
         (int status, string body) byReadOnlyKey = Bearer(JsonValue(readOnlyKey.body, "key"), "GET", "/api/v1/alerts/rules");
         Assert.AreEqual(401, byReadOnlyKey.status, byReadOnlyKey.body);

         // And the widest keys there are - a full scope, and the server
         // administrator's role written on the key - on every one of the seven
         // routes, the writes included. An account holding a role reaches these
         // through the permission decision; a key reaches none of them.
         (int status, string body) fullKey = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest audit full\",\"scope\":\"full\"}");
         Assert.AreEqual(201, fullKey.status, fullKey.body);
         (int status, string body) serverAdminKey = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rolestest audit serveradmin\",\"role\":\"serveradmin\"}");
         Assert.AreEqual(201, serverAdminKey.status, serverAdminKey.body);

         var routes = new[]
         {
            new[] { "GET", "/api/v1/audit", null },
            new[] { "GET", "/api/v1/audit/verify", null },
            new[] { "GET", "/api/v1/alerts/rules", null },
            new[] { "PUT", "/api/v1/alerts/rules/disk.low", "{}" },
            new[] { "GET", "/api/v1/alerts/events", null },
            new[] { "POST", "/api/v1/alerts/test", "{\"condition\":\"disk.low\"}" },
            new[] { "POST", "/api/v1/alerts/run", "{}" },
         };

         foreach (string token in new[] { JsonValue(fullKey.body, "key"), JsonValue(serverAdminKey.body, "key") })
         {
            foreach (string[] route in routes)
            {
               (int status, string body) refusedKey = Bearer(token, route[0], route[1], route[2]);
               Assert.AreEqual(401, refusedKey.status, "A key reached " + route[0] + " " + route[1] + ". Body: " + refusedKey.body);
            }
         }

         (int status, string body) administrator = Admin("GET", "/api/v1/audit");
         Assert.AreEqual(200, administrator.status, administrator.body);
      }

      [Test]
      [Description("A role given, changed and taken away, and a custom role made, changed and deleted, are on the audit trail in words - the account's address and the role's name, before and after - and never as the ids their rows are made of")]
      public void RoleChangesAreOnTheRecordInWordsBeforeAndAfter()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "olga@example.test", AccountPassword);

         _application.Database.ExecuteSQL("delete from hm_audit");

         Assign("helen@example.test", "helpdesk", "");
         Assign("helen@example.test", "auditor", "*");
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/role-assignments/helen@example.test").status);

         // Taking away a role from an account that holds none takes nothing, and
         // writes nothing: COM's AssignRole with an empty role reaches Unassign
         // for any account.
         _application.AssignRole("helen@example.test", "", "");

         Assert.AreEqual(201, Admin("POST", "/api/v1/roles",
            "{\"name\":\"rolestest-words\",\"description\":\"reads logs\",\"permissions\":\"view_logs\"}").status);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/roles/rolestest-words",
            "{\"description\":\"reads logs and the queue\",\"permissions\":\"view_logs,view_queue\"}").status);
         Assign("olga@example.test", "rolestest-words", "");
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/roles/rolestest-words").status);

         // A role nobody held, deleted: the role's row, and no row saying that
         // nobody's assignment went.
         Assert.AreEqual(201, Admin("POST", "/api/v1/roles",
            "{\"name\":\"rolestest-unheld\",\"description\":\"\",\"permissions\":\"view_logs\"}").status);
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/roles/rolestest-unheld").status);

         (int status, string body) trail = Admin("GET", "/api/v1/audit?limit=200");
         Assert.AreEqual(200, trail.status, trail.body);

         using (JsonDocument document = JsonDocument.Parse(trail.body))
         {
            List<JsonElement> rows = document.RootElement.GetProperty("entries").EnumerateArray()
               .Where(e => e.GetProperty("object_type").GetString() == "role assignment" || e.GetProperty("object_type").GetString() == "role")
               .Select(e => e.Clone()).ToList();

            Func<string, string, string, JsonElement> row = (objectType, action, objectName) =>
            {
               List<JsonElement> found = rows.Where(e => e.GetProperty("object_type").GetString() == objectType &&
                                                         e.GetProperty("action").GetString() == action &&
                                                         e.GetProperty("object").GetString() == objectName).ToList();
               Assert.AreEqual(1, found.Count, "Expected one " + objectType + " " + action + " for " + objectName + ". Trail: " + trail.body);
               return found[0];
            };

            StringAssert.StartsWith("role: helpdesk; domains: the account's own domain",
               row("role assignment", "created", "helen@example.test").GetProperty("detail").GetString());
            StringAssert.StartsWith("role: helpdesk -> auditor; domains: the account's own domain -> every domain",
               row("role assignment", "updated", "helen@example.test").GetProperty("detail").GetString());
            StringAssert.StartsWith("role: auditor -> (none); domains: every domain -> (none)",
               row("role assignment", "deleted", "helen@example.test").GetProperty("detail").GetString());

            StringAssert.StartsWith("permissions: view_logs; description: 'reads logs'",
               row("role", "created", "rolestest-words").GetProperty("detail").GetString());
            StringAssert.StartsWith("permissions: view_logs -> view_queue,view_logs; description: 'reads logs' -> 'reads logs and the queue'",
               row("role", "updated", "rolestest-words").GetProperty("detail").GetString());
            StringAssert.StartsWith("role: rolestest-words -> (none), because the role was deleted; held by olga@example.test",
               row("role assignment", "deleted", "olga@example.test").GetProperty("detail").GetString());
            StringAssert.StartsWith("permissions: view_queue,view_logs; description: 'reads logs and the queue'",
               row("role", "deleted", "rolestest-words").GetProperty("detail").GetString());
            row("role", "deleted", "rolestest-unheld");

            Assert.IsFalse(rows.Any(e => e.GetProperty("detail").GetString().Contains("raaccountid") ||
                                         e.GetProperty("detail").GetString().Contains("raroleid") ||
                                         e.GetProperty("object").GetString().StartsWith("roleid") ||
                                         e.GetProperty("object").GetString().StartsWith("raid")),
               "A role change is on the record as the ids its row is made of. Trail: " + trail.body);

            // No row for a change that changed nothing: the role taken from an
            // account holding none, and the assignments of a role nobody held.
            Assert.IsFalse(rows.Any(e => e.GetProperty("detail").GetString().Contains("(none) -> (none)") ||
                                         e.GetProperty("object").GetString().Contains("nobody held it") ||
                                         (e.GetProperty("object_type").GetString() == "role assignment" &&
                                          e.GetProperty("detail").GetString().Contains("rolestest-unheld"))),
               "A change that changed nothing is on the record. Trail: " + trail.body);
         }

         (int status, string body) verified = Admin("GET", "/api/v1/audit/verify");
         Assert.AreEqual(200, verified.status, verified.body);
         StringAssert.Contains("\"intact\":true", verified.body);
      }

      [Test]
      [Description("An unlock changes no row and no setting and is on the audit trail all the same: a help desk's unlock over REST and over COM is a row naming the account, the action unlocked and the role, under the help desk's own name")]
      public void AnUnlockIsOnTheAuditTrail()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@example.test", AccountPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", AccountPassword);
         Assign("helen@example.test", "helpdesk", "");

         _application.Database.ExecuteSQL("delete from hm_audit");

         (int status, string body) overRest = AsAccount("helen@example.test", "POST", "/api/v1/accounts/user@example.test/unlock");
         Assert.AreEqual(200, overRest.status, overRest.body);

         var desk = new Application();
         Assert.IsNotNull(desk.Authenticate("helen@example.test", AccountPassword));
         desk.UnlockAccount("user@example.test");

         (int status, string body) trail = Admin("GET", "/api/v1/audit?action=unlocked&limit=100");
         Assert.AreEqual(200, trail.status, trail.body);

         using (JsonDocument document = JsonDocument.Parse(trail.body))
         {
            List<JsonElement> unlocks = document.RootElement.GetProperty("entries").EnumerateArray().Select(e => e.Clone()).ToList();

            Assert.AreEqual(2, unlocks.Count,
               "One unlock over REST and one over COM, so two unlocked rows - an unlock writes no row and no setting, so without an explicit record there are none. Trail: " + trail.body);
            Assert.IsTrue(unlocks.All(e => e.GetProperty("object_type").GetString() == "account" &&
                                           e.GetProperty("object").GetString() == "user@example.test" &&
                                           e.GetProperty("detail").GetString().Contains("[role: helpdesk]")),
               "Trail: " + trail.body);
         }

         JsonElement restRow = AuditRow(trail.body, "account:helen@example.test", "REST");
         Assert.AreEqual("account", restRow.GetProperty("actor_type").GetString());
         JsonElement comRow = AuditRow(trail.body, "helen@example.test", "COM");
         Assert.AreEqual("account", comRow.GetProperty("actor_type").GetString());

         (int status, string body) verified = Admin("GET", "/api/v1/audit/verify");
         Assert.AreEqual(200, verified.status, verified.body);
         StringAssert.Contains("\"intact\":true", verified.body);
      }

      [Test]
      [Description("An event script that signs in to COM in process leaves no actor on the server thread it ran on: when OnClientValidatePassword authenticates as the administrator, asks for the settings and refuses the logon, the auto-ban range the same thread then writes is not on the audit trail as the administrator's")]
      public void AScriptLeavesNoActorOnTheThreadItRanOn()
      {
         const string victim = "victim@example.test";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, victim, AccountPassword);

         bool autoBan = _settings.AutoBanOnLogonFailure;
         int maxAttempts = _settings.MaxInvalidLogonAttempts;
         int within = _settings.MaxInvalidLogonAttemptsWithin;
         int minutes = _settings.AutoBanMinutes;
         var scripting = _settings.Scripting;

         try
         {
            // One refused logon bans the address. The ban is an INSERT into
            // hm_securityranges - an audited table - made by the server itself, on
            // the thread that ran the logon, straight after the script: exactly the
            // write that must be recorded as nobody's.
            _settings.AutoBanOnLogonFailure = true;
            _settings.MaxInvalidLogonAttempts = 1;
            _settings.MaxInvalidLogonAttemptsWithin = 5;
            _settings.AutoBanMinutes = 5;
            _settings.ClearLogonFailureList();

            // The administrator password rather than an account's, so that signing
            // in does not validate a password and fire this handler again. Reading
            // Settings asks GetIsServerAdmin, which installs the actor on the thread;
            // Result.Value is set only after it, so a ban below proves both ran.
            string script =
               "Sub OnClientValidatePassword(account, password)\r\n" +
               "   If LCase(account.Address) <> \"" + victim + "\" Then Exit Sub\r\n" +
               "   Dim app, settings\r\n" +
               "   Set app = CreateObject(\"hMailServer.Application\")\r\n" +
               "   Call app.Authenticate(\"Administrator\", \"" + AdminPassword + "\")\r\n" +
               "   Set settings = app.Settings\r\n" +
               "   Result.Value = 1\r\n" +
               "End Sub\r\n";

            File.WriteAllText(scripting.CurrentScriptFile, script);
            scripting.Enabled = true;
            scripting.Reload();

            _application.Database.ExecuteSQL("delete from hm_audit");

            Assert.IsFalse(ImapClientSimulator.ValidatePassword(victim, AccountPassword),
               "The script refuses every password for " + victim + ".");

            _settings.SecurityRanges.Refresh();
            SecurityRange ban = null;
            try
            {
               ban = _settings.SecurityRanges.get_ItemByName("Auto-ban: " + victim);
            }
            catch (COMException)
            {
            }

            Assert.IsNotNull(ban,
               "The refused logon did not ban the address, so either the script did not reach Result.Value - its COM calls failed - or the ban was not written, and the test proves nothing.");

            // Lifted before anything else connects from this address.
            ban.Delete();

            (int status, string body) trail = Admin("GET", "/api/v1/audit?action=created&limit=500");
            Assert.AreEqual(200, trail.status, trail.body);

            using (JsonDocument document = JsonDocument.Parse(trail.body))
            {
               List<JsonElement> bans = document.RootElement.GetProperty("entries").EnumerateArray()
                  .Where(e => e.GetProperty("object_type").GetString() == "ip range").Select(e => e.Clone()).ToList();

               Assert.IsEmpty(bans,
                  "The auto-ban the server wrote on the script's thread is on the audit trail under the credential the script signed in to COM with - the actor outlived the script. Trail: " + trail.body);
            }
         }
         finally
         {
            scripting.Enabled = false;
            File.WriteAllText(scripting.CurrentScriptFile, "");
            scripting.Reload();

            try
            {
               _settings.SecurityRanges.Refresh();
               _settings.SecurityRanges.get_ItemByName("Auto-ban: " + victim).Delete();
            }
            catch (COMException)
            {
               // Already lifted above.
            }

            _settings.AutoBanOnLogonFailure = autoBan;
            _settings.MaxInvalidLogonAttempts = maxAttempts;
            _settings.MaxInvalidLogonAttemptsWithin = within;
            _settings.AutoBanMinutes = minutes;
            _settings.ClearLogonFailureList();
         }
      }

      // --------------------------------------------------------------- helpers

      // The newest audit row made by this actor over this interface.
      private static JsonElement AuditRow(string trail, string actor, string interfaceName)
      {
         using (JsonDocument document = JsonDocument.Parse(trail))
         {
            foreach (JsonElement entry in document.RootElement.GetProperty("entries").EnumerateArray())
            {
               if (entry.GetProperty("actor").GetString() == actor && entry.GetProperty("interface").GetString() == interfaceName)
                  return entry.Clone();
            }
         }

         Assert.Fail("No audit row by " + actor + " over " + interfaceName + ". Trail: " + trail);
         return default(JsonElement);
      }

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
