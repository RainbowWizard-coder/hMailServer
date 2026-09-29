// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Threading;
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
   }
}
