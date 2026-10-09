// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The question the Directory authentication page asks before it saves the [LDAP]
   /// section switched on while what the server needs is missing (review finding F130).
   ///
   /// The page saved such a section without a word, though the card above it said what
   /// was missing, and the server reads the section again within two seconds of a save.
   /// What it then does is LdapDirectoryAuthenticator::ValidateUser's: with Enabled on
   /// and LdapConfiguration::IsComplete false it attempts no directory sign-in at all,
   /// reports error 5922 once a minute, and goes on validating directory-linked accounts
   /// through Windows logon (LogonUser), which works only on a computer joined to the
   /// domain - the usual reason the page is being filled in is that this one is not.
   /// The question names the fields by their captions on the page, one whole sentence
   /// for each case, and the page asks it with Save anyway and Cancel, Cancel the
   /// answer Enter gives. Held by LdapIncompleteSaveTests.
   /// </summary>
   public static class LdapIncompleteSave
   {
      /// <summary>
      /// Whether a save asks first: directory authentication switched on, and the host
      /// name missing, or the search base missing where the sign-in searches for the
      /// user - a simple bind with no user DN template (LdapConfiguration::UsesSearch).
      /// </summary>
      public static bool AsksFirst(bool enabled, bool serverMissing, bool searchBaseMissing)
         => enabled && (serverMissing || searchBaseMissing);

      /// <summary>The question, for what is missing; null when nothing is.</summary>
      public static string Question(bool serverMissing, bool searchBaseMissing)
      {
         if (serverMissing && searchBaseMissing)
            return L("Directory authentication is switched on here, but Directory server host name and Search base are empty. Until both are set, the server tries no directory sign-in, and directory-linked accounts go on signing in through Windows logon, which works only on a computer joined to the domain.\n\nSave it anyway?");

         if (serverMissing)
            return L("Directory authentication is switched on here, but Directory server host name is empty. Until it is set, the server tries no directory sign-in, and directory-linked accounts go on signing in through Windows logon, which works only on a computer joined to the domain.\n\nSave it anyway?");

         if (searchBaseMissing)
            return L("Directory authentication is switched on here, but Search base is empty, and a simple bind without a user DN template searches under it. Until it is set, the server tries no directory sign-in, and directory-linked accounts go on signing in through Windows logon, which works only on a computer joined to the domain.\n\nSave it anyway?");

         return null;
      }
   }
}
