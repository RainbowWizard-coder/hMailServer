// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    TCPIPPort.SMTPAuthentication over COM (GitLab work item #54): what the
   ///    Control Panel's port dialog reads and writes, the values it refuses, the
   ///    protocol it is refused on, and what TCPIPPorts.SetDefault puts back. The
   ///    behaviour of a listener is held over REST and SMTP by
   ///    SmtpAuthOnDeliveryPorts, which both suites run; this is the COM surface
   ///    alone, so it is not linked into the Linux suite.
   ///
   ///    The member is reached through dynamic, as AutoBanNeverBanNamesCom reaches
   ///    its own: the suite's interop is generated from the registered type
   ///    library, and naming it early-bound would not compile against a bench whose
   ///    server predates it. Nothing here reinitialises, so the listeners the rest
   ///    of the suite uses never take the values written.
   /// </summary>
   [TestFixture]
   public class SmtpAuthOnDeliveryPortsCom : TestFixtureBase
   {
      [TearDown]
      public void PutThePortsBack()
      {
         _settings.TCPIPPorts.SetDefault();
      }

      [Test]
      [Description("Port 25 reads 0 (On) after SetDefault; 2 (Honeypot) and 1 (Off) are saved and read back from the database; 3 is refused by the property, and Off or Honeypot by Save on an IMAP port. Negative control: 0 on the IMAP port is saved.")]
      public void TheModeIsReadWrittenAndRefusedOverCom()
      {
         dynamic port25 = FindPort(25);
         ClassicAssert.AreEqual(0, (int) port25.SMTPAuthentication, "SetDefault leaves port 25 On.");

         port25.SMTPAuthentication = 2;
         port25.Save();
         ClassicAssert.AreEqual(2, (int) FindPort(25).SMTPAuthentication, "Honeypot was not saved.");

         dynamic again = FindPort(25);
         again.SMTPAuthentication = 1;
         again.Save();
         ClassicAssert.AreEqual(1, (int) FindPort(25).SMTPAuthentication, "Off was not saved.");

         dynamic bogus = FindPort(25);
         var refused = Assert.Throws<COMException>(() => bogus.SMTPAuthentication = 3);
         StringAssert.Contains("Invalid SMTP authentication", refused.Message);
         ClassicAssert.AreEqual(1, (int) FindPort(25).SMTPAuthentication, "A refused value changed the port.");

         dynamic imap = FindPort(143);
         imap.SMTPAuthentication = 2;
         var notSmtp = Assert.Throws<COMException>(() => imap.Save());
         StringAssert.Contains("SMTP authentication can be Off or Honeypot on an SMTP port only.", notSmtp.Message);

         dynamic imapOn = FindPort(143);
         imapOn.SMTPAuthentication = 0;
         imapOn.Save();
         ClassicAssert.AreEqual(0, (int) FindPort(143).SMTPAuthentication, "Negative control: On on an IMAP port was not saved.");
      }

      [Test]
      [Description("TCPIPPorts.SetDefault puts both SMTP ports back On: the defaults are the listeners a create script makes, and restoring them never arms a ban. Negative control: before it, port 25 reads Honeypot.")]
      public void RestoringTheDefaultsPutsAuthBackOn()
      {
         dynamic port25 = FindPort(25);
         port25.SMTPAuthentication = 2;
         port25.Save();
         ClassicAssert.AreEqual(2, (int) FindPort(25).SMTPAuthentication, "Negative control: Honeypot was not saved.");

         _settings.TCPIPPorts.SetDefault();

         ClassicAssert.AreEqual(0, (int) FindPort(25).SMTPAuthentication, "SetDefault left port 25 a honeypot.");
         ClassicAssert.AreEqual(0, (int) FindPort(587).SMTPAuthentication);
      }

      /// <summary>The port with that number, read afresh from the database.</summary>
      private dynamic FindPort(int number)
      {
         dynamic ports = _settings.TCPIPPorts;
         for (int i = 0; i < (int) ports.Count; i++)
         {
            dynamic port = ports[i];
            if ((int) port.PortNumber == number)
               return port;
         }

         Assert.Fail("No port " + number + " after SetDefault.");
         return null;
      }
   }
}
