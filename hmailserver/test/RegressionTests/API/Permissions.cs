// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Runtime.InteropServices;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   public class Permissions : TestFixtureBase
   {
      [Test]
      [Description("Issue 303, Domain administrators should not have permission to add new domains.")]
      public void DomainAdminShouldNotBeAbleToAddDomain()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();

         // Create an account with normal privileges.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");
         account.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         account.Save();

         var newApp = new Application();
         var authenticated = newApp.Authenticate(account.Address, "test");
         Assert.IsNotNull(authenticated);

         // This should throw an exception.
         var ex = Assert.Throws<COMException>(() => newApp.Domains.Add());
         StringAssert.Contains("You do not have access to this property / method.", ex.Message);
      }

      [Test]
      [Description("Issue 303, Normal users should not have permission to add new domains.")]
      public void NormalUserShouldNotBeAbleToAddDomain()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();

         // Create an account with normal privileges.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");

         var newApp = new Application();
         var authenticated = newApp.Authenticate(account.Address, "test");
         Assert.IsNotNull(authenticated);

         // This should throw an exception.
         var ex = Assert.Throws<COMException>(() => newApp.Domains.Add());
         StringAssert.Contains("You do not have access to this property / method.", ex.Message);
      }

      [Test]
      [Description("Issue 303, Server admin should be allowed to add domain.")]
      public void ServerAdminShouldBeAbleToAddDomain()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();

         // Create an account with normal privileges.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");
         account.AdminLevel = eAdminLevel.hAdminLevelServerAdmin;
         account.Save();

         var newApp = new Application();
         var authenticated = newApp.Authenticate(account.Address, "test");
         Assert.IsNotNull(authenticated);

         // This should throw an exception.
         var newDomain = newApp.Domains.Add();
      }

      [Test]
      public void DomainAdminShouldBeAbleToSaveDomain()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();

         // Create an account with normal privileges.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");
         account.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         account.Save();

         var newApp = new Application();
         var authenticated = newApp.Authenticate(account.Address, "test");
         Assert.IsNotNull(authenticated);

         Assert.AreEqual(1, newApp.Domains.Count);

         // Retrieve our domain.
         var newDomain = newApp.Domains[0];
         newDomain.Save();
      }

      [Test]
      [Description("A grant on a folder somebody else owns is not reachable over COM by an ordinary account: the accounts collection it is given holds its own account alone, so there is no folder to reach the grant through. Its OWN folder's grants it does reach, and changes, which is the parity with IMAP SETACL that Save() has no administrator check for.")]
      public void AnAccountCannotReachAGrantOnAFolderItDoesNotOwn()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();

         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "aclowner@example.test", "test");
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "aclother@example.test", "test");

         // The share an owner would make with SETACL, made here by the server
         // administrator: the other account may look up and read this folder.
         var shared = owner.IMAPFolders.Add("Shared");
         shared.Save();

         var granted = shared.Permissions.Add();
         granted.PermissionType = eACLPermissionType.ePermissionTypeUser;
         granted.PermissionAccountID = other.ID;
         granted.set_Permission(eACLPermission.ePermissionLookup, true);
         granted.set_Permission(eACLPermission.ePermissionRead, true);
         granted.Save();

         var delegateApp = new Application();
         Assert.IsNotNull(delegateApp.Authenticate(other.Address, "test"));

         // Everything the delegate can see of the domain is its own account. This is
         // the boundary: Save and Delete on a permission ask no authority of their
         // own, so what stops this account editing the grant above is that it cannot
         // get to the folder the grant is on.
         var visible = delegateApp.Domains[0].Accounts;
         Assert.AreEqual(1, visible.Count, "An ordinary account sees only its own account.");
         Assert.AreEqual(other.Address, visible[0].Address);

         Assert.Throws<COMException>(() => visible.get_ItemByAddress(owner.Address),
            "The owner's account must not be reachable, or the grant on its folder would be.");

         // Its own folder it does reach, and administers - the same thing SETACL
         // lets it do over IMAP, which is why COM asks for no administrator here.
         var own = visible[0].IMAPFolders.Add("Own");
         own.Save();
         Assert.AreEqual(0, own.Permissions.Count);

         // And the grant on the owner's folder is exactly as the administrator left
         // it: the delegate reached nothing and changed nothing.
         var after = owner.IMAPFolders.get_ItemByName("Shared").Permissions;
         Assert.AreEqual(1, after.Count);
         Assert.AreEqual(other.ID, after[0].PermissionAccountID);
         Assert.IsTrue(after[0].get_Permission(eACLPermission.ePermissionRead));
         Assert.IsFalse(after[0].get_Permission(eACLPermission.ePermissionAdminister));
      }

      [Test]
      public void UserShouldNotBeAbleToSaveDomain()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();

         // Create an account with normal privileges.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");
         account.AdminLevel = eAdminLevel.hAdminLevelNormal;
         account.Save();

         var newApp = new Application();
         var authenticated = newApp.Authenticate(account.Address, "test");
         Assert.IsNotNull(authenticated);

         Assert.AreEqual(1, newApp.Domains.Count);

         // Retrieve our domain.
         var newDomain = newApp.Domains[0];
         var ex = Assert.Throws<COMException>(() => newDomain.Save());
         StringAssert.Contains("You do not have access to this property / method.", ex.Message);
      }
   }
}