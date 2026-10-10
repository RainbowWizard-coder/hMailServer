// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Where the Control Deck is, and why it cannot be opened when it cannot (review
   /// finding F281). Twenty-nine texts sent the reader to "the Control Deck" - a
   /// case is opened there, alert rules and webhook subscriptions are set there,
   /// trap addresses declared and held messages reviewed there - with no button and
   /// no address; one said what it is. The Deck is the browser administration page
   /// the REST listener serves at its root (RestApiServer, GET /), so its address is
   /// the listener's, by the rules <see cref="RestSettings.Endpoint"/> states.
   ///
   /// It has no address for one of its views: its only fragment is the single
   /// sign-on ticket, so every button opens the Deck itself (the owner's decision
   /// of 9 October 2026: deep links where it has them).
   /// </summary>
   public static class ControlDeck
   {
      /// <summary>
      /// The listener's settings, which the server takes when it applies them - a
      /// reinitialise or a restart (IniSettingsBaseline.h: AppliesReinitialize) - and
      /// not as they are saved: until then hMailServer.ini holds an address nothing
      /// listens on.
      /// </summary>
      public static readonly IReadOnlyList<string> ListenerSettings = new[]
      {
         "RestApiPort", "RestApiBindAddress", "RestApiCertificateFile", "RestApiPrivateKeyFile" // no-loc
      };

      /// <summary>
      /// The Deck's address from the listener's four settings - and, on the server,
      /// whether the ACME certificate it falls back to is there - or null with the
      /// reason in <paramref name="whyNot"/>, as one sentence a disabled button can
      /// carry: the REST API is off; it listens on the loopback address and this
      /// Control Panel is on another machine; or it cannot listen beyond this machine
      /// without a certificate.
      /// </summary>
      public static Uri Address(string port, string bindAddress, string certificateFile, string privateKeyFile,
         bool? acmeCertificate, bool localSession, string host, out string whyNot)
         => Address(port, bindAddress, certificateFile, privateKeyFile, acmeCertificate, localSession, host, null, null, out whyNot);

      /// <summary>
      /// <see cref="Address(string, string, string, string, bool?, bool, string, out string)"/>,
      /// knowing which settings the server has saved and not yet applied
      /// (<paramref name="pending"/>, IniFeatureStore.PendingNames; null where the
      /// server cannot say) and, for a page that holds the port, what to say there
      /// when the REST API is off (<paramref name="whyOffHere"/>, null for the
      /// sentence that names the API &amp; monitoring page).
      ///
      /// While one of <see cref="ListenerSettings"/> waits to be applied, and the port
      /// saved is not 0, the Deck is not where the file says - the REST API turned on
      /// and saved listens nowhere yet, a port changed and saved still listens on the
      /// old one - so the button is disabled until the server has applied them, where
      /// it opened an address nothing listened on (found re-checking eeff3c210). A
      /// port saved as 0 is the REST API turned off, which the button says as it is.
      /// </summary>
      public static Uri Address(string port, string bindAddress, string certificateFile, string privateKeyFile,
         bool? acmeCertificate, bool localSession, string host, IReadOnlyCollection<string> pending, string whyOffHere,
         out string whyNot)
      {
         string portText = (port ?? "").Trim();
         bool off = !int.TryParse(portText, NumberStyles.Integer, CultureInfo.InvariantCulture, out int number) || number <= 0 || number > 65535;

         if (!off && pending != null && ListenerSettings.Any(name => pending.Contains(name, StringComparer.OrdinalIgnoreCase)))
         {
            whyNot = L("The REST API's new settings are saved but not yet in effect - the Control Deck can be opened once the server applies them.");
            return null;
         }

         Uri root = RestSettings.Endpoint(port, bindAddress, certificateFile, privateKeyFile, acmeCertificate, localSession, host, out _);
         if (root != null)
         {
            whyNot = null;
            return root;
         }

         string bind = string.IsNullOrWhiteSpace(bindAddress) ? "127.0.0.1" : bindAddress.Trim(); // no-loc
         bool loopback = bind == "127.0.0.1" || bind == "::1" || bind.Equals("localhost", StringComparison.OrdinalIgnoreCase); // no-loc

         if (off)
            whyNot = whyOffHere ?? L("The Control Deck is served by the REST API, which is off - give it a port on the API & monitoring page.");
         else if (loopback && !localSession)
            whyNot = F("The REST API listens on {0} only, so the Control Deck opens only in a browser on the server itself.", bind);
         else
            whyNot = F("The REST API is not running: listening on {0} needs a certificate, set on the API & monitoring page.", bind);

         return null;
      }
   }
}
