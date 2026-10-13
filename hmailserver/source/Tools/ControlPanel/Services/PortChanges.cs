// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The TCP/IP ports page's two rules of a change (review finding F222).
   ///
   /// A port the page adds, edits, deletes or restores is not what the server listens
   /// on until the server is reinitialised, and the page asked "Reinitialise the server
   /// now?" in a dialog after every one of them. It says so in a notice above the list
   /// instead, with Reinitialise now, from the first change in a session until a
   /// reinitialise from this Control Panel - the page's own, or the pending bar's Apply
   /// now - or a restart of the service; Restore defaults, which replaces every port,
   /// still asks. The changes belong to the session they were made in: a sign-in to
   /// another server does not show the first one's.
   ///
   /// And a port with SSL/TLS or STARTTLS needs the certificate it presents: the add
   /// form had no certificate field, so a TLS port it added had none and did not
   /// start, and the port's dialog saved one with "(none)".
   ///
   /// No WPF and no COM here, so the tests hold the rules and the words.
   /// </summary>
   public static class PortChanges
   {
      // The session whose ports were changed since its last reinitialise, by identity.
      private static object changedIn_;

      /// <summary>A port of <paramref name="session"/> was added, edited, deleted or restored.</summary>
      public static void Changed(object session)
      {
         if (session != null)
            changedIn_ = session;
      }

      /// <summary><paramref name="session"/>'s server was reinitialised or restarted, and listens on its ports as they are.</summary>
      public static void Applied(object session)
      {
         if (session != null && ReferenceEquals(changedIn_, session))
            changedIn_ = null;
      }

      /// <summary>Whether <paramref name="session"/>'s ports changed since its server last read them.</summary>
      public static bool ArePending(object session) => session != null && ReferenceEquals(changedIn_, session);

      /// <summary>The notice above the list while changes wait.</summary>
      public static string PendingNotice => L("Ports changed - the server goes on listening on the old set until it is reinitialised.");

      /// <summary>
      /// Why a port cannot be saved with the certificate chosen, said under the
      /// certificate's box, or null: a port with connection security (anything but
      /// None, 0) needs one, and there may be none on the server to choose.
      /// </summary>
      public static string CertificateProblem(int security, int certificateId, int certificatesOnServer)
      {
         if (security == 0 || certificateId > 0)
            return null;
         return certificatesOnServer > 0
            ? L("Choose the certificate this port presents.")
            : L("A port with SSL/TLS or STARTTLS presents a certificate, and the server has none yet: add one on the TLS certificates page first.");
      }
   }
}
