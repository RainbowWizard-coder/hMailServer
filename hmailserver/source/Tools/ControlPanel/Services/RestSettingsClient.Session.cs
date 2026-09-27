// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The half of RestSettingsClient that ties it to the server this Control Panel is
   /// signed in to: one client per session, the credential it signs in with, and
   /// where that server's listener is, from its own settings.
   /// </summary>
   internal sealed partial class RestSettingsClient
   {
      private static readonly object gate_ = new();
      private static RestSettingsClient current_;

      private ServerSession session_;

      private RestSettingsClient(ServerSession session)
         : this(() => session?.RestAuthorization(), () => Find_(session))
      {
         session_ = session;
      }

      /// <summary>The client for the server this Control Panel is signed in to.</summary>
      public static RestSettingsClient For(ServerSession session)
      {
         lock (gate_)
         {
            if (current_ == null || !ReferenceEquals(current_.session_, session))
               current_ = new RestSettingsClient(session);
            return current_;
         }
      }

      /// <summary>
      /// The listener, from its four settings and - on the server itself - whether the
      /// ACME certificate it falls back to is there, as the Discovery page finds it;
      /// and, there, the certificate file it serves, which the certificate it presents
      /// is held to (RestSettings.TrustsCertificate).
      /// </summary>
      private static Listener Find_(ServerSession session)
      {
         var ini = new IniFeatureStore();
         string certificateFile = ini.Read("RestApiCertificateFile", ""); // no-loc

         bool? acmeCertificate = null;
         string acmeFolder = null;
         if (ServerSession.IsLocalSession)
         {
            string dataFolder = ini.IniPath == null ? "" : ProfileApi.ReadString("Directories", "DataFolder", "", ini.IniPath, 4096); // no-loc
            acmeFolder = DiscoveryReport.AcmeCertificateFolder(ini.Read("AcmeCertificateDirectory", ""), dataFolder); // no-loc
            acmeCertificate = DiscoveryReport.AcmeCertificateIn(acmeFolder, System.IO.File.Exists);
         }

         System.Uri endpoint = RestSettings.Endpoint(
            ini.Read("RestApiPort", "0"), ini.Read("RestApiBindAddress", "127.0.0.1"), // no-loc
            certificateFile, ini.Read("RestApiPrivateKeyFile", ""), // no-loc
            acmeCertificate, ServerSession.IsLocalSession, session?.Host, out string refusal);

         string served = ServerSession.IsLocalSession
            ? RestSettings.ServedCertificateFile(certificateFile, acmeFolder, acmeCertificate)
            : null;

         return new Listener(endpoint, refusal, served);
      }
   }
}
