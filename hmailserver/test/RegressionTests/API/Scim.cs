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
using System.Threading;
using DBUpdater;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    SCIM 2.0 provisioning (docs/Scim.md): /scim/v2 under the REST listener, where the
   ///    organisation's identity provider makes, renames, suspends and removes accounts,
   ///    with their aliases, and distribution lists. Every claim stands beside its negative
   ///    control: a credential that is not a key that provisions, a domain the key does not
   ///    reach, a filter this server does not answer, a stale version, a weak password, the
   ///    account's own address removed, the account as it was before the write.
   ///
   ///    Keys are issued per test through POST /api/v1/apikeys and revoked in the teardown,
   ///    as the send API's fixture issues them. The accounts SCIM makes are read back over
   ///    COM, over IMAP and over SMTP - the protocols a person uses - and not only over SCIM.
   /// </summary>
   [TestFixture]
   public class Scim : TestFixtureBase
   {
      private static int RestPort = 9936;
      private const string AdminPassword = "testar";
      private const string Password = "Scim-Provisioned-Passw0rd!";
      private const string OtherDomain = "scim-other.test";
      private const string ProvisionRole = "scimtest-provisioner";
      private const string ReaderRole = "scimtest-reader";

      private const string UserSchema = "urn:ietf:params:scim:schemas:core:2.0:User";
      private const string GroupSchema = "urn:ietf:params:scim:schemas:core:2.0:Group";
      private const string ErrorSchema = "urn:ietf:params:scim:api:messages:2.0:Error";
      private const string ListSchema = "urn:ietf:params:scim:api:messages:2.0:ListResponse";
      private const string PatchSchema = "urn:ietf:params:scim:api:messages:2.0:PatchOp";

      private readonly List<string> _keys = new List<string>();
      private bool _policySet;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         ClassicAssert.AreEqual(200, Admin("GET", "/api/v1/status", null).Status, "REST API did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // Each step on its own: a key revocation that throws - the listener
            // down - must not leave a role behind, which the next test's MakeRole
            // would be refused.
            foreach (string id in _keys)
               Tidy(() => Admin("DELETE", "/api/v1/apikeys/" + id, null));
            _keys.Clear();

            Tidy(() => Admin("DELETE", "/api/v1/roles/" + ProvisionRole, null));
            Tidy(() => Admin("DELETE", "/api/v1/roles/" + ReaderRole, null));

            Tidy(() =>
            {
               Domain other = _application.Domains.get_ItemByName(OtherDomain);
               if (other != null)
                  _application.Domains.DeleteByDBID(other.ID);
            });
         }
         finally
         {
            if (_policySet)
            {
               ServerIniFile.SetSetting("PasswordPolicyMinimumLength", null);
               _policySet = false;
            }

            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---------------------------------------------------------------- discovery

      [Test]
      [Description("ServiceProviderConfig, Schemas and ResourceTypes answer RFC 7644's shapes in application/scim+json to a key that provisions - a custom role holding provision alone included. No credential, the administrator password and an account's password are 401 in SCIM's error shape with a Bearer challenge; a key whose role does not hold provision is 403; a path SCIM does not serve is SCIM's 404, and bulk its 501.")]
      public void DiscoveryAnswersAKeyThatProvisionsAndNothingElse()
      {
         var key = MakeKey("scim discovery");

         Reply config = AsKey(key.token, "GET", "/scim/v2/ServiceProviderConfig", null);
         ClassicAssert.AreEqual(200, config.Status, config.Body);
         StringAssert.StartsWith("application/scim+json", config.Header("Content-Type"));
         JsonElement root = config.Json;
         ClassicAssert.IsTrue(root.GetProperty("patch").GetProperty("supported").GetBoolean());
         ClassicAssert.IsFalse(root.GetProperty("bulk").GetProperty("supported").GetBoolean());
         ClassicAssert.IsTrue(root.GetProperty("filter").GetProperty("supported").GetBoolean());
         ClassicAssert.AreEqual(200, root.GetProperty("filter").GetProperty("maxResults").GetInt32());
         ClassicAssert.IsTrue(root.GetProperty("changePassword").GetProperty("supported").GetBoolean());
         ClassicAssert.IsFalse(root.GetProperty("sort").GetProperty("supported").GetBoolean());
         ClassicAssert.IsTrue(root.GetProperty("etag").GetProperty("supported").GetBoolean());
         ClassicAssert.AreEqual("oauthbearertoken", root.GetProperty("authenticationSchemes")[0].GetProperty("type").GetString());

         Reply schemas = AsKey(key.token, "GET", "/scim/v2/Schemas", null);
         ClassicAssert.AreEqual(200, schemas.Status, schemas.Body);
         StringAssert.StartsWith("application/scim+json", schemas.Header("Content-Type"));
         ClassicAssert.AreEqual(ListSchema, schemas.Json.GetProperty("schemas")[0].GetString());
         ClassicAssert.AreEqual(2, schemas.Json.GetProperty("totalResults").GetInt32());
         var schemaIds = schemas.Json.GetProperty("Resources").EnumerateArray().Select(r => r.GetProperty("id").GetString()).ToList();
         CollectionAssert.AreEquivalent(new[] { UserSchema, GroupSchema }, schemaIds);

         Reply userSchema = AsKey(key.token, "GET", "/scim/v2/Schemas/" + UserSchema, null);
         ClassicAssert.AreEqual(200, userSchema.Status, userSchema.Body);
         var attributes = userSchema.Json.GetProperty("attributes").EnumerateArray().Select(a => a.GetProperty("name").GetString()).ToList();
         CollectionAssert.Contains(attributes, "userName");
         CollectionAssert.Contains(attributes, "emails");
         CollectionAssert.DoesNotContain(attributes, "phoneNumbers", "The schema lists what this server keeps, and it keeps no telephone number.");

         Reply types = AsKey(key.token, "GET", "/scim/v2/ResourceTypes", null);
         ClassicAssert.AreEqual(200, types.Status, types.Body);
         var endpoints = types.Json.GetProperty("Resources").EnumerateArray().Select(r => r.GetProperty("endpoint").GetString()).ToList();
         CollectionAssert.AreEquivalent(new[] { "/Users", "/Groups" }, endpoints);
         ClassicAssert.AreEqual(200, AsKey(key.token, "GET", "/scim/v2/ResourceTypes/Group", null).Status);

         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Nothing", null), 404, null);
         AssertScimError(AsKey(key.token, "POST", "/scim/v2/Bulk", "{}"), 501, null);

         // The permission alone is enough: a custom role that provisions and does
         // nothing else.
         MakeRole(ProvisionRole, "provision");
         var provisioner = MakeKey("scim provisioner", null, ProvisionRole);
         ClassicAssert.AreEqual(200, AsKey(provisioner.token, "GET", "/scim/v2/ServiceProviderConfig", null).Status);

         // The controls.
         Reply nobody = Send("GET", "/scim/v2/ServiceProviderConfig", null, null);
         AssertScimError(nobody, 401, null);
         StringAssert.StartsWith("Bearer", nobody.Header("WWW-Authenticate"), "The challenge is the scheme SCIM answers to.");

         AssertScimError(Admin("GET", "/scim/v2/ServiceProviderConfig", null), 401, null);

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "scim-person@" + _domain.Name, Password);
         AssertScimError(Send("GET", "/scim/v2/Users", Basic("scim-person@" + _domain.Name, Password), null), 401, null);
         ClassicAssert.AreEqual(200, Send("GET", "/api/v1/me", Basic("scim-person@" + _domain.Name, Password), null).Status,
            "The control: the same credential is good at the listener, so the 401 is SCIM's refusal of anything but a key.");

         var reader = MakeKey("scim read-only", "readonly");
         Reply refused = AsKey(reader.token, "GET", "/scim/v2/ServiceProviderConfig", null);
         AssertScimError(refused, 403, null);
         StringAssert.Contains("provision", refused.Json.GetProperty("detail").GetString());

         MakeRole(ReaderRole, "view_accounts");
         var viewer = MakeKey("scim viewer", null, ReaderRole);
         AssertScimError(AsKey(viewer.token, "GET", "/scim/v2/Users", null), 403, null);
      }

      // ---------------------------------------------------------------- users

      [Test]
      [Description("POST /Users makes the account with its names, its switch, its password and the identity provider's id: COM reads them back and the account signs in over IMAP with that password. A second POST for the address is 409 uniqueness, an address at a domain this server does not host is 400 invalidValue, an attribute the core schema has and this server does not keep is accepted and not answered, one the schema does not have is 400, and an account made with no password signs in with no password anybody was told.")]
      public void CreatingAUserMakesAnAccountThatSignsIn()
      {
         var key = MakeKey("scim create");
         string address = "ada@" + _domain.Name;

         Reply created = AsKey(key.token, "POST", "/scim/v2/Users",
            UserBody(address, "Ada", "Lovelace", Password, "entra-0001", true,
                     ",\"title\":\"Countess\",\"emails\":[{\"value\":\"" + address + "\",\"type\":\"work\",\"primary\":true}]"));

         ClassicAssert.AreEqual(201, created.Status, created.Body);
         StringAssert.StartsWith("application/scim+json", created.Header("Content-Type"));
         StringAssert.EndsWith("/scim/v2/Users/" + address, created.Header("Location"));
         ClassicAssert.IsNotEmpty(created.Header("ETag"));

         JsonElement user = created.Json;
         ClassicAssert.AreEqual(address, user.GetProperty("id").GetString());
         ClassicAssert.AreEqual(address, user.GetProperty("userName").GetString());
         ClassicAssert.AreEqual("entra-0001", user.GetProperty("externalId").GetString());
         ClassicAssert.AreEqual("Ada", user.GetProperty("name").GetProperty("givenName").GetString());
         ClassicAssert.AreEqual("Ada Lovelace", user.GetProperty("displayName").GetString());
         ClassicAssert.IsTrue(user.GetProperty("active").GetBoolean());
         ClassicAssert.AreEqual("User", user.GetProperty("meta").GetProperty("resourceType").GetString());
         ClassicAssert.AreEqual(created.Header("ETag"), user.GetProperty("meta").GetProperty("version").GetString());
         ClassicAssert.IsFalse(user.TryGetProperty("title", out _), "A title is accepted and not kept.");
         ClassicAssert.IsFalse(user.TryGetProperty("password", out _), "A password is never answered.");

         Account account = _domain.Accounts.get_ItemByAddress(address);
         ClassicAssert.AreEqual("Ada", account.PersonFirstName);
         ClassicAssert.AreEqual("Lovelace", account.PersonLastName);
         ClassicAssert.IsTrue(account.Active);
         ClassicAssert.AreEqual("entra-0001", (string) ((dynamic) account).ExternalId, "COM's Account.ExternalId is the same column.");
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(address, Password), "The account signs in with the password SCIM set.");

         // The controls.
         AssertScimError(AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Ada", "Again", Password, null, true, null)), 409, "uniqueness");
         AssertScimError(AsKey(key.token, "POST", "/scim/v2/Users", UserBody("someone@scim-not-hosted.test", null, null, Password, null, true, null)), 400, "invalidValue");

         Reply misspelt = AsKey(key.token, "POST", "/scim/v2/Users", UserBody("misspelt@" + _domain.Name, null, null, Password, null, true, ",\"favouriteColour\":\"blue\""));
         AssertScimError(misspelt, 400, "invalidValue");
         StringAssert.Contains("favouriteColour", misspelt.Json.GetProperty("detail").GetString());
         ClassicAssert.AreEqual(404, Admin("GET", "/api/v1/accounts/misspelt@" + _domain.Name, null).Status, "A refused create makes nothing.");

         // Entra sends no password: the account is made with one nobody is told.
         string babbage = "babbage@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(babbage, "Charles", "Babbage", null, null, true, null)).Status);
         ClassicAssert.IsNotNull(_domain.Accounts.get_ItemByAddress(babbage));
         ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword(babbage, Password), "No password anybody chose opens it.");
         ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword(babbage, ""), "Nor an empty one.");
      }

      [Test]
      [Description("A key confined to example.test provisions example.test alone: an account at another hosted domain is refused with the very sentence a domain this server does not host gets; the account already there is 404 to its GET, PUT, PATCH and DELETE and missing from its listing, its userName eq and co filters, its paging and an emails.value filter through an alias here; a rename into the other domain and an alias there are 400; a list there is 404 - and the same requests with an unconfined key find and change them.")]
      public void AKeyConfinedToADomainProvisionsThatDomainAlone()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);
         string carol = "carol@" + OtherDomain;
         SingletonProvider<TestSetup>.Instance.AddAccount(other, carol, Password);
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "to-carol@" + _domain.Name, carol);

         var confined = MakeKey("scim confined", "full", null, _domain.Name);
         var everywhere = MakeKey("scim everywhere");

         Reply outside = AsKey(confined.token, "POST", "/scim/v2/Users", UserBody("dave@" + OtherDomain, null, null, Password, null, true, null));
         AssertScimError(outside, 400, "invalidValue");

         Reply notHosted = AsKey(confined.token, "POST", "/scim/v2/Users", UserBody("dave@scim-not-hosted.test", null, null, Password, null, true, null));
         AssertScimError(notHosted, 400, "invalidValue");
         ClassicAssert.AreEqual(notHosted.Json.GetProperty("detail").GetString(), outside.Json.GetProperty("detail").GetString(),
            "A domain the key does not reach and a domain nobody hosts are refused alike, so the refusal names no tenant.");

         AssertScimError(AsKey(confined.token, "GET", "/scim/v2/Users/" + carol, null), 404, null);
         AssertScimError(Send("PUT", "/scim/v2/Users/" + carol, "Bearer " + confined.token, UserBody(carol, "Carol", "Taken", null, null, true, null)), 404, null);
         AssertScimError(Patch(confined.token, "Users/" + carol, "{\"op\":\"replace\",\"path\":\"active\",\"value\":false}"), 404, null);
         AssertScimError(AsKey(confined.token, "DELETE", "/scim/v2/Users/" + carol, null), 404, null);
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(carol, Password), "Nothing the confined key sent reached the other domain's account.");

         ClassicAssert.AreEqual(0, Total(AsKey(confined.token, "GET", "/scim/v2/Users?filter=" + Filter("userName eq \"" + carol + "\""), null)));
         CollectionAssert.DoesNotContain(Ids(AsKey(confined.token, "GET", "/scim/v2/Users", null)), carol);
         CollectionAssert.DoesNotContain(Ids(AsKey(confined.token, "GET", "/scim/v2/Users?filter=" + Filter("userName co \"@\""), null)), carol);
         ClassicAssert.AreEqual(0, Total(AsKey(confined.token, "GET", "/scim/v2/Users?filter=" + Filter("emails.value eq \"to-carol@" + _domain.Name + "\""), null)),
            "An alias the key reaches does not lead it to an account it does not.");

         // In its own domain the same key provisions - and not out of it.
         string dave = "dave@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(confined.token, "POST", "/scim/v2/Users", UserBody(dave, null, null, Password, null, true, null)).Status);
         ClassicAssert.AreEqual(Ids(AsKey(confined.token, "GET", "/scim/v2/Users", null)).Count,
            Total(AsKey(confined.token, "GET", "/scim/v2/Users?filter=" + Filter("userName co \"@\"") + "&startIndex=1&count=1", null)),
            "Paging counts only what the key reaches.");
         AssertScimError(Patch(confined.token, "Users/" + dave, "{\"op\":\"replace\",\"path\":\"userName\",\"value\":\"dave@" + OtherDomain + "\"}"), 400, "invalidValue");
         AssertScimError(Patch(confined.token, "Users/" + dave, "{\"op\":\"add\",\"path\":\"emails\",\"value\":[{\"value\":\"dave-alias@" + OtherDomain + "\",\"type\":\"other\"}]}"), 400, "invalidValue");
         ClassicAssert.IsNotNull(_domain.Accounts.get_ItemByAddress(dave), "The refused rename left the account where it was.");

         string otherList = "carols-list@" + OtherDomain;
         ClassicAssert.AreEqual(201, AsKey(everywhere.token, "POST", "/scim/v2/Groups", "{\"displayName\":\"" + otherList + "\"}").Status);
         AssertScimError(AsKey(confined.token, "GET", "/scim/v2/Groups/" + otherList, null), 404, null);
         AssertScimError(Patch(confined.token, "Groups/" + otherList, "{\"op\":\"add\",\"path\":\"members\",\"value\":[{\"value\":\"" + dave + "\"}]}"), 404, null);

         // The controls: the unconfined key reads the same account every way, and changes it.
         ClassicAssert.AreEqual(200, AsKey(everywhere.token, "GET", "/scim/v2/Users/" + carol, null).Status);
         ClassicAssert.AreEqual(1, Total(AsKey(everywhere.token, "GET", "/scim/v2/Users?filter=" + Filter("userName eq \"" + carol + "\""), null)));
         CollectionAssert.Contains(Ids(AsKey(everywhere.token, "GET", "/scim/v2/Users", null)), carol);
         CollectionAssert.Contains(Ids(AsKey(everywhere.token, "GET", "/scim/v2/Users?filter=" + Filter("userName co \"@\""), null)), carol);
         CollectionAssert.AreEqual(new[] { carol }, Ids(AsKey(everywhere.token, "GET", "/scim/v2/Users?filter=" + Filter("emails.value eq \"to-carol@" + _domain.Name + "\""), null)));
         ClassicAssert.AreEqual(200, AsKey(everywhere.token, "GET", "/scim/v2/Groups/" + otherList, null).Status);
         ClassicAssert.AreEqual(200, Patch(everywhere.token, "Users/" + carol, "{\"op\":\"replace\",\"path\":\"name.familyName\",\"value\":\"Changed\"}").Status);
      }

      [Test]
      [Description("A key may not take over an account whose authority it does not take in. A full key confined to example.test is 403 for a PATCH of the password and for a DELETE of an example.test account that administers another domain, of one that audits every domain, and of the server administrator's; a key whose role holds provision alone is 403 for a domain administrator's. Each password is unchanged. The controls: the confined key changes a domain administrator of its own domain, the provisioning key an ordinary account, and the key issued for every domain takes over each of the three.")]
      public void AKeyCannotTakeOverAnAccountThatReachesFurtherThanItDoes()
      {
         SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);

         var confined = MakeKey("scim takeover confined", "full", null, _domain.Name);
         var everywhere = MakeKey("scim takeover everywhere");
         MakeRole(ProvisionRole, "provision");
         var provisioner = MakeKey("scim takeover provisioner", null, ProvisionRole);

         string wide = "wide-admin@" + _domain.Name;
         string auditor = "all-auditor@" + _domain.Name;
         string server = "server-admin@" + _domain.Name;
         string local = "local-admin@" + _domain.Name;
         string plain = "plain@" + _domain.Name;

         foreach (string address in new[] { wide, auditor, server, local, plain })
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);

         Assign(wide, "domainadmin", OtherDomain);
         Assign(auditor, "auditor", "");
         SetAdminLevel(server, eAdminLevel.hAdminLevelServerAdmin);
         SetAdminLevel(local, eAdminLevel.hAdminLevelDomainAdmin);

         const string taken = "Taken-Over-Passw0rd-2026";
         string passwordPatch = "{\"op\":\"replace\",\"path\":\"password\",\"value\":\"" + taken + "\"}";

         foreach (string target in new[] { wide, auditor, server })
         {
            Reply refused = Patch(confined.token, "Users/" + target, passwordPatch);
            AssertScimError(refused, 403, null);
            StringAssert.Contains("holds authority this api key does not", refused.Json.GetProperty("detail").GetString());
            AssertScimError(AsKey(confined.token, "DELETE", "/scim/v2/Users/" + target, null), 403, null);
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(target, Password), target + "'s password is unchanged, and the account is there.");
         }

         AssertScimError(Patch(provisioner.token, "Users/" + local, passwordPatch), 403, null);
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(local, Password), "A key holding provision alone takes in no administrator.");

         // The controls.
         ClassicAssert.AreEqual(200, Patch(confined.token, "Users/" + local, passwordPatch).Status,
            "A domain administrator of the key's own domain is the confined key's to change.");
         ClassicAssert.AreEqual(200, Patch(provisioner.token, "Users/" + plain, passwordPatch).Status,
            "An ordinary account is the provisioning key's to change.");

         foreach (string target in new[] { wide, auditor, server })
         {
            Reply allowed = Patch(everywhere.token, "Users/" + target, passwordPatch);
            ClassicAssert.AreEqual(200, allowed.Status, allowed.Body);
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(target, taken), "The key issued for every domain takes in " + target + ".");
         }
      }

      [Test]
      [Description("The account routes answer a full key confined to named domains as SCIM does: PUT /api/v1/accounts/{address} with a password and the password reset are 403 for a server administrator whose mailbox is in the key's own domain - by admin level or by the serveradmin role - and the password is unchanged; the key issued for every domain sets both, the control. RoleManager::Encompasses took in every account for any key holding administer_server until it read the key's list.")]
      public void AConfinedFullKeyCannotSetAServerAdministratorsPasswordOnTheAccountRoutes()
      {
         var confined = MakeKey("routes confined", "full", null, _domain.Name);
         var everywhere = MakeKey("routes everywhere");

         string byLevel = "level-admin@" + _domain.Name;
         string byRole = "role-admin@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, byLevel, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, byRole, Password);
         SetAdminLevel(byLevel, eAdminLevel.hAdminLevelServerAdmin);
         Assign(byRole, "serveradmin", "");

         const string taken = "Taken-Over-Passw0rd-2027";
         string body = "{\"password\":\"" + taken + "\"}";

         foreach (string target in new[] { byLevel, byRole })
         {
            Reply put = Send("PUT", "/api/v1/accounts/" + target, "Bearer " + confined.token, body);
            ClassicAssert.AreEqual(403, put.Status, "A key for one domain set a server administrator's password with PUT. " + put.Body);
            Reply reset = Send("POST", "/api/v1/accounts/" + target + "/password", "Bearer " + confined.token, body);
            ClassicAssert.AreEqual(403, reset.Status, "A key for one domain reset a server administrator's password. " + reset.Body);
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(target, Password), target + "'s password is unchanged.");
         }

         // The controls.
         foreach (string target in new[] { byLevel, byRole })
         {
            Reply put = Send("PUT", "/api/v1/accounts/" + target, "Bearer " + everywhere.token, body);
            ClassicAssert.AreEqual(200, put.Status, put.Body);
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(target, taken));

            Reply reset = Send("POST", "/api/v1/accounts/" + target + "/password", "Bearer " + everywhere.token, "{\"password\":\"Reset-Again-Passw0rd-2028\"}");
            ClassicAssert.AreEqual(200, reset.Status, reset.Body);
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(target, "Reset-Again-Passw0rd-2028"));
         }
      }

      [Test]
      [Description("GET /Users filters on userName eq, co and sw, externalId eq (exactly), emails.value eq (the address or an alias of it) and active eq, and pages with startIndex and count over three users in the order they were made; a filter this server does not answer is 400 invalidFilter.")]
      public void FiltersAndPagingAnswerWhatTheyShould()
      {
         var key = MakeKey("scim filters");

         string first = "scimu1@" + _domain.Name;
         string second = "scimu2@" + _domain.Name;
         string third = "scimu3@" + _domain.Name;

         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(first, "One", "User", Password, "ext-1", false, null)).Status);
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(second, "Two", "User", Password, "ext-2", true, null)).Status);
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(third, "Three", "User", Password, "ext-3", true, null)).Status);
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "two@" + _domain.Name, second);

         CollectionAssert.AreEqual(new[] { second }, Ids(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName eq \"" + second + "\""), null)));
         CollectionAssert.AreEqual(new[] { second }, Ids(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName eq \"SCIMU2@" + _domain.Name.ToUpperInvariant() + "\""), null)),
            "userName is compared without regard to case.");
         CollectionAssert.AreEqual(new[] { third }, Ids(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("externalId eq \"ext-3\""), null)));
         ClassicAssert.AreEqual(0, Total(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("externalId eq \"EXT-3\""), null)),
            "externalId is compared exactly.");
         CollectionAssert.AreEqual(new[] { second }, Ids(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("emails.value eq \"two@" + _domain.Name + "\""), null)),
            "An alias finds the account it is an alias of.");
         CollectionAssert.AreEqual(new[] { first }, Ids(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("active eq false"), null)));
         ClassicAssert.AreEqual(3, Total(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName sw \"scimu\""), null)));
         CollectionAssert.AreEqual(new[] { second }, Ids(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName co \"u2@\""), null)));

         Reply page = AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName sw \"scimu\"") + "&startIndex=2&count=1", null);
         ClassicAssert.AreEqual(200, page.Status, page.Body);
         ClassicAssert.AreEqual(3, page.Json.GetProperty("totalResults").GetInt32());
         ClassicAssert.AreEqual(1, page.Json.GetProperty("itemsPerPage").GetInt32());
         ClassicAssert.AreEqual(2, page.Json.GetProperty("startIndex").GetInt32());
         CollectionAssert.AreEqual(new[] { second }, Ids(page), "The second of three, in the order they were made.");

         Reply none = AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName sw \"scimu\"") + "&count=0", null);
         ClassicAssert.AreEqual(3, Total(none));
         ClassicAssert.AreEqual(0, Ids(none).Count, "count=0 answers the total and no resource.");

         // Sorting is not supported, so sortBy and sortOrder are ignored (RFC 7644
         // section 3.4.2.3): the order is still the order the accounts were made.
         CollectionAssert.AreEqual(new[] { first, second, third },
            Ids(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName sw \"scimu\"") + "&sortBy=userName&sortOrder=descending", null)));

         // The controls.
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("name.givenName eq \"Two\""), null), 400, "invalidFilter");
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("title eq \"Countess\""), null), 400, "invalidFilter");
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName eq"), null), 400, "invalidFilter");
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName eq \"" + second + "\" and active eq true"), null), 400, "invalidFilter");
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users?filter=" + Filter("userName sw \"scimu\"") + "&startIndex=two", null), 400, "invalidValue");
      }

      [Test]
      [Description("PATCH active false suspends the account - an IMAP sign-in is refused - and true restores it; PATCH name.givenName changes the name; Entra's spellings - emails[type eq \"work\"].value written as the account's own address, a path-less replace carrying a value object with a dotted key, and a boolean as the string \"False\" - are taken; the work address written as another address is 400 and changes nothing.")]
      public void PatchSuspendsRestoresAndTakesEntrasSpellings()
      {
         var key = MakeKey("scim patch");
         string address = "grace@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Grace", "Hopper", Password, null, true, null)).Status);
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(address, Password), "The control: the account signs in.");

         Reply suspended = Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"active\",\"value\":false}");
         ClassicAssert.AreEqual(200, suspended.Status, suspended.Body);
         ClassicAssert.IsFalse(suspended.Json.GetProperty("active").GetBoolean());
         ClassicAssert.IsFalse(_domain.Accounts.get_ItemByAddress(address).Active);
         ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword(address, Password), "A suspended account does not sign in.");

         ClassicAssert.AreEqual(200, Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"active\",\"value\":true}").Status);
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(address, Password), "Restored, it signs in again.");

         Reply renamed = Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"name.givenName\",\"value\":\"Amazing\"}");
         ClassicAssert.AreEqual(200, renamed.Status, renamed.Body);
         ClassicAssert.AreEqual("Amazing", _domain.Accounts.get_ItemByAddress(address).PersonFirstName);
         ClassicAssert.AreEqual("Hopper", _domain.Accounts.get_ItemByAddress(address).PersonLastName, "The other name is left as it was.");

         // Entra's spellings.
         Reply work = Patch(key.token, "Users/" + address, "{\"op\":\"Replace\",\"path\":\"emails[type eq \\\"work\\\"].value\",\"value\":\"" + address + "\"}");
         ClassicAssert.AreEqual(200, work.Status, work.Body);

         Reply pathless = Patch(key.token, "Users/" + address, "{\"op\":\"Replace\",\"value\":{\"active\":\"False\",\"name.familyName\":\"King\"}}");
         ClassicAssert.AreEqual(200, pathless.Status, pathless.Body);
         Account after = _domain.Accounts.get_ItemByAddress(address);
         ClassicAssert.IsFalse(after.Active, "\"False\" as a string is false.");
         ClassicAssert.AreEqual("King", after.PersonLastName);

         // The control: the work address written as another is the account's
         // address being changed by the back door, and is refused whole.
         Reply otherWork = Patch(key.token, "Users/" + address,
            "{\"op\":\"replace\",\"path\":\"name.familyName\",\"value\":\"Changed\"},{\"op\":\"replace\",\"path\":\"emails[type eq \\\"work\\\"].value\",\"value\":\"someone-else@" + _domain.Name + "\"}");
         AssertScimError(otherWork, 400, "invalidValue");
         ClassicAssert.AreEqual("King", _domain.Accounts.get_ItemByAddress(address).PersonLastName, "A refused PATCH writes none of its operations.");

         AssertScimError(Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"favouriteColour\",\"value\":\"blue\"}"), 400, "invalidPath");
         ClassicAssert.AreEqual(200, Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"urn:ietf:params:scim:schemas:extension:enterprise:2.0:User:department\",\"value\":\"Navy\"}").Status,
            "An extension's attribute is accepted and not kept.");
      }

      [Test]
      [Description("PATCH password, on an account that stays active, drops its live IMAP connection where it stands - a session with INBOX selected gets no answer to its next command - and ends the browser session the old password started, which the inactive flag cannot be what refuses; PATCH active false does the same. Before each PATCH the connection answered NOOP and the session read /api/v1/me.")]
      public void ANewPasswordAndASuspensionEndTheSessionsAndDropTheConnections()
      {
         var key = MakeKey("scim sessions");
         string address = "sessions@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Session", "Holder", Password, null, true, null)).Status);

         ImapClientSimulator imap = OpenImap(address, Password);
         string cookie = StartSession(address, Password);

         const string changed = "Another-Scim-Passw0rd-2026";
         Reply password = Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"password\",\"value\":\"" + changed + "\"}");
         ClassicAssert.AreEqual(200, password.Status, password.Body);
         AssertDropped(imap, "a new password");
         ClassicAssert.AreEqual(401, Send("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie).Status,
            "The browser session the old password started ends with it.");
         ClassicAssert.IsTrue(_domain.Accounts.get_ItemByAddress(address).Active, "The account is active: what ended the session is the new password.");

         imap = OpenImap(address, changed);
         cookie = StartSession(address, changed);

         Reply suspended = Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"active\",\"value\":false}");
         ClassicAssert.AreEqual(200, suspended.Status, suspended.Body);
         AssertDropped(imap, "a suspension");
         ClassicAssert.AreEqual(401, Send("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie).Status);
      }

      [Test]
      [Description("A displayName written with no name beside it is split into the first and last names for an account made without one - the control - and is accepted and not kept for an account that has them: a PATCH of the display name alone, as Entra sends when only that changed in the directory, leaves the names the account has.")]
      public void ADisplayNameAloneSplitsOnlyWhereThereIsNoName()
      {
         var key = MakeKey("scim display name");
         string named = "ada-named@" + _domain.Name;
         string unnamed = "ada-unnamed@" + _domain.Name;

         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(named, "Ada", "Lovelace", Password, null, true, null)).Status);
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(unnamed, null, null, Password, null, true, ",\"displayName\":\"Ada Lovelace\"")).Status);

         Account split = _domain.Accounts.get_ItemByAddress(unnamed);
         ClassicAssert.AreEqual("Ada", split.PersonFirstName, "An account with no name takes one from displayName.");
         ClassicAssert.AreEqual("Lovelace", split.PersonLastName);

         Reply changed = Patch(key.token, "Users/" + named, "{\"op\":\"replace\",\"path\":\"displayName\",\"value\":\"Ada Lovelace (Finance)\"}");
         ClassicAssert.AreEqual(200, changed.Status, changed.Body);

         Account kept = _domain.Accounts.get_ItemByAddress(named);
         ClassicAssert.AreEqual("Ada", kept.PersonFirstName, "A display name alone does not replace the names an account has.");
         ClassicAssert.AreEqual("Lovelace", kept.PersonLastName);
         ClassicAssert.AreEqual("Ada Lovelace", changed.Json.GetProperty("displayName").GetString(), "displayName is composed from the names.");
      }

      [Test]
      [Description("A change is judged whole before anything is written. With the domain at its limit of aliases, a PATCH that changes the family name and adds an alias is 400 and the family name and the aliases are as they were; the same PATCH suspending the account leaves it active and signing in; a PUT renaming the account, whose old address the rename would keep as an alias in the full domain, is 400 and the account is where it was. With the limit raised the same PATCH is taken whole - the control.")]
      public void AChangeIsJudgedWholeBeforeAnythingIsWritten()
      {
         var key = MakeKey("scim whole");
         string address = "whole@" + _domain.Name;
         string nick = "whole-nick@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Whole", "Before", Password, null, true, null)).Status);

         // One alias in the domain, and a limit of one: the domain is full.
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "taken@" + _domain.Name, "postmaster@" + _domain.Name);
         _domain.MaxNumberOfAliases = 1;
         _domain.MaxNumberOfAliasesEnabled = true;
         _domain.Save();

         string patch = "{\"op\":\"replace\",\"path\":\"name.familyName\",\"value\":\"After\"}," +
                        "{\"op\":\"add\",\"path\":\"emails\",\"value\":[{\"value\":\"" + nick + "\",\"type\":\"other\"}]}";

         Reply refused = Patch(key.token, "Users/" + address, patch);
         AssertScimError(refused, 400, "invalidValue");
         StringAssert.Contains("maximum number of aliases", refused.Json.GetProperty("detail").GetString());
         ClassicAssert.AreEqual("Before", _domain.Accounts.get_ItemByAddress(address).PersonLastName, "A refused change writes none of itself.");
         CollectionAssert.AreEqual(new[] { "taken@" + _domain.Name }, AliasNames(), "Nor any alias.");

         Reply suspension = Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"active\",\"value\":false}," + patch);
         AssertScimError(suspension, 400, "invalidValue");
         ClassicAssert.IsTrue(_domain.Accounts.get_ItemByAddress(address).Active, "A refused suspension leaves the account as it was.");
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(address, Password));

         WaitUntilMovable(address);
         string renamedTo = "whole-renamed@" + _domain.Name;
         Reply rename = Send("PUT", "/scim/v2/Users/" + address, "Bearer " + key.token, UserBody(renamedTo, "Whole", "Before", null, null, true, null));
         AssertScimError(rename, 400, "invalidValue");
         StringAssert.Contains("old address", rename.Json.GetProperty("detail").GetString());
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users/" + renamedTo, null), 404, null);
         ClassicAssert.AreEqual(200, AsKey(key.token, "GET", "/scim/v2/Users/" + address, null).Status, "The account is where it was.");

         // The control.
         _domain.MaxNumberOfAliases = 5;
         _domain.Save();

         Reply taken = Patch(key.token, "Users/" + address, patch);
         ClassicAssert.AreEqual(200, taken.Status, taken.Body);
         ClassicAssert.AreEqual("After", _domain.Accounts.get_ItemByAddress(address).PersonLastName);
         ClassicAssert.AreEqual(address, _domain.Aliases.get_ItemByName(nick).Value);
      }

      [Test]
      [Description("PATCH password goes through the password policy: with a minimum length of sixteen a short one is 400 invalidValue with the policy's sentence and the old password still opens the account; a long one is taken, opens it, and the old one no longer does.")]
      public void APatchedPasswordObeysThePolicy()
      {
         var key = MakeKey("scim password");
         string address = "linus@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Linus", null, Password, null, true, null)).Status);

         _policySet = true;
         ServerIniFile.SetSetting("PasswordPolicyMinimumLength", "16");
         _application.Reinitialize();

         Reply weak = Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"password\",\"value\":\"Short-1!\"}");
         AssertScimError(weak, 400, "invalidValue");
         StringAssert.Contains("16", weak.Json.GetProperty("detail").GetString(), "The refusal names the requirement.");
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(address, Password), "The refused password changed nothing.");

         const string strong = "A-Much-Longer-Passw0rd-2026";
         ClassicAssert.AreEqual(200, Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"password\",\"value\":\"" + strong + "\"}").Status);
         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword(address, strong));
         ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword(address, Password), "The old password no longer opens it.");
      }

      [Test]
      [Description("Every representation carries a weak ETag, in the ETag field and in meta.version, that changes when the account does; a PATCH or a PUT whose If-Match names an older one is 412 and changes nothing, and one naming the current one is taken.")]
      public void IfMatchWithAStaleVersionIsRefused()
      {
         var key = MakeKey("scim etag");
         string address = "etag@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Early", "One", Password, null, true, null)).Status);

         Reply read = AsKey(key.token, "GET", "/scim/v2/Users/" + address, null);
         string first = read.Header("ETag");
         StringAssert.StartsWith("W/\"", first);
         ClassicAssert.AreEqual(first, read.Json.GetProperty("meta").GetProperty("version").GetString());

         Reply changed = Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"name.familyName\",\"value\":\"Two\"}", first);
         ClassicAssert.AreEqual(200, changed.Status, changed.Body);
         string second = changed.Header("ETag");
         ClassicAssert.AreNotEqual(first, second, "The version moved with the account.");

         AssertScimError(Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"name.familyName\",\"value\":\"Three\"}", first), 412, null);
         AssertScimError(Send("PUT", "/scim/v2/Users/" + address, "Bearer " + key.token, UserBody(address, "Early", "Four", null, null, true, null), "If-Match: " + first), 412, null);
         AssertScimError(Send("DELETE", "/scim/v2/Users/" + address, "Bearer " + key.token, null, "If-Match: " + first), 412, null);
         ClassicAssert.AreEqual("Two", _domain.Accounts.get_ItemByAddress(address).PersonLastName, "A stale If-Match writes nothing, and removes nothing.");

         ClassicAssert.AreEqual(200, Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"name.familyName\",\"value\":\"Five\"}", second).Status,
            "The control: the current version is taken.");
         ClassicAssert.AreEqual("Five", _domain.Accounts.get_ItemByAddress(address).PersonLastName);
      }

      [Test]
      [Description("A PUT with a new userName renames the account through the mailbox move: the same account row at the new address with its message and its folder, the new id answered, the old address kept as an alias of type other that still takes mail, and the old id 404. A later PUT whose emails leave the old address out removes that alias.")]
      public void APutWithANewUserNameRenamesTheAccount()
      {
         var key = MakeKey("scim rename");
         string from = "rename-from@" + _domain.Name;
         string to = "rename-to@" + _domain.Name;

         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(from, "Mary", "Somerville", Password, "entra-rename", true, null)).Status);
         Account account = _domain.Accounts.get_ItemByAddress(from);
         long accountId = account.ID;

         IMAPFolder folder = account.IMAPFolders.Add("Projects");
         folder.Save();

         SmtpClientSimulator.StaticSend("outside@example.com", from, "Seed", "Delivered before the rename.");
         CustomAsserts.AssertFolderMessageCount(_domain.Accounts.get_ItemByAddress(from).IMAPFolders.get_ItemByName("INBOX"), 1);
         WaitUntilMovable(from);

         Reply put = Send("PUT", "/scim/v2/Users/" + from, "Bearer " + key.token,
            UserBody(to, "Mary", "Somerville", null, "entra-rename", true, ",\"emails\":[{\"value\":\"" + to + "\",\"type\":\"work\",\"primary\":true}]"), null);
         ClassicAssert.AreEqual(200, put.Status, put.Body);
         ClassicAssert.AreEqual(to, put.Json.GetProperty("id").GetString(), "A rename changes the id.");

         var emails = put.Json.GetProperty("emails").EnumerateArray().ToList();
         JsonElement kept = emails.Single(e => e.GetProperty("value").GetString() == from);
         ClassicAssert.AreEqual("other", kept.GetProperty("type").GetString(), "The old address is kept as an alias, and an alias is other.");
         ClassicAssert.IsFalse(kept.GetProperty("primary").GetBoolean());

         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users/" + from, null), 404, null);

         Account moved = _domain.Accounts.get_ItemByAddress(to);
         ClassicAssert.AreEqual(accountId, (long) moved.ID, "The same account row, at the new address.");
         ClassicAssert.IsNotNull(moved.IMAPFolders.get_ItemByName("Projects"), "The folder came with it.");
         CustomAsserts.AssertFolderMessageCount(moved.IMAPFolders.get_ItemByName("INBOX"), 1);
         ClassicAssert.AreEqual(to, _domain.Aliases.get_ItemByName(from).Value);

         SmtpClientSimulator.StaticSend("outside@example.com", from, "After", "Sent to the old address after the rename.");
         CustomAsserts.AssertFolderMessageCount(_domain.Accounts.get_ItemByAddress(to).IMAPFolders.get_ItemByName("INBOX"), 2);
         ImapClientSimulator.AssertMessageCount(to, Password, "INBOX", 2);

         // A later PUT that leaves the old address out of emails removes it.
         Reply dropped = Send("PUT", "/scim/v2/Users/" + to, "Bearer " + key.token,
            UserBody(to, "Mary", "Somerville", null, "entra-rename", true, ",\"emails\":[{\"value\":\"" + to + "\",\"type\":\"work\",\"primary\":true}]"), null);
         ClassicAssert.AreEqual(200, dropped.Status, dropped.Body);
         ClassicAssert.AreEqual(1, dropped.Json.GetProperty("emails").GetArrayLength());
         ClassicAssert.AreEqual(0, _domain.Aliases.Count, "The alias the rename kept is gone once emails leaves it out.");
      }

      [Test]
      [Description("A PATCH of userName - Entra's rename - renames the account as a PUT does: the same account row at the new address, the old address kept as an alias. A rename onto one of the account's own aliases takes that alias's place - the alias gives way to the address, and the address the account left is kept as an alias in its turn - where a rename onto an alias of another account is 409 uniqueness and moves nothing, the control.")]
      public void APatchOfUserNameRenamesAndAnAliasCanBecomeTheAddress()
      {
         var key = MakeKey("scim rename by patch");
         string from = "patch-from@" + _domain.Name;
         string nick = "patch-nick@" + _domain.Name;
         string to = "patch-to@" + _domain.Name;

         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users",
            UserBody(from, "Emmy", "Noether", Password, null, true,
                     ",\"emails\":[{\"value\":\"" + from + "\",\"type\":\"work\",\"primary\":true},{\"value\":\"" + nick + "\",\"type\":\"other\"}]")).Status);
         long accountId = _domain.Accounts.get_ItemByAddress(from).ID;

         Reply patched = Patch(key.token, "Users/" + from, "{\"op\":\"replace\",\"path\":\"userName\",\"value\":\"" + to + "\"}");
         ClassicAssert.AreEqual(200, patched.Status, patched.Body);
         ClassicAssert.AreEqual(to, patched.Json.GetProperty("id").GetString());
         ClassicAssert.AreEqual(accountId, (long) _domain.Accounts.get_ItemByAddress(to).ID, "The same account row, at the new address.");
         ClassicAssert.AreEqual(to, _domain.Aliases.get_ItemByName(from).Value, "The old address is kept as an alias.");
         ClassicAssert.AreEqual(to, _domain.Aliases.get_ItemByName(nick).Value, "The account's alias follows it.");

         // The control: an alias of somebody else's is not the account's to take.
         string someone = "someone@" + _domain.Name;
         string someonesNick = "someones-nick@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, someone, Password);
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, someonesNick, someone);

         WaitUntilMovable(to);
         AssertScimError(Patch(key.token, "Users/" + to, "{\"op\":\"replace\",\"path\":\"userName\",\"value\":\"" + someonesNick + "\"}"), 409, "uniqueness");
         ClassicAssert.AreEqual(accountId, (long) _domain.Accounts.get_ItemByAddress(to).ID, "Nothing moved.");
         ClassicAssert.AreEqual(someone, _domain.Aliases.get_ItemByName(someonesNick).Value);

         // Onto its own alias: the alias gives way.
         Reply onto = Patch(key.token, "Users/" + to, "{\"op\":\"replace\",\"path\":\"userName\",\"value\":\"" + nick + "\"}");
         ClassicAssert.AreEqual(200, onto.Status, onto.Body);
         ClassicAssert.AreEqual(nick, onto.Json.GetProperty("id").GetString());
         ClassicAssert.AreEqual(accountId, (long) _domain.Accounts.get_ItemByAddress(nick).ID);
         CollectionAssert.DoesNotContain(AliasNames(), nick, "The alias became the address.");
         ClassicAssert.AreEqual(nick, _domain.Aliases.get_ItemByName(to).Value, "The address the account left is kept as an alias in its turn.");

         var emails = onto.Json.GetProperty("emails").EnumerateArray().Select(e => e.GetProperty("value").GetString()).ToList();
         CollectionAssert.AreEquivalent(new[] { nick, from, to }, emails);
      }

      [Test]
      [Description("An alias added through emails is made and takes mail for the account; removed by emails[value eq ...], it is gone; the account's own address cannot be removed (400) and an address emails does not hold is noTarget.")]
      public void EmailsAddAndRemoveAliases()
      {
         var key = MakeKey("scim emails");
         string address = "hedy@" + _domain.Name;
         string nick = "nick@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Hedy", "Lamarr", Password, null, true, null)).Status);
         ClassicAssert.AreEqual(0, _domain.Aliases.Count, "The control: no alias before.");

         Reply added = Patch(key.token, "Users/" + address, "{\"op\":\"add\",\"path\":\"emails\",\"value\":[{\"value\":\"" + nick + "\",\"type\":\"other\"}]}");
         ClassicAssert.AreEqual(200, added.Status, added.Body);
         CollectionAssert.Contains(added.Json.GetProperty("emails").EnumerateArray().Select(e => e.GetProperty("value").GetString()).ToList(), nick);
         ClassicAssert.AreEqual(address, _domain.Aliases.get_ItemByName(nick).Value);

         SmtpClientSimulator.StaticSend("outside@example.com", nick, "To the alias", "Delivered through the alias SCIM made.");
         CustomAsserts.AssertFolderMessageCount(_domain.Accounts.get_ItemByAddress(address).IMAPFolders.get_ItemByName("INBOX"), 1);

         AssertScimError(Patch(key.token, "Users/" + address, "{\"op\":\"remove\",\"path\":\"emails[value eq \\\"" + address + "\\\"]\"}"), 400, "invalidValue");
         AssertScimError(Patch(key.token, "Users/" + address, "{\"op\":\"remove\",\"path\":\"emails[value eq \\\"nobody@" + _domain.Name + "\\\"]\"}"), 400, "noTarget");
         ClassicAssert.IsNotNull(_domain.Accounts.get_ItemByAddress(address), "The account is still there.");

         Reply removed = Patch(key.token, "Users/" + address, "{\"op\":\"remove\",\"path\":\"emails[value eq \\\"" + nick + "\\\"]\"}");
         ClassicAssert.AreEqual(200, removed.Status, removed.Body);
         ClassicAssert.AreEqual(1, removed.Json.GetProperty("emails").GetArrayLength());
         ClassicAssert.AreEqual(0, _domain.Aliases.Count, "The alias is gone.");
      }

      [Test]
      [Description("DELETE /Users removes the account, every alias emails answered for it and the address's row in every distribution list, and is 204; the account is then 404 over SCIM and REST and no list names it - and before it, the alias was there, the account answered and both lists had it as a member.")]
      public void DeleteRemovesTheAccountItsAliasesAndItsMemberships()
      {
         var key = MakeKey("scim delete");
         string address = "leaving@" + _domain.Name;
         string alias = "left-behind@" + _domain.Name;
         string team = "leavers-team@" + _domain.Name;
         string everyone = "leavers-everyone@" + _domain.Name;

         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users",
            UserBody(address, null, null, Password, null, true, ",\"emails\":[{\"value\":\"" + address + "\",\"type\":\"work\",\"primary\":true},{\"value\":\"" + alias + "\",\"type\":\"other\"}]")).Status);
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Groups", "{\"displayName\":\"" + team + "\",\"members\":[{\"value\":\"" + address + "\"}]}").Status);
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Groups", "{\"displayName\":\"" + everyone + "\",\"members\":[{\"value\":\"" + address + "\"},{\"value\":\"postmaster@" + _domain.Name + "\"}]}").Status);

         ClassicAssert.AreEqual(1, _domain.Aliases.Count, "The control: the alias was made.");
         ClassicAssert.AreEqual(200, AsKey(key.token, "GET", "/scim/v2/Users/" + address, null).Status);
         CollectionAssert.Contains(Members(AsKey(key.token, "GET", "/scim/v2/Groups/" + team, null)), address, "The control: a member of both lists.");
         CollectionAssert.Contains(Members(AsKey(key.token, "GET", "/scim/v2/Groups/" + everyone, null)), address);

         Reply deleted = AsKey(key.token, "DELETE", "/scim/v2/Users/" + address, null);
         ClassicAssert.AreEqual(204, deleted.Status, deleted.Body);

         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Users/" + address, null), 404, null);
         ClassicAssert.AreEqual(404, Admin("GET", "/api/v1/accounts/" + address, null).Status);
         ClassicAssert.AreEqual(0, _domain.Aliases.Count, "The alias went with the account.");

         CollectionAssert.IsEmpty(Members(AsKey(key.token, "GET", "/scim/v2/Groups/" + team, null)), "The address left the list with the account.");
         CollectionAssert.AreEqual(new[] { "postmaster@" + _domain.Name }, Members(AsKey(key.token, "GET", "/scim/v2/Groups/" + everyone, null)),
            "It left the other list too, and the other member stayed.");
         ClassicAssert.AreEqual(0, _domain.DistributionLists.get_ItemByAddress(team).Recipients.Count, "No row names it, member or not.");

         AssertScimError(AsKey(key.token, "DELETE", "/scim/v2/Users/" + address, null), 404, null);
      }

      // ---------------------------------------------------------------- groups

      [Test]
      [Description("POST /Groups makes a distribution list with its members, PATCH adds and removes a member, GET filters on displayName eq, and DELETE removes the list; a member outside a confined key's domain is 400 while the unconfined key may name one, and a group filter this server does not answer is 400 invalidFilter.")]
      public void GroupsAreDistributionLists()
      {
         var key = MakeKey("scim groups");
         string team = "team@" + _domain.Name;
         string m1 = "member1@" + _domain.Name;
         string m2 = "member2@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, m1, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, m2, Password);

         Reply created = AsKey(key.token, "POST", "/scim/v2/Groups",
            "{\"schemas\":[\"" + GroupSchema + "\"],\"displayName\":\"" + team + "\",\"externalId\":\"entra-group-1\",\"members\":[{\"value\":\"" + m1 + "\"}]}");
         ClassicAssert.AreEqual(201, created.Status, created.Body);
         ClassicAssert.AreEqual(team, created.Json.GetProperty("id").GetString());
         CollectionAssert.AreEqual(new[] { m1 }, Members(created));
         ClassicAssert.IsFalse(created.Json.TryGetProperty("externalId", out _), "A group's externalId is accepted and not kept, so not answered.");
         ClassicAssert.AreEqual(1, _domain.DistributionLists.get_ItemByAddress(team).Recipients.Count);

         Reply added = Patch(key.token, "Groups/" + team, "{\"op\":\"add\",\"path\":\"members\",\"value\":[{\"value\":\"" + m2 + "\"}]}");
         ClassicAssert.AreEqual(200, added.Status, added.Body);
         CollectionAssert.AreEquivalent(new[] { m1, m2 }, Members(added));

         Reply removed = Patch(key.token, "Groups/" + team, "{\"op\":\"remove\",\"path\":\"members[value eq \\\"" + m1 + "\\\"]\"}");
         ClassicAssert.AreEqual(200, removed.Status, removed.Body);
         CollectionAssert.AreEqual(new[] { m2 }, Members(removed));
         ClassicAssert.AreEqual(1, _domain.DistributionLists.get_ItemByAddress(team).Recipients.Count, "The member's row was deleted.");

         SmtpClientSimulator.StaticSend("outside@example.com", team, "To the list", "Delivered to the members SCIM left.");
         CustomAsserts.AssertFolderMessageCount(_domain.Accounts.get_ItemByAddress(m2).IMAPFolders.get_ItemByName("INBOX"), 1);

         // A row waiting for its own confirmation is no member, and an add
         // confirms it rather than making a second row beside it.
         string waiting = "waiting@" + _domain.Name;
         DistributionListRecipient row = _domain.DistributionLists.get_ItemByAddress(team).Recipients.Add();
         row.RecipientAddress = waiting;
         row.Save();
         _application.Database.ExecuteSQL("update hm_distributionlistsrecipients set distributionlistrecipientconfirmed = 0 " +
                                          "where distributionlistrecipientaddress = '" + waiting + "'");
         int rows = _domain.DistributionLists.get_ItemByAddress(team).Recipients.Count;
         CollectionAssert.DoesNotContain(Members(AsKey(key.token, "GET", "/scim/v2/Groups/" + team, null)), waiting, "The control: a waiting row is no member.");

         Reply confirmed = Patch(key.token, "Groups/" + team, "{\"op\":\"add\",\"path\":\"members\",\"value\":[{\"value\":\"" + waiting + "\"}]}");
         ClassicAssert.AreEqual(200, confirmed.Status, confirmed.Body);
         CollectionAssert.AreEquivalent(new[] { m2, waiting }, Members(confirmed));
         ClassicAssert.AreEqual(rows, _domain.DistributionLists.get_ItemByAddress(team).Recipients.Count, "The waiting row was confirmed, not doubled.");

         ClassicAssert.AreEqual(1, Total(AsKey(key.token, "GET", "/scim/v2/Groups?filter=" + Filter("displayName eq \"" + team + "\""), null)));
         ClassicAssert.AreEqual(0, Total(AsKey(key.token, "GET", "/scim/v2/Groups?filter=" + Filter("displayName eq \"nobody@" + _domain.Name + "\""), null)));
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Groups?filter=" + Filter("externalId eq \"entra-group-1\""), null), 400, "invalidFilter");

         // A confined key names a member outside its domain: refused, and the
         // unconfined key's same body is the control.
         SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);
         var confined = MakeKey("scim groups confined", "full", null, _domain.Name);
         string outsider = "outsider@" + OtherDomain;
         string confinedBody = "{\"displayName\":\"confined@" + _domain.Name + "\",\"members\":[{\"value\":\"" + outsider + "\"}]}";
         Reply outsideMember = AsKey(confined.token, "POST", "/scim/v2/Groups", confinedBody);
         AssertScimError(outsideMember, 400, "invalidValue");
         StringAssert.Contains(outsider, outsideMember.Json.GetProperty("detail").GetString(), "The refusal names the member the key sent.");
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Groups", confinedBody).Status,
            "The control: the same body from the unconfined key.");

         ClassicAssert.AreEqual(204, AsKey(key.token, "DELETE", "/scim/v2/Groups/" + team, null).Status);
         AssertScimError(AsKey(key.token, "GET", "/scim/v2/Groups/" + team, null), 404, null);
         ClassicAssert.AreEqual(0, Total(AsKey(key.token, "GET", "/scim/v2/Groups?filter=" + Filter("displayName eq \"" + team + "\""), null)));
      }

      [Test]
      [Description("A key confined to example.test changes a list an administrator gave a member elsewhere by the members it names alone: its PATCH add and its PUT of the members it knows are taken and leave the member elsewhere where it is, and a remove naming that member is 400, the refusal naming it; the unconfined key removes it, the control. A member that is the list's own address, or another distribution list's, is 400 to any key, and an ordinary address is taken, the control.")]
      public void AListChangesByTheMembersAKeyNames()
      {
         SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);
         var confined = MakeKey("scim list confined", "full", null, _domain.Name);
         var everywhere = MakeKey("scim list everywhere");

         string list = "mixed@" + _domain.Name;
         string own1 = "own1@" + _domain.Name;
         string own2 = "own2@" + _domain.Name;
         string partner = "partner@" + OtherDomain;

         // The administrator's list: a member here and one elsewhere.
         ClassicAssert.AreEqual(201, AsKey(everywhere.token, "POST", "/scim/v2/Groups",
            "{\"displayName\":\"" + list + "\",\"members\":[{\"value\":\"" + own1 + "\"},{\"value\":\"" + partner + "\"}]}").Status);

         Reply added = Patch(confined.token, "Groups/" + list, "{\"op\":\"add\",\"path\":\"members\",\"value\":[{\"value\":\"" + own2 + "\"}]}");
         ClassicAssert.AreEqual(200, added.Status, added.Body);
         CollectionAssert.AreEquivalent(new[] { own1, own2, partner }, Members(added));

         Reply put = Send("PUT", "/scim/v2/Groups/" + list, "Bearer " + confined.token,
            "{\"displayName\":\"" + list + "\",\"members\":[{\"value\":\"" + own2 + "\"}]}");
         ClassicAssert.AreEqual(200, put.Status, put.Body);
         CollectionAssert.AreEquivalent(new[] { own2, partner }, Members(put), "The member elsewhere is not the confined key's to remove by leaving it out.");

         Reply named = Patch(confined.token, "Groups/" + list, "{\"op\":\"remove\",\"path\":\"members[value eq \\\"" + partner + "\\\"]\"}");
         AssertScimError(named, 400, "invalidValue");
         StringAssert.Contains(partner, named.Json.GetProperty("detail").GetString());
         CollectionAssert.AreEquivalent(new[] { own2, partner }, Members(AsKey(confined.token, "GET", "/scim/v2/Groups/" + list, null)));

         Reply removed = Patch(everywhere.token, "Groups/" + list, "{\"op\":\"remove\",\"path\":\"members[value eq \\\"" + partner + "\\\"]\"}");
         ClassicAssert.AreEqual(200, removed.Status, removed.Body);
         CollectionAssert.AreEqual(new[] { own2 }, Members(removed));

         // A list inside itself, or inside another list.
         string inner = "inner@" + _domain.Name;
         ClassicAssert.AreEqual(201, AsKey(everywhere.token, "POST", "/scim/v2/Groups", "{\"displayName\":\"" + inner + "\"}").Status);

         AssertScimError(Patch(everywhere.token, "Groups/" + list, "{\"op\":\"add\",\"path\":\"members\",\"value\":[{\"value\":\"" + list + "\"}]}"), 400, "invalidValue");
         AssertScimError(Patch(everywhere.token, "Groups/" + list, "{\"op\":\"add\",\"path\":\"members\",\"value\":[{\"value\":\"" + inner + "\"}]}"), 400, "invalidValue");
         AssertScimError(AsKey(everywhere.token, "POST", "/scim/v2/Groups",
            "{\"displayName\":\"outer@" + _domain.Name + "\",\"members\":[{\"value\":\"" + inner + "\"}]}"), 400, "invalidValue");
         CollectionAssert.AreEqual(new[] { own2 }, Members(AsKey(everywhere.token, "GET", "/scim/v2/Groups/" + list, null)));

         ClassicAssert.AreEqual(200, Patch(everywhere.token, "Groups/" + list, "{\"op\":\"add\",\"path\":\"members\",\"value\":[{\"value\":\"" + own1 + "\"}]}").Status,
            "The control: an ordinary address is taken.");
      }

      // ---------------------------------------------------------------- audit and schema

      [Test]
      [Description("Every SCIM write reaches the audit trail under the key's actor, key:<id>, as an API key: the account made, changed and removed, and the distribution list made. A change the administrator password makes is not under the key.")]
      public void EveryWriteIsOnTheAuditTrailUnderTheKey()
      {
         var key = MakeKey("scim audit");
         string address = "audited@" + _domain.Name;
         string list = "audited-list@" + _domain.Name;

         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Users", UserBody(address, "Audited", null, Password, null, true, null)).Status);
         ClassicAssert.AreEqual(200, Patch(key.token, "Users/" + address, "{\"op\":\"replace\",\"path\":\"name.familyName\",\"value\":\"Person\"}").Status);

         // A delete names its row by its id, as every deleted row on the trail is
         // named (docs/AlertsAndAuditTrail.md).
         long accountId = _domain.Accounts.get_ItemByAddress(address).ID;
         ClassicAssert.AreEqual(201, AsKey(key.token, "POST", "/scim/v2/Groups", "{\"displayName\":\"" + list + "\"}").Status);
         ClassicAssert.AreEqual(204, AsKey(key.token, "DELETE", "/scim/v2/Users/" + address, null).Status);

         // The control: made with the administrator password.
         string adminAlias = "admin-made@" + _domain.Name;
         ClassicAssert.AreEqual(201, Admin("POST", "/api/v1/domains/" + _domain.Name + "/aliases", "{\"name\":\"" + adminAlias + "\",\"value\":\"postmaster@" + _domain.Name + "\"}").Status);

         Reply trail = Admin("GET", "/api/v1/audit?limit=200&actor=" + Uri.EscapeDataString("key:" + key.id), null);
         ClassicAssert.AreEqual(200, trail.Status, trail.Body);

         var entries = trail.Json.GetProperty("entries").EnumerateArray().ToList();
         foreach (JsonElement entry in entries)
         {
            ClassicAssert.AreEqual("key:" + key.id, entry.GetProperty("actor").GetString());
            ClassicAssert.AreEqual("apikey", entry.GetProperty("actor_type").GetString());
         }

         Func<string, string, string, bool> recorded = (type, name, action) => entries.Any(e =>
            e.GetProperty("object_type").GetString() == type &&
            string.Equals(e.GetProperty("object").GetString(), name, StringComparison.OrdinalIgnoreCase) &&
            e.GetProperty("action").GetString() == action);

         ClassicAssert.IsTrue(recorded("account", address, "created"), "The account made. Trail: " + trail.Body);
         ClassicAssert.IsTrue(recorded("account", address, "updated"), "The account changed. Trail: " + trail.Body);
         ClassicAssert.IsTrue(recorded("account", "accountid " + accountId, "deleted"), "The account removed. Trail: " + trail.Body);
         ClassicAssert.IsTrue(recorded("distribution list", list, "created"), "The list made. Trail: " + trail.Body);
         ClassicAssert.IsFalse(entries.Any(e => string.Equals(e.GetProperty("object").GetString(), adminAlias, StringComparison.OrdinalIgnoreCase)),
            "The administrator password's own change is not the key's.");
      }

      [Test]
      [Description("DBUpdater's probe for schema 6096 - the column the externalId filter reads - succeeds on the bench's database, and the same probe naming a column that is not there fails, its error-log line asserted and cleared.")]
      public void TheSchemaProbeForTheExternalIdSucceeds()
      {
         Database database = _application.Database;
         SchemaProbe probe = SchemaVerification.GetProbesFor(6096).First(p => p.Describes == "hm_accounts.accountexternalid");

         database.ExecuteSQL(probe.Statement);

         string absent = probe.Statement.Replace("accountexternalid", "accountnosuchcolumn");
         ClassicAssert.AreNotEqual(probe.Statement, absent);

         try
         {
            var refused = Assert.Throws<COMException>(() => database.ExecuteSQL(absent), "The probe must fail for a column that is not there.");

            // The backend evaluating the probe and refusing the column - HM10044, the
            // connection reporting the provider's own error - and not the provider
            // choking on the statement's shape, which is HM10045.
            StringAssert.Contains("HM10044", refused.Message, "The probe failed for a reason other than the backend evaluating it.");
            StringAssert.DoesNotContain("HM10045", refused.Message);
         }
         finally
         {
            // The refusal is written to the error log too; that line is this test's.
            CustomAsserts.AssertReportedError("accountnosuchcolumn");
         }
      }

      // ---------------------------------------------------------------- helpers

      private sealed class Reply
      {
         public int Status;
         public string Headers = "";
         public string Body = "";

         public string Header(string name)
         {
            foreach (string line in Headers.Split(new[] { "\r\n" }, StringSplitOptions.None))
            {
               int colon = line.IndexOf(':');
               if (colon > 0 && string.Equals(line.Substring(0, colon).Trim(), name, StringComparison.OrdinalIgnoreCase))
                  return line.Substring(colon + 1).Trim();
            }

            return "";
         }

         public JsonElement Json
         {
            get
            {
               using (JsonDocument document = JsonDocument.Parse(Body))
                  return document.RootElement.Clone();
            }
         }
      }

      private (string id, string token) MakeKey(string label, string scope = "full", string role = null, string domains = null)
      {
         var body = new StringBuilder("{\"label\":\"" + label + "\"");
         if (scope != null)
            body.Append(",\"scope\":\"" + scope + "\"");
         if (role != null)
            body.Append(",\"role\":\"" + role + "\"");
         if (domains != null)
            body.Append(",\"domains\":\"" + domains + "\"");
         body.Append("}");

         Reply created = Admin("POST", "/api/v1/apikeys", body.ToString());
         ClassicAssert.AreEqual(201, created.Status, created.Body);

         string id = created.Json.GetProperty("id").GetString();
         string token = created.Json.GetProperty("key").GetString();
         _keys.Add(id);

         return (id, token);
      }

      private void MakeRole(string name, string permissions)
      {
         Reply role = Admin("POST", "/api/v1/roles",
            "{\"name\":\"" + name + "\",\"description\":\"scim fixture\",\"permissions\":\"" + permissions + "\"}");
         ClassicAssert.AreEqual(201, role.Status, role.Body);
      }

      // A role given with the administrator password. The assignment goes with the
      // account, which the next test's setup removes.
      private static void Assign(string address, string role, string domains)
      {
         Reply assigned = Admin("PUT", "/api/v1/role-assignments/" + address,
            "{\"role\":\"" + role + "\",\"domains\":\"" + domains + "\"}");
         ClassicAssert.AreEqual(200, assigned.Status, "The administrator could not give " + address + " the role " + role + ". " + assigned.Body);
      }

      private void SetAdminLevel(string address, eAdminLevel level)
      {
         Account account = _domain.Accounts.get_ItemByAddress(address);
         account.AdminLevel = level;
         account.Save();
      }

      private List<string> AliasNames()
      {
         var names = new List<string>();
         Aliases aliases = _domain.Aliases;
         for (int index = 0; index < aliases.Count; index++)
            names.Add(aliases[index].Name);

         names.Sort(StringComparer.OrdinalIgnoreCase);
         return names;
      }

      // An IMAP session with INBOX selected, and answering: the connection a
      // suspension or a new password must drop.
      private static ImapClientSimulator OpenImap(string address, string password)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, password), "The control: the account signs in over IMAP.");
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"), "The control: INBOX is selected.");
         StringAssert.Contains("A01 OK", imap.SendSingleCommand("A01 NOOP"), "The control: the connection answers.");
         return imap;
      }

      // A browser session started with the account's password, and reading
      // /api/v1/me: the session a suspension or a new password must end.
      private static string StartSession(string address, string password)
      {
         Reply started = Send("POST", "/api/v1/session", Basic(address, password), null);
         ClassicAssert.AreEqual(201, started.Status, started.Body);

         string setCookie = started.Header("Set-Cookie");
         int begin = setCookie.IndexOf('=') + 1;
         int end = setCookie.IndexOf(';', begin);
         string cookie = end > begin ? setCookie.Substring(begin, end - begin) : setCookie.Substring(begin);

         ClassicAssert.AreEqual(200, Send("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie).Status,
            "The control: the session reads /api/v1/me.");
         return cookie;
      }

      // The connection dropped where it stood: the next command gets no OK, because
      // nobody is at the other end to give one. Send rather than SendSingleCommand,
      // which would wait for a tagged answer and fail from inside its own timeout
      // (RestApiDevices does the same).
      private static void AssertDropped(ImapClientSimulator imap, string cause)
      {
         string afterwards;
         try
         {
            afterwards = imap.Send("A02 NOOP");
         }
         catch (Exception closed) when (!ExceptionPolicy.IsFatal(closed))
         {
            afterwards = "(closed: " + closed.GetType().Name + ")";
         }

         ClassicAssert.IsFalse(afterwards.Contains("A02 OK"), "After " + cause + " the IMAP connection must have been dropped, but NOOP answered: " + afterwards);

         try
         {
            imap.Disconnect();
         }
         catch (Exception gone) when (!ExceptionPolicy.IsFatal(gone))
         {
         }
      }

      // One step of the teardown: what it throws is written down and the next
      // step runs.
      private static void Tidy(Action step)
      {
         try
         {
            step();
         }
         catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
         {
            Console.WriteLine("SCIM teardown: " + tidying.Message);
         }
      }

      private static string UserBody(string userName, string given, string family, string password, string externalId, bool active, string extra)
      {
         var body = new StringBuilder("{\"schemas\":[\"" + UserSchema + "\"],\"userName\":\"" + userName + "\"");

         if (given != null || family != null)
         {
            body.Append(",\"name\":{");
            if (given != null)
               body.Append("\"givenName\":\"" + given + "\"");
            if (given != null && family != null)
               body.Append(",");
            if (family != null)
               body.Append("\"familyName\":\"" + family + "\"");
            body.Append("}");
         }

         if (password != null)
            body.Append(",\"password\":\"" + password + "\"");
         if (externalId != null)
            body.Append(",\"externalId\":\"" + externalId + "\"");

         body.Append(",\"active\":" + (active ? "true" : "false"));

         if (extra != null)
            body.Append(extra);

         body.Append("}");
         return body.ToString();
      }

      // A PatchOp of the operations written, each a JSON object; ifMatch, when
      // given, is sent as If-Match.
      private static Reply Patch(string token, string resource, string operations, string ifMatch = null)
      {
         string body = "{\"schemas\":[\"" + PatchSchema + "\"],\"Operations\":[" + operations + "]}";
         return Send("PATCH", "/scim/v2/" + resource, "Bearer " + token, body, ifMatch == null ? null : "If-Match: " + ifMatch);
      }

      private static string Filter(string filter)
      {
         return Uri.EscapeDataString(filter);
      }

      private static int Total(Reply reply)
      {
         ClassicAssert.AreEqual(200, reply.Status, reply.Body);
         return reply.Json.GetProperty("totalResults").GetInt32();
      }

      private static List<string> Ids(Reply reply)
      {
         ClassicAssert.AreEqual(200, reply.Status, reply.Body);
         return reply.Json.GetProperty("Resources").EnumerateArray().Select(r => r.GetProperty("id").GetString()).ToList();
      }

      private static List<string> Members(Reply reply)
      {
         return reply.Json.GetProperty("members").EnumerateArray().Select(m => m.GetProperty("value").GetString()).ToList();
      }

      private static void AssertScimError(Reply reply, int status, string scimType)
      {
         ClassicAssert.AreEqual(status, reply.Status, reply.Body);
         StringAssert.StartsWith("application/scim+json", reply.Header("Content-Type"), "Every answer under /scim/ is SCIM's. Body: " + reply.Body);

         JsonElement root = reply.Json;
         ClassicAssert.AreEqual(ErrorSchema, root.GetProperty("schemas")[0].GetString(), reply.Body);
         ClassicAssert.AreEqual(status.ToString(), root.GetProperty("status").GetString(), reply.Body);

         if (scimType != null)
            ClassicAssert.AreEqual(scimType, root.GetProperty("scimType").GetString(), reply.Body);
      }

      // The POP3 lock is released after the client has read the reply to QUIT, and
      // an IMAP session holds a mailbox while a folder is selected; the move route's
      // GET says when neither holds this one.
      private static void WaitUntilMovable(string address)
      {
         DateTime until = DateTime.Now.AddSeconds(30);
         string body = "";

         while (DateTime.Now < until)
         {
            Reply state = Admin("GET", "/api/v1/accounts/" + address + "/move", null);
            body = state.Body;

            if (state.Status == 200 && state.Body.Contains("\"in_use\":false"))
               return;

            Thread.Sleep(200);
         }

         Assert.Fail("The mailbox was still in use after 30 seconds: " + body);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static Reply Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static Reply AsKey(string token, string method, string path, string requestBody)
      {
         return Send(method, path, "Bearer " + token, requestBody);
      }

      // One request, with the response's header block kept, since SCIM's content
      // type, ETag, Location and challenge are what several tests are about.
      private static Reply Send(string method, string path, string authorization, string requestBody, string extraHeader = null)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1:" + RestPort + "\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (!string.IsNullOrEmpty(extraHeader))
                  head.Append(extraHeader + "\r\n");
               if (requestBody != null)
               {
                  head.Append("Content-Type: application/scim+json\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);

               var reply = new Reply
               {
                  Headers = separator >= 0 ? raw.Substring(0, separator) : raw,
                  Body = separator >= 0 ? raw.Substring(separator + 4) : ""
               };

               string[] statusLine = reply.Headers.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               if (statusLine.Length >= 2)
                  int.TryParse(statusLine[1], out reply.Status);

               return reply;
            }
         }
      }
   }
}
