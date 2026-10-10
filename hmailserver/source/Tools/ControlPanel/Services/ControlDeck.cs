// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
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
      /// The Deck's address from the listener's four settings - and, on the server,
      /// whether the ACME certificate it falls back to is there - or null with the
      /// reason in <paramref name="whyNot"/>, as one sentence a disabled button can
      /// carry: the REST API is off; it listens on the loopback address and this
      /// Control Panel is on another machine; or it cannot listen beyond this machine
      /// without a certificate.
      /// </summary>
      public static Uri Address(string port, string bindAddress, string certificateFile, string privateKeyFile,
         bool? acmeCertificate, bool localSession, string host, out string whyNot)
      {
         Uri root = RestSettings.Endpoint(port, bindAddress, certificateFile, privateKeyFile, acmeCertificate, localSession, host, out _);
         if (root != null)
         {
            whyNot = null;
            return root;
         }

         string portText = (port ?? "").Trim();
         bool off = !int.TryParse(portText, NumberStyles.Integer, CultureInfo.InvariantCulture, out int number) || number <= 0 || number > 65535;
         string bind = string.IsNullOrWhiteSpace(bindAddress) ? "127.0.0.1" : bindAddress.Trim(); // no-loc
         bool loopback = bind == "127.0.0.1" || bind == "::1" || bind.Equals("localhost", StringComparison.OrdinalIgnoreCase); // no-loc

         if (off)
            whyNot = L("The Control Deck is served by the REST API, which is off - give it a port on the API & monitoring page.");
         else if (loopback && !localSession)
            whyNot = F("The REST API listens on {0} only, so the Control Deck opens only in a browser on the server itself.", bind);
         else
            whyNot = F("The REST API is not running: listening on {0} needs a certificate, set on the API & monitoring page.", bind);

         return null;
      }
   }
}
