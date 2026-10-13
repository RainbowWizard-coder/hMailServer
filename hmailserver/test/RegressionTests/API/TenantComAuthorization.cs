// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The COM half of the tenant-authorisation fixes: a domain administrator,
   ///    over the COM API the Control Panel uses, is held to the same decisions the
   ///    REST API holds a confined key to (RestApiTenantAuthorization).
   ///
   ///    A domain administrator keeps every ordinary change on its own domain -
   ///    that is the control each test carries - and is refused the ones that
   ///    reach beyond it: renaming the domain, creating a domain alias, setting an
   ///    internal relay host, and putting bind_to_address, send_using_route or
   ///    script_function on one of its rules. The server administrator does all of
   ///    them, which is what makes each refusal about the credential - and a
   ///    domain whose internal smart host the server administrator set stays its
   ///    domain administrator's to save, since only a change of the host is judged.
   /// </summary>
   [TestFixture]
   public class TenantComAuthorization : TestFixtureBase
   {
      private const string AccountPassword = "ComTenant-secret-1234";
      private const string AccessDenied = "You do not have access to this property / method.";

      private Application AsDomainAdmin()
      {
         Account admin = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dadmin@example.test", AccountPassword);
         admin.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         admin.Save();

         var app = new Application();
         Assert.IsNotNull(app.Authenticate("dadmin@example.test", AccountPassword), "The domain administrator could not sign in over COM.");
         return app;
      }

      [Test]
      [Description("A domain administrator may change its domain's settings but not rename it; the server administrator renames it")]
      public void ADomainAdministratorCannotRenameItsDomainOverCom()
      {
         Application app = AsDomainAdmin();
         Domain domain = app.Domains.get_ItemByName("example.test");

         // An ordinary change saves, as it always has.
         domain.Postmaster = "postmaster@example.test";
         domain.Save();

         // The name does not.
         var ex = Assert.Throws<COMException>(() => { domain.Name = "com-renamed.test"; },
            "A domain administrator renamed its domain over COM.");
         StringAssert.Contains(AccessDenied, ex.Message);

         // The server administrator may, over a throwaway domain so nothing else moves.
         Domain throwaway = SingletonProvider<TestSetup>.Instance.AddDomain("com-rename-source.test");
         throwaway.Name = "com-rename-target.test";
         throwaway.Save();
      }

      [Test]
      [Description("A domain administrator may list and remove its domain's aliases but not create one; the server administrator creates it")]
      public void ADomainAdministratorCannotCreateADomainAliasOverCom()
      {
         // The server administrator makes one, which the domain administrator may
         // then see and remove.
         Domain asAdmin = _application.Domains.get_ItemByName("example.test");
         DomainAlias existing = asAdmin.DomainAliases.Add();
         existing.AliasName = "com-existing-alias.test";
         existing.Save();

         Application app = AsDomainAdmin();
         Domain domain = app.Domains.get_ItemByName("example.test");

         // Listing and removing its own domain's aliases: allowed.
         Assert.GreaterOrEqual(domain.DomainAliases.Count, 1, "A domain administrator lists its domain's aliases.");
         domain.DomainAliases.DeleteByDBID(existing.ID);

         // Creating one: refused at Save.
         DomainAlias alias = domain.DomainAliases.Add();
         alias.AliasName = "com-made-by-dadmin.test";
         var ex = Assert.Throws<COMException>(() => alias.Save(), "A domain administrator created a domain alias over COM.");
         StringAssert.Contains(AccessDenied, ex.Message);

         // The server administrator creates one.
         DomainAlias byAdmin = asAdmin.DomainAliases.Add();
         byAdmin.AliasName = "com-made-by-admin.test";
         byAdmin.Save();
      }

      [Test]
      [Description("A domain administrator may set a public relay host and is refused an internal one; the server administrator may set an internal smart host")]
      public void ADomainAdministratorsRelayHostIsHeldToPublicAddressesOverCom()
      {
         Application app = AsDomainAdmin();
         Domain domain = app.Domains.get_ItemByName("example.test");

         // A public relay host saves (93.184.216.34 is a public literal).
         domain.RelayHost = "93.184.216.34";
         domain.Save();

         // An internal one does not.
         domain.RelayHost = "10.73.0.5";
         var ex = Assert.Throws<COMException>(() => domain.Save(), "A domain administrator set an internal relay host over COM.");
         StringAssert.Contains("public address", ex.Message);

         // The server administrator may set an internal smart host.
         Domain asAdmin = _application.Domains.get_ItemByName("example.test");
         asAdmin.RelayHost = "10.73.0.5";
         asAdmin.Save();
      }

      [Test]
      [Description("A domain administrator still saves its domain after the server administrator set an internal smart host on it, writing that host back unchanged as the Control Panel does; a change to another internal host is refused and clearing it is not")]
      public void ADomainAdministratorSavesItsDomainWhoseInternalRelayHostTheServerAdministratorSetOverCom()
      {
         // The server administrator sets an internal smart host for the tenant.
         Domain asAdmin = _application.Domains.get_ItemByName("example.test");
         asAdmin.RelayHost = "10.73.0.5";
         asAdmin.Save();

         Application app = AsDomainAdmin();
         Domain domain = app.Domains.get_ItemByName("example.test");

         // Any other change saves: the relay host the domain has is not the domain
         // administrator's change, and is not judged.
         domain.Postmaster = "postmaster@example.test";
         domain.Save();

         // Writing the loaded host back with another change, as the Control Panel's
         // domain dialog writes back every field it loaded, saves too.
         string loaded = domain.RelayHost;
         Assert.AreEqual("10.73.0.5", loaded, "The domain administrator does not read the smart host it is to write back.");
         domain.RelayHost = loaded;
         domain.Postmaster = "admin@example.test";
         domain.Save();

         Domain stored = _application.Domains.get_ItemByName("example.test");
         Assert.AreEqual("10.73.0.5", stored.RelayHost, "The domain administrator's saves changed the server administrator's smart host.");
         Assert.AreEqual("admin@example.test", stored.Postmaster, "The domain administrator's change was not saved.");

         // A change to another internal host is the domain administrator's own, and
         // is refused; nothing is saved.
         domain.RelayHost = "10.73.0.6";
         var ex = Assert.Throws<COMException>(() => domain.Save(), "A domain administrator changed the internal relay host to another internal one over COM.");
         StringAssert.Contains("public address", ex.Message);
         Assert.AreEqual("10.73.0.5", _application.Domains.get_ItemByName("example.test").RelayHost, "The refused save changed the relay host.");

         // Clearing it is never refused.
         domain.RelayHost = "";
         domain.Save();
         Assert.AreEqual("", _application.Domains.get_ItemByName("example.test").RelayHost, "The domain administrator could not clear the relay host.");
      }

      [Test]
      [Description("A domain administrator may put ordinary actions on its domain's rules and is refused bind_to_address; the server administrator sets it")]
      public void ADomainAdministratorCannotSetServerActionsOnItsRulesOverCom()
      {
         Application app = AsDomainAdmin();
         Domain domain = app.Domains.get_ItemByName("example.test");

         // An ordinary action on a domain rule: allowed.
         Rule ordinary = domain.Rules.Add();
         ordinary.Name = "com-ordinary-rule";
         ordinary.Save();
         RuleAction del = ordinary.Actions.Add();
         del.Type = eRuleActionType.eRADeleteEmail;
         del.Save();

         // bind_to_address on a domain rule: refused.
         Rule rule = domain.Rules.Add();
         rule.Name = "com-server-action-rule";
         rule.Save();
         RuleAction action = rule.Actions.Add();
         action.Type = eRuleActionType.eRABindToAddress;
         action.Value = "93.184.216.34";
         var ex = Assert.Throws<COMException>(() => action.Save(),
            "A domain administrator put bind_to_address on its domain's rule over COM.");
         StringAssert.Contains("server administrator", ex.Message);

         // The server administrator sets a server action on a global rule, as before.
         Rule global = _application.Rules.Add();
         global.Name = "com-global-server-action";
         global.Save();
         RuleAction bind = global.Actions.Add();
         bind.Type = eRuleActionType.eRABindToAddress;
         bind.Value = "93.184.216.34";
         bind.Save();
      }
   }
}
