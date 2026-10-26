// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiVirus
{
   /// <summary>
   ///    The Linux server takes the absolute path of clamd's Unix socket as
   ///    ClamAVHost (LinuxRegressionTests/AntiVirus/ClamdUnixSocket.cs). clamd on
   ///    Windows listens on TCP and this server reaches no Unix socket, so here a
   ///    path is refused with a sentence that names the setting - over COM and over
   ///    REST, and by the connection test, which takes a host of its own - and
   ///    nothing is stored. A host name beside it is the control: it is taken as
   ///    before. Windows only, and not linked into the Linux suite, where the same
   ///    path is the one that works. No clamd is needed.
   /// </summary>
   [TestFixture]
   public class ClamdSocketPathOnWindows : TestFixtureBase
   {
      private const string DebianSocket = "/var/run/clamav/clamd.ctl";
      private const string AdminPassword = "testar";

      // A port of this fixture's own, as every REST fixture has one.
      private static int RestPort = 10421;

      [Test]
      [Description("COM refuses ClamAVHost set to a socket path, naming the setting and the path, and stores nothing; a host name is taken.")]
      public void ComRefusesASocketPath()
      {
         hMailServer.AntiVirus antiVirus = _application.Settings.AntiVirus;
         string before = antiVirus.ClamAVHost;

         var refused = Assert.Throws<COMException>(() => antiVirus.ClamAVHost = DebianSocket);
         StringAssert.Contains("ClamAVHost is " + DebianSocket + ", the path of a Unix socket, which this server cannot reach on Windows", refused.Message);
         StringAssert.Contains("ClamAVPort", refused.Message);
         Assert.AreEqual(before, antiVirus.ClamAVHost, "A refused host must not have been stored.");
         Assert.AreEqual(before, _application.Settings.AntiVirus.ClamAVHost, "A refused host must not have been stored.");

         antiVirus.ClamAVHost = "clamd.socket-path.test";
         Assert.AreEqual("clamd.socket-path.test", _application.Settings.AntiVirus.ClamAVHost);
         antiVirus.ClamAVHost = before;
      }

      [Test]
      [Description("The connection test given a socket path says why it cannot be used here, naming ClamAVHost, and fails.")]
      public void TheConnectionTestRefusesASocketPath()
      {
         string text;
         Assert.IsFalse(_application.Settings.AntiVirus.TestClamAVScanner(DebianSocket, 3310, out text), text);
         StringAssert.Contains("ClamAVHost is " + DebianSocket + ", the path of a Unix socket", text);
         StringAssert.DoesNotContain("Unable to connect", text, "Nothing should have been tried.");
      }

      [Test]
      [Description("PUT /api/v1/settings/antivirus refuses clamav_host set to a socket path with 400 and the same sentence, and changes nothing; a host name is taken.")]
      public void RestRefusesASocketPath()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         try
         {
            hMailServer.AntiVirus antiVirus = _application.Settings.AntiVirus;
            string before = antiVirus.ClamAVHost;

            (int status, string body) refused = Rest("PUT", "/api/v1/settings/antivirus", "{\"clamav_host\":\"" + DebianSocket + "\"}");
            Assert.AreEqual(400, refused.status, refused.body);
            StringAssert.Contains("ClamAVHost is " + DebianSocket + ", the path of a Unix socket", refused.body);
            Assert.AreEqual(before, _application.Settings.AntiVirus.ClamAVHost);

            (int status, string body) taken = Rest("PUT", "/api/v1/settings/antivirus", "{\"clamav_host\":\"clamd.socket-path.test\"}");
            Assert.AreEqual(200, taken.status, taken.body);
            Assert.AreEqual("clamd.socket-path.test", _application.Settings.AntiVirus.ClamAVHost);
            _application.Settings.AntiVirus.ClamAVHost = before;
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      private static (int status, string body) Rest(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
