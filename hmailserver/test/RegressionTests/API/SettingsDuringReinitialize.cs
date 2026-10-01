// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Settings read over COM while the server reinitialises are the settings in force.
   ///
   ///    A reinitialise runs Configuration::Load while COM calls from the Control Panel or
   ///    a script go on being answered. Until 29 September 2026 Load put a new, empty
   ///    settings store in place and only then read hm_settings into it, and put new SMTP,
   ///    POP3 and IMAP configurations in place before it had loaded them, so a read in
   ///    that window found every setting missing: 0 or an empty string, and an HM5015
   ///    line in the ERROR log for each read - and a pointer copied while it was being
   ///    replaced could hold a freed object. Load now builds each object whole and hands
   ///    it over under a lock, so a read in the window gets the previous load's objects,
   ///    whose values are the same.
   ///
   ///    A value read that is not the one set fails the test; the fixed server cannot
   ///    give one, and the old one gave one to any read that fell in the window, which a
   ///    reader in a tight loop through three reinitialises reaches. The reads made while
   ///    a reinitialise was running are counted, so that a run in which none overlapped
   ///    one fails rather than passes for nothing.
   ///
   ///    The Windows suite only: over the REST API the listener itself is stopped while
   ///    the server reloads, so there is no read to make.
   /// </summary>
   [TestFixture]
   public class SettingsDuringReinitialize : TestFixtureBase
   {
      [Test]
      public void SettingsReadWhileTheServerReinitialisesAreTheValuesInForce()
      {
         const int maxSmtp = 4321;
         const int maxPop3 = 1234;
         const string welcome = "reinitialise-probe";

         var oldSmtp = _settings.MaxSMTPConnections;
         var oldPop3 = _settings.MaxPOP3Connections;
         var oldWelcome = _settings.WelcomeIMAP;

         _settings.MaxSMTPConnections = maxSmtp;
         _settings.MaxPOP3Connections = maxPop3;
         _settings.WelcomeIMAP = welcome;

         try
         {
            var settings = _settings;
            var wrong = new List<string>();
            Exception failure = null;
            long reads = 0;
            long readsDuring = 0;
            var reinitializing = 0;
            var stop = 0;

            var reader = new Thread(() =>
            {
               try
               {
                  while (Volatile.Read(ref stop) == 0)
                  {
                     var during = Volatile.Read(ref reinitializing) == 1;

                     var smtp = settings.MaxSMTPConnections;
                     var pop3 = settings.MaxPOP3Connections;
                     var imap = settings.WelcomeIMAP;

                     if (smtp != maxSmtp || pop3 != maxPop3 || imap != welcome)
                     {
                        lock (wrong)
                        {
                           if (wrong.Count < 10)
                              wrong.Add("MaxSMTPConnections " + smtp + ", MaxPOP3Connections " + pop3 + ", WelcomeIMAP '" + imap + "'");
                        }
                     }

                     Interlocked.Increment(ref reads);

                     // Counted only when the reinitialise was running both before and
                     // after the read, so that the read certainly overlapped it.
                     if (during && Volatile.Read(ref reinitializing) == 1)
                        Interlocked.Increment(ref readsDuring);
                  }
               }
               catch (Exception ex)
               {
                  failure = ex;
               }
            });

            reader.IsBackground = true;
            reader.Start();

            // The reader is stopped whatever Reinitialize does: a reinitialise that
            // throws (a listener that cannot be opened again) would otherwise leave it
            // calling the server in a tight loop through every test after this one.
            try
            {
               for (var i = 0; i < 3; i++)
               {
                  Volatile.Write(ref reinitializing, 1);
                  _application.Reinitialize();
                  Volatile.Write(ref reinitializing, 0);
               }
            }
            finally
            {
               Volatile.Write(ref reinitializing, 0);
               Volatile.Write(ref stop, 1);
            }

            Assert.That(reader.Join(TimeSpan.FromSeconds(60)), Is.True, "The reader did not stop.");
            Assert.That(failure, Is.Null, "A read failed: " + failure);
            Assert.That(wrong, Is.Empty, "Values read during a reinitialise that are not the ones set: " + string.Join("; ", wrong));
            Assert.That(Interlocked.Read(ref readsDuring), Is.GreaterThan(0),
               "No read overlapped a reinitialise (" + Interlocked.Read(ref reads) + " reads in all), so nothing was tested.");
         }
         finally
         {
            _settings.MaxSMTPConnections = oldSmtp;
            _settings.MaxPOP3Connections = oldPop3;
            _settings.WelcomeIMAP = oldWelcome;
         }
      }

      /// <summary>
      ///    A list COM hands out and a caller keeps across a reinitialise - the Control
      ///    Panel keeps one for as long as its page is open - is the list the server
      ///    reads after it. Up to 6.3.5 each reinitialise made new lists (routes,
      ///    incoming relays, server messages, DNS blacklists and the rest), so a route
      ///    added, a relay deleted or a message changed through a list held from before
      ///    reached the database and the old list only: the server went on without the
      ///    route, trusting the deleted relay and sending the old text until the next
      ///    reinitialise. Configuration::Load now reads the lists it made at the start
      ///    again in place. A fresh Settings.Routes (and the others) is what the server
      ///    reads, so each change made through a held list must be in it at once.
      /// </summary>
      [Test]
      public void ListsHeldAcrossAReinitialiseAreTheListsTheServerReads()
      {
         const string routeDomain = "held-across-reinitialise.test";
         const string relayName = "held-across-reinitialise";
         const string blackListHost = "held-across-reinitialise.dnsbl.test";
         const string messageName = "VIRUS_ATTACHMENT_REMOVED";
         const string changedText = "Changed through a list held across a reinitialise.";

         Routes routes = _settings.Routes;
         IncomingRelays relays = _settings.IncomingRelays;
         ServerMessages messages = _settings.ServerMessages;
         DNSBlackLists blackLists = _settings.AntiSpam.DNSBlackLists;
         string originalText = messages.get_ItemByName(messageName).Text;

         _application.Reinitialize();

         try
         {
            Route route = routes.Add();
            route.DomainName = routeDomain;
            route.TargetSMTPHost = "127.0.0.1";
            route.TargetSMTPPort = 25;
            route.NumberOfTries = 1;
            route.MinutesBetweenTry = 5;
            route.TreatRecipientAsLocalDomain = false;
            route.TreatSenderAsLocalDomain = false;
            route.AllAddresses = false;
            route.ConnectionSecurity = eConnectionSecurity.eCSNone;
            route.Save();
            Assert.That(RouteIds(routeDomain), Has.Count.EqualTo(1),
               "A route added through a Routes object held across a reinitialise is not in the routes the server reads.");

            IncomingRelay relay = relays.Add();
            relay.Name = relayName;
            relay.LowerIP = "10.73.0.1";
            relay.UpperIP = "10.73.0.9";
            relay.Save();
            int relayId = relay.ID;
            Assert.That(RelayIds(relayName), Has.Count.EqualTo(1),
               "A relay added through an IncomingRelays object held across a reinitialise is not in the relays the server reads.");

            relays.DeleteByDBID(relayId);
            Assert.That(RelayIds(relayName), Is.Empty,
               "A relay deleted through an IncomingRelays object held across a reinitialise is still trusted by the server.");

            ServerMessage message = messages.get_ItemByName(messageName);
            message.Text = changedText;
            message.Save();
            Assert.AreEqual(changedText, _settings.ServerMessages.get_ItemByName(messageName).Text,
               "A message changed through a ServerMessages object held across a reinitialise is not the text the server sends.");

            DNSBlackList blackList = blackLists.Add();
            blackList.Active = false;
            blackList.DNSHost = blackListHost;
            blackList.RejectMessage = "Listed";
            blackList.ExpectedResult = "127.0.0.2";
            blackList.Score = 3;
            blackList.Save();
            Assert.That(BlackListIds(blackListHost), Has.Count.EqualTo(1),
               "A DNS blacklist added through a DNSBlackLists object held across a reinitialise is not in the lists the server asks.");
         }
         finally
         {
            foreach (int id in RouteIds(routeDomain))
               _settings.Routes.DeleteByDBID(id);
            foreach (int id in RelayIds(relayName))
               _settings.IncomingRelays.DeleteByDBID(id);
            foreach (int id in BlackListIds(blackListHost))
               _settings.AntiSpam.DNSBlackLists.DeleteByDBID(id);

            ServerMessage restore = _settings.ServerMessages.get_ItemByName(messageName);
            restore.Text = originalText;
            restore.Save();
         }
      }

      /// <summary>
      ///    A Database object made before a reinitialise answers for the database the
      ///    server is connected to after it: up to 6.3.5 its IsConnected asked the
      ///    connection manager it was made with, which the reinitialise had closed.
      /// </summary>
      [Test]
      public void ADatabaseObjectHeldAcrossAReinitialiseAnswersForTheDatabaseInUse()
      {
         Database database = _application.Database;
         Assert.IsTrue(database.IsConnected, "Connected before the reinitialise.");

         _application.Reinitialize();

         Assert.IsTrue(database.IsConnected,
            "A Database object held across a reinitialise answered for the connection manager the reinitialise closed.");
         Assert.AreEqual(_application.Database.CurrentVersion, database.CurrentVersion);
      }

      private List<int> RouteIds(string domainName)
      {
         var ids = new List<int>();
         Routes routes = _settings.Routes;
         for (int i = 0; i < routes.Count; i++)
         {
            Route route = routes[i];
            if (string.Equals(route.DomainName, domainName, StringComparison.OrdinalIgnoreCase))
               ids.Add(route.ID);
         }
         return ids;
      }

      private List<int> RelayIds(string name)
      {
         var ids = new List<int>();
         IncomingRelays relays = _settings.IncomingRelays;
         for (int i = 0; i < relays.Count; i++)
         {
            IncomingRelay relay = relays[i];
            if (relay.Name == name)
               ids.Add(relay.ID);
         }
         return ids;
      }

      private List<int> BlackListIds(string host)
      {
         var ids = new List<int>();
         DNSBlackLists lists = _settings.AntiSpam.DNSBlackLists;
         for (int i = 0; i < lists.Count; i++)
         {
            DNSBlackList list = lists[i];
            if (list.DNSHost == host)
               ids.Add(list.ID);
         }
         return ids;
      }
   }
}
