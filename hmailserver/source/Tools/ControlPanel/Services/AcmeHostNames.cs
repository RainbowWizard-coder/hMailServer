// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether AcmeDomains names a host, read as the server reads it
   /// (AcmeClient::RequestCertificate splits it at commas and trims each name), with
   /// no WPF so that AcmeHostNamesTests holds it. With none, the server logs "ACME: No
   /// domains configured" and asks the CA for nothing, so the Certificates (ACME)
   /// page says that before anything about the network (review finding F237).
   /// </summary>
   public static class AcmeHostNames
   {
      /// <summary>True when the list holds at least one name that is not blank.</summary>
      public static bool Any(string domains)
         => (domains ?? "").Split(',').Any(name => name.Trim().Length > 0);
   }
}
