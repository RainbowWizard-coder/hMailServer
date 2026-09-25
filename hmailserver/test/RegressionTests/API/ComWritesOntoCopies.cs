// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Runtime.InteropServices;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A COM change to an item of a collection the running server reads is written onto a
   ///    copy the COM object holds, and is in effect from its Save() and not before
   ///    (COM/COMCollection.h; Roadmap2, "A COM change to a shared configuration object is
   ///    written onto a copy"). Until 24 September 2026 a put_* on a route, a relay, a DNS
   ///    black list, a SURBL server, a remote domain policy, a server message, a blocked
   ///    attachment, an SSL certificate, a group or a route's address wrote the very object
   ///    every session reads, so it was in effect at once, Save() or not, and a Save() the
   ///    database refused left the refused value in effect - except a remote domain policy,
   ///    whose refused save read the running record back from the database in place.
   ///
   ///    One test per class, each in the same steps. The item is read from its collection
   ///    twice, into the object that will be written and into one that will not, and a field
   ///    is put on the first; that object reads back what was put. The same item read from
   ///    the collection again - the object the running server holds - still has the old
   ///    value. After Save() a fresh read has the new one, and the object read before the
   ///    save, which was never written, keeps what it read: the collection was read again
   ///    under it and it holds the item it was given. Every one of these tests fails against
   ///    the code before the row (866c6287c), where both objects wrapped the running item
   ///    and the put wrote it: the "unchanged before Save" and the "keeps what it read"
   ///    assertions both see the put. The group's reads go through one Groups object, taken
   ///    once, because Settings.Groups reads the groups from the database every time it is
   ///    asked, and a read through a fresh one would find the old name in the database
   ///    whether or not the running group had been renamed.
   ///
   ///    The others: a refused route save, which fails against the old code too (it left the
   ///    refused values on the running route); a route's address list held across the
   ///    route's save, and a Save() with nothing put on an object a refresh has overtaken,
   ///    which guard the row's first version (4b175e3cb) - a save that read the routes
   ///    again stranded the held list, and a Save() with nothing put wrote back the values
   ///    the object was handed - and which the old code, having neither defect, passes; and
   ///    a save whose list cannot be read again, reported as HM7390, which the old code
   ///    never reported.
   /// </summary>
   [TestFixture]
   public class ComWritesOntoCopies : TestFixtureBase
   {
      [Test]
      [Description("A route's fields are put on a copy: the COM object reads them back, the running route keeps the old ones until Save, a fresh read has the new ones after it, and an object read before the Save keeps what it read.")]
      public void ARouteChangeIsNotInEffectUntilSave()
      {
         Route created = AddRoute("copies-route.test", 2525);
         int id = created.ID;

         Route held = _settings.Routes.get_ItemByDBID(id);
         Route before = _settings.Routes.get_ItemByDBID(id);

         held.DomainName = "copies-route-renamed.test";
         held.TargetSMTPPort = 2626;
         held.Description = "changed over COM";
         held.NumberOfTries = 7;

         Assert.AreEqual("copies-route-renamed.test", held.DomainName, "The object the put was made on reads its own copy.");
         Assert.AreEqual(2626, held.TargetSMTPPort);
         Assert.AreEqual("changed over COM", held.Description);
         Assert.AreEqual(7, held.NumberOfTries);

         Route running = _settings.Routes.get_ItemByDBID(id);
         Assert.AreEqual("copies-route.test", running.DomainName, "The running route was changed before Save.");
         Assert.AreEqual(2525, running.TargetSMTPPort, "The running route was changed before Save.");
         Assert.AreEqual("", running.Description);
         Assert.AreEqual(1, running.NumberOfTries);

         held.Save();

         Route saved = _settings.Routes.get_ItemByDBID(id);
         Assert.AreEqual("copies-route-renamed.test", saved.DomainName, "The saved route is not the running one.");
         Assert.AreEqual(2626, saved.TargetSMTPPort);
         Assert.AreEqual("changed over COM", saved.Description);
         Assert.AreEqual(7, saved.NumberOfTries);
         Assert.AreEqual(id, saved.ID, "A save keeps the route's id.");
         Assert.AreEqual(1, _settings.Routes.Count, "A save is not a second route.");

         Assert.AreEqual("copies-route.test", before.DomainName, "An object read before the Save keeps what it read.");
         Assert.AreEqual(2525, before.TargetSMTPPort);

         // Written on again: the object holds its copy, and a second Save publishes it.
         held.TargetSMTPPort = 2727;
         Assert.AreEqual(2626, _settings.Routes.get_ItemByDBID(id).TargetSMTPPort, "The second change was in effect before its Save.");
         held.Save();
         Assert.AreEqual(2727, _settings.Routes.get_ItemByDBID(id).TargetSMTPPort);

         // A put never saved changes nothing.
         Route abandoned = _settings.Routes.get_ItemByDBID(id);
         abandoned.TargetSMTPPort = 9999;
         abandoned.DomainName = "never-saved.test";
         Assert.AreEqual(2727, _settings.Routes.get_ItemByDBID(id).TargetSMTPPort, "A put that was never saved is in effect.");
         Assert.AreEqual("copies-route-renamed.test", _settings.Routes.get_ItemByDBID(id).DomainName);
      }

      [Test]
      [Description("A Save with nothing put before it, on an object handed out before another object's save replaced the route, saves the running route's values and not the ones the object was handed: the other object's change survives it, in the running server and in the database.")]
      public void ASaveWithNothingPutDoesNotWriteBackWhatTheObjectWasHanded()
      {
         Route created = AddRoute("copies-stale.test", 2525);
         int id = created.ID;

         Route stale = _settings.Routes.get_ItemByDBID(id);
         Route changer = _settings.Routes.get_ItemByDBID(id);

         changer.TargetSMTPPort = 2626;
         changer.Save();

         // Nothing put: the object still holds the route it was given, port 2525, which the
         // save above replaced. Saving it saves a copy of the running route.
         stale.Save();

         Assert.AreEqual(2626, _settings.Routes.get_ItemByDBID(id).TargetSMTPPort, "The running route lost the other object's change.");

         _settings.Routes.Refresh();
         Assert.AreEqual(2626, _settings.Routes.get_ItemByDBID(id).TargetSMTPPort, "The database has the values the object was handed, written back over the other object's change.");
      }

      [Test]
      [Description("A route change the database refuses - a second route for a domain that has one - leaves the running route as it was; before, the refused name governed delivery until the next restart.")]
      public void ARefusedRouteChangeLeavesTheRunningRouteAsItWas()
      {
         AddRoute("copies-first.test", 2525);
         Route second = AddRoute("copies-second.test", 2526);
         int id = second.ID;

         Route held = _settings.Routes.get_ItemByDBID(id);
         held.DomainName = "copies-first.test";
         held.TargetSMTPPort = 2999;

         Assert.Throws<COMException>(() => held.Save(), "A second route for the same domain was saved.");

         Route running = _settings.Routes.get_ItemByDBID(id);
         Assert.AreEqual("copies-second.test", running.DomainName, "The refused name is on the running route.");
         Assert.AreEqual(2526, running.TargetSMTPPort, "The refused port is on the running route.");
         Assert.AreEqual("copies-first.test", held.DomainName, "The object keeps what was put on it, to be corrected and saved again.");
      }

      [Test]
      [Description("An address of a running route is put on a copy and in effect from its Save, and an object read before the save keeps what it read; an address added through a route object that holds a copy of the route goes into the running route's list.")]
      public void ARouteAddressChangeIsNotInEffectUntilSave()
      {
         Route route = AddRoute("copies-addresses.test", 2525);
         int routeId = route.ID;

         RouteAddress first = route.Addresses.Add();
         first.Address = "one@copies-addresses.test";
         first.Save();
         int addressId = first.ID;

         RouteAddress held = _settings.Routes.get_ItemByDBID(routeId).Addresses.get_ItemByDBID(addressId);
         RouteAddress before = _settings.Routes.get_ItemByDBID(routeId).Addresses.get_ItemByDBID(addressId);
         held.Address = "two@copies-addresses.test";
         Assert.AreEqual("two@copies-addresses.test", held.Address);
         Assert.AreEqual("one@copies-addresses.test", RunningAddress(routeId, addressId), "The running address was changed before Save.");

         held.Save();
         Assert.AreEqual("two@copies-addresses.test", RunningAddress(routeId, addressId));
         Assert.AreEqual(1, _settings.Routes.get_ItemByDBID(routeId).Addresses.Count, "A save is not a second address.");
         Assert.AreEqual("one@copies-addresses.test", before.Address, "An object read before the Save keeps what it read.");

         // The route written onto a copy, and then an address added through the same
         // object: the list it hands out is the running route's, not the copy's.
         Route copyHolder = _settings.Routes.get_ItemByDBID(routeId);
         copyHolder.Description = "holds a copy";
         RouteAddress added = copyHolder.Addresses.Add();
         added.Address = "three@copies-addresses.test";
         added.Save();

         Assert.AreEqual(2, _settings.Routes.get_ItemByDBID(routeId).Addresses.Count, "The address went into a list the running route does not read.");
         Assert.AreEqual("", _settings.Routes.get_ItemByDBID(routeId).Description, "The route's own change was in effect before its Save.");
      }

      [Test]
      [Description("A route's Addresses read before the route's own Save - which reads the routes again and so replaces the route - is the retired route's list; an address added or deleted through it still reaches the running route, which reads its list again.")]
      public void AnAddressAddedThroughAListHeldAcrossTheRouteSaveReachesTheRunningRoute()
      {
         Route route = AddRoute("copies-held-list.test", 2525);
         int routeId = route.ID;

         RouteAddresses heldList = _settings.Routes.get_ItemByDBID(routeId).Addresses;

         Route changer = _settings.Routes.get_ItemByDBID(routeId);
         changer.Description = "saved while the list was held";
         changer.Save();

         RouteAddress added = heldList.Add();
         added.Address = "held@copies-held-list.test";
         added.Save();

         RouteAddresses running = _settings.Routes.get_ItemByDBID(routeId).Addresses;
         Assert.AreEqual(1, running.Count, "The address was saved into the retired route's list only, and the running route refuses it.");
         Assert.AreEqual("held@copies-held-list.test", running[0].Address);

         heldList.DeleteByDBID(added.ID);
         Assert.AreEqual(0, _settings.Routes.get_ItemByDBID(routeId).Addresses.Count, "The address was deleted from the database and the running route still accepts it.");
      }

      [Test]
      [Description("An incoming relay's name and range are put on a copy and in effect from Save, so a session never reads one end of the old range with the other end of the new.")]
      public void AnIncomingRelayChangeIsNotInEffectUntilSave()
      {
         IncomingRelay relay = _settings.IncomingRelays.Add();
         relay.Name = "copies-relay";
         relay.LowerIP = "10.71.0.1";
         relay.UpperIP = "10.71.0.9";
         relay.Save();
         int id = relay.ID;

         IncomingRelay held = _settings.IncomingRelays.get_ItemByDBID(id);
         IncomingRelay before = _settings.IncomingRelays.get_ItemByDBID(id);
         held.Name = "copies-relay-moved";
         held.LowerIP = "10.72.0.1";
         held.UpperIP = "10.72.0.9";

         Assert.AreEqual("10.72.0.1", held.LowerIP);
         Assert.AreEqual("10.72.0.9", held.UpperIP);

         IncomingRelay running = _settings.IncomingRelays.get_ItemByDBID(id);
         Assert.AreEqual("copies-relay", running.Name, "The running relay was changed before Save.");
         Assert.AreEqual("10.71.0.1", running.LowerIP, "The running relay was changed before Save.");
         Assert.AreEqual("10.71.0.9", running.UpperIP, "The running relay was changed before Save.");

         held.Save();

         IncomingRelay saved = _settings.IncomingRelays.get_ItemByDBID(id);
         Assert.AreEqual("copies-relay-moved", saved.Name);
         Assert.AreEqual("10.72.0.1", saved.LowerIP);
         Assert.AreEqual("10.72.0.9", saved.UpperIP);
         Assert.AreEqual("10.71.0.1", before.LowerIP, "An object read before the Save keeps what it read.");
         Assert.AreEqual("10.71.0.9", before.UpperIP);
      }

      [Test]
      [Description("A DNS black list's host, message, answer and score are put on a copy and in effect from Save, and an object read before the save keeps what it read.")]
      public void ADnsBlackListChangeIsNotInEffectUntilSave()
      {
         int id = AddDnsBlackList("copies-dnsbl.test");

         DNSBlackList held = _settings.AntiSpam.DNSBlackLists.get_ItemByDBID(id);
         DNSBlackList before = _settings.AntiSpam.DNSBlackLists.get_ItemByDBID(id);
         held.DNSHost = "copies-dnsbl-changed.test";
         held.RejectMessage = "Listed, changed";
         held.ExpectedResult = "127.0.0.3";
         held.Score = 6;
         Assert.AreEqual("copies-dnsbl-changed.test", held.DNSHost);
         Assert.AreEqual(6, held.Score);

         DNSBlackList running = _settings.AntiSpam.DNSBlackLists.get_ItemByDBID(id);
         Assert.AreEqual("copies-dnsbl.test", running.DNSHost, "The running list was changed before Save.");
         Assert.AreEqual("Listed", running.RejectMessage);
         Assert.AreEqual("127.0.0.2", running.ExpectedResult);
         Assert.AreEqual(3, running.Score);

         held.Save();

         DNSBlackList saved = _settings.AntiSpam.DNSBlackLists.get_ItemByDBID(id);
         Assert.AreEqual("copies-dnsbl-changed.test", saved.DNSHost);
         Assert.AreEqual("Listed, changed", saved.RejectMessage);
         Assert.AreEqual("127.0.0.3", saved.ExpectedResult);
         Assert.AreEqual(6, saved.Score);
         Assert.IsFalse(saved.Active);
         Assert.AreEqual("copies-dnsbl.test", before.DNSHost, "An object read before the Save keeps what it read.");
         Assert.AreEqual(3, before.Score);
      }

      [Test]
      [Description("A save whose list cannot be read again afterwards - the database fault injector failing the DNS black lists' read, and nothing else - still succeeds, is reported as HM7390, and is not in effect until the list is next read.")]
      public void ASaveWhoseListCannotBeReadAgainIsReportedAndNotYetInEffect()
      {
         int id = AddDnsBlackList("copies-unread.test");

         DNSBlackList held = _settings.AntiSpam.DNSBlackLists.get_ItemByDBID(id);
         held.DNSHost = "copies-unread-changed.test";

         // The list's read alone: DNSBlackLists::Refresh orders by id, and the save's
         // UPDATE names the table without an order. The injector applies when it is
         // saved; a reinitialise with it set would fail the load of the whole
         // anti-spam configuration, and is not what is under test.
         IniFileSetting.Write("SimulateDatabaseFailureFor", "hm_dnsbl order by");
         try
         {
            held.Save();
         }
         finally
         {
            IniFileSetting.Write("SimulateDatabaseFailureFor", "");
         }

         CustomAsserts.AssertReportedError("HM7390", "HM6119");

         Assert.AreEqual("copies-unread-changed.test", held.DNSHost, "The object does not read what it saved.");
         Assert.AreEqual("copies-unread.test", _settings.AntiSpam.DNSBlackLists.get_ItemByDBID(id).DNSHost,
            "The change is in effect although its list could not be read again.");

         _settings.AntiSpam.DNSBlackLists.Refresh();
         Assert.AreEqual("copies-unread-changed.test", _settings.AntiSpam.DNSBlackLists.get_ItemByDBID(id).DNSHost,
            "The change was not saved, or the next read of the list does not put it in effect.");
      }

      [Test]
      [Description("A SURBL server's host, message, answer and score are put on a copy and in effect from Save, and an object read before the save keeps what it read.")]
      public void ASurblServerChangeIsNotInEffectUntilSave()
      {
         SURBLServer server = _settings.AntiSpam.SURBLServers.Add();
         server.Active = false;
         server.DNSHost = "copies-surbl.test";
         server.RejectMessage = "Listed";
         server.ExpectedResult = "127.0.0.2";
         server.Score = 3;
         server.Save();
         int id = server.ID;

         try
         {
            SURBLServer held = _settings.AntiSpam.SURBLServers.get_ItemByDBID(id);
            SURBLServer before = _settings.AntiSpam.SURBLServers.get_ItemByDBID(id);
            held.DNSHost = "copies-surbl-changed.test";
            held.RejectMessage = "Listed, changed";
            held.ExpectedResult = "127.0.0.4";
            held.Score = 8;
            Assert.AreEqual("copies-surbl-changed.test", held.DNSHost);

            SURBLServer running = _settings.AntiSpam.SURBLServers.get_ItemByDBID(id);
            Assert.AreEqual("copies-surbl.test", running.DNSHost, "The running server was changed before Save.");
            Assert.AreEqual("Listed", running.RejectMessage);
            Assert.AreEqual("127.0.0.2", running.ExpectedResult);
            Assert.AreEqual(3, running.Score);

            held.Save();

            SURBLServer saved = _settings.AntiSpam.SURBLServers.get_ItemByDBID(id);
            Assert.AreEqual("copies-surbl-changed.test", saved.DNSHost);
            Assert.AreEqual("Listed, changed", saved.RejectMessage);
            Assert.AreEqual("127.0.0.4", saved.ExpectedResult);
            Assert.AreEqual(8, saved.Score);
            Assert.AreEqual("copies-surbl.test", before.DNSHost, "An object read before the Save keeps what it read.");
            Assert.AreEqual(3, before.Score);
         }
         finally
         {
            // PerformBasicSetup removes every SURBL server but multi.surbl.org, so this
            // is for the run whose last test this was.
            _settings.AntiSpam.SURBLServers.DeleteByDBID(id);
         }
      }

      [Test]
      [Description("A remote domain policy's fields are put on a copy and in effect from Save - a put never writes the record delivery reads - and an object read before the save keeps what it read.")]
      public void ARemoteDomainPolicyChangeIsNotInEffectUntilSave()
      {
         RemoteDomainPolicy policy = _settings.RemoteDomainPolicies.Add();
         policy.DomainName = "copies-policy.test";
         policy.Description = "as added";
         policy.Active = true;
         policy.MaxConnections = 2;
         policy.Save();
         int id = policy.ID;

         try
         {
            RemoteDomainPolicy held = _settings.RemoteDomainPolicies.get_ItemByDBID(id);
            RemoteDomainPolicy before = _settings.RemoteDomainPolicies.get_ItemByDBID(id);
            held.Description = "changed over COM";
            held.MaxConnections = 5;
            held.MaxMessagesPerMinute = 30;
            Assert.AreEqual("changed over COM", held.Description);
            Assert.AreEqual(5, held.MaxConnections);

            RemoteDomainPolicy running = _settings.RemoteDomainPolicies.get_ItemByDBID(id);
            Assert.AreEqual("as added", running.Description, "The running policy was changed before Save.");
            Assert.AreEqual(2, running.MaxConnections, "The running policy was changed before Save.");
            Assert.AreEqual(0, running.MaxMessagesPerMinute);

            held.Save();

            RemoteDomainPolicy saved = _settings.RemoteDomainPolicies.get_ItemByDBID(id);
            Assert.AreEqual("changed over COM", saved.Description);
            Assert.AreEqual(5, saved.MaxConnections);
            Assert.AreEqual(30, saved.MaxMessagesPerMinute);
            Assert.AreEqual(5, _settings.RemoteDomainPolicies.get_PolicyForDomain("copies-policy.test").MaxConnections,
               "The policy delivery asks for is not the saved one.");
            Assert.AreEqual("as added", before.Description, "An object read before the Save keeps what it read.");
            Assert.AreEqual(2, before.MaxConnections);
         }
         finally
         {
            _settings.RemoteDomainPolicies.DeleteByDBID(id);
         }
      }

      [Test]
      [Description("A server message's text is put on a copy and in effect from Save, and an object read before the save keeps what it read.")]
      public void AServerMessageChangeIsNotInEffectUntilSave()
      {
         const string name = "VIRUS_ATTACHMENT_REMOVED";
         string original = _settings.ServerMessages.get_ItemByName(name).Text;

         try
         {
            ServerMessage held = _settings.ServerMessages.get_ItemByName(name);
            ServerMessage before = _settings.ServerMessages.get_ItemByName(name);
            held.Text = "Changed over COM, on a copy.";
            Assert.AreEqual("Changed over COM, on a copy.", held.Text);
            Assert.AreEqual(original, _settings.ServerMessages.get_ItemByName(name).Text, "The running message was changed before Save.");

            held.Save();
            Assert.AreEqual("Changed over COM, on a copy.", _settings.ServerMessages.get_ItemByName(name).Text);
            Assert.AreEqual(original, before.Text, "An object read before the Save keeps what it read.");
         }
         finally
         {
            ServerMessage restore = _settings.ServerMessages.get_ItemByName(name);
            restore.Text = original;
            restore.Save();
         }

         Assert.AreEqual(original, _settings.ServerMessages.get_ItemByName(name).Text, "Put back as it was.");
      }

      [Test]
      [Description("A blocked attachment's wildcard and description are put on a copy and in effect from Save, and an object read before the save keeps what it read.")]
      public void ABlockedAttachmentChangeIsNotInEffectUntilSave()
      {
         BlockedAttachment attachment = _settings.AntiVirus.BlockedAttachments.Add();
         attachment.Wildcard = "*.copies-one";
         attachment.Description = "as added";
         attachment.Save();
         int id = attachment.ID;

         try
         {
            BlockedAttachment held = _settings.AntiVirus.BlockedAttachments.get_ItemByDBID(id);
            BlockedAttachment before = _settings.AntiVirus.BlockedAttachments.get_ItemByDBID(id);
            held.Wildcard = "*.copies-two";
            held.Description = "changed over COM";
            Assert.AreEqual("*.copies-two", held.Wildcard);

            BlockedAttachment running = _settings.AntiVirus.BlockedAttachments.get_ItemByDBID(id);
            Assert.AreEqual("*.copies-one", running.Wildcard, "The running wildcard was changed before Save.");
            Assert.AreEqual("as added", running.Description);

            held.Save();

            BlockedAttachment saved = _settings.AntiVirus.BlockedAttachments.get_ItemByDBID(id);
            Assert.AreEqual("*.copies-two", saved.Wildcard);
            Assert.AreEqual("changed over COM", saved.Description);
            Assert.AreEqual("*.copies-one", before.Wildcard, "An object read before the Save keeps what it read.");
            Assert.AreEqual("as added", before.Description);
         }
         finally
         {
            _settings.AntiVirus.BlockedAttachments.DeleteByDBID(id);
         }
      }

      [Test]
      [Description("An SSL certificate's name and files are put on a copy and in effect from Save, and an object read before the save keeps what it read.")]
      public void AnSslCertificateChangeIsNotInEffectUntilSave()
      {
         SSLCertificate certificate = _settings.SSLCertificates.Add();
         certificate.Name = "copies-certificate";
         certificate.CertificateFile = "C:\\copies\\one.pem";
         certificate.PrivateKeyFile = "C:\\copies\\one.key";
         certificate.Save();
         int id = certificate.ID;

         SSLCertificate held = _settings.SSLCertificates.get_ItemByDBID(id);
         SSLCertificate before = _settings.SSLCertificates.get_ItemByDBID(id);
         held.Name = "copies-certificate-renewed";
         held.CertificateFile = "C:\\copies\\two.pem";
         held.PrivateKeyFile = "C:\\copies\\two.key";
         Assert.AreEqual("C:\\copies\\two.pem", held.CertificateFile);

         SSLCertificate running = _settings.SSLCertificates.get_ItemByDBID(id);
         Assert.AreEqual("copies-certificate", running.Name, "The running certificate was changed before Save.");
         Assert.AreEqual("C:\\copies\\one.pem", running.CertificateFile, "The running certificate was changed before Save.");
         Assert.AreEqual("C:\\copies\\one.key", running.PrivateKeyFile);

         held.Save();

         SSLCertificate saved = _settings.SSLCertificates.get_ItemByDBID(id);
         Assert.AreEqual("copies-certificate-renewed", saved.Name);
         Assert.AreEqual("C:\\copies\\two.pem", saved.CertificateFile);
         Assert.AreEqual("C:\\copies\\two.key", saved.PrivateKeyFile);
         Assert.AreEqual("C:\\copies\\one.pem", before.CertificateFile, "An object read before the Save keeps what it read.");
         Assert.AreEqual("copies-certificate", before.Name);
      }

      [Test]
      [Description("A group's name is put on a copy and in effect from Save - the running group, the one an IMAP session looks a group up in by name, is never renamed in place - and an object read before the save keeps what it read. Every read goes through one Groups object, because Settings.Groups reads the groups from the database each time and would show the old name either way.")]
      public void AGroupChangeIsNotInEffectUntilSave()
      {
         Group group = _settings.Groups.Add();
         group.Name = "CopiesGroup";
         group.Save();
         int id = group.ID;

         // One read of the groups, whose getters hand out the running groups without
         // reading them again.
         Groups groups = _settings.Groups;

         Group held = groups.get_ItemByDBID(id);
         Group before = groups.get_ItemByDBID(id);
         held.Name = "CopiesGroupRenamed";
         Assert.AreEqual("CopiesGroupRenamed", held.Name);
         Assert.AreEqual("CopiesGroup", groups.get_ItemByDBID(id).Name, "The running group was renamed before Save.");

         held.Save();

         Assert.AreEqual("CopiesGroupRenamed", groups.get_ItemByDBID(id).Name, "The saved name is not the running group's.");
         Assert.AreEqual(id, groups.get_ItemByName("CopiesGroupRenamed").ID);
         Assert.AreEqual(1, groups.Count, "A rename is not a second group.");
         Assert.AreEqual("CopiesGroup", before.Name, "An object read before the Save keeps what it read.");
      }

      private Route AddRoute(string domainName, int port)
      {
         Route route = _settings.Routes.Add();
         route.DomainName = domainName;
         route.TargetSMTPHost = "127.0.0.1";
         route.TargetSMTPPort = port;
         route.NumberOfTries = 1;
         route.MinutesBetweenTry = 5;
         route.TreatRecipientAsLocalDomain = false;
         route.TreatSenderAsLocalDomain = false;
         route.AllAddresses = false;
         route.ConnectionSecurity = eConnectionSecurity.eCSNone;
         route.Save();
         return route;
      }

      private int AddDnsBlackList(string host)
      {
         // Inactive: nothing here should be looked up.
         DNSBlackList list = _settings.AntiSpam.DNSBlackLists.Add();
         list.Active = false;
         list.DNSHost = host;
         list.RejectMessage = "Listed";
         list.ExpectedResult = "127.0.0.2";
         list.Score = 3;
         list.Save();
         return list.ID;
      }

      private string RunningAddress(int routeId, int addressId)
      {
         return _settings.Routes.get_ItemByDBID(routeId).Addresses.get_ItemByDBID(addressId).Address;
      }
   }
}
