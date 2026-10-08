// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The password pepper, and what a change of it does to the passwords already
   /// stored.
   ///
   /// The pepper was an ordinary secret among the hashing fields of the
   /// Authentication page, its caption shouting that changing it "invalidates ALL
   /// existing passwords", and Save wrote whatever was typed into it (review finding
   /// F169). The server mixes it into Argon2id and scrypt hashes only (Crypt.cpp,
   /// ApplyPepper_): PBKDF2 - the default, whose hash is also SCRAM's salted
   /// password - SHA-256 and MD5 hashes and the administrator password do not use
   /// it, so a new pepper stops exactly the passwords stored as Argon2id or scrypt
   /// hashes from working, and a pepper set for the first time stops those too. The
   /// page now says so in a warning that stays under the field, and asks before a
   /// save writes a new pepper on a server that has accounts, with Cancel the answer
   /// Enter gives. Held by PepperChangeTests.
   /// </summary>
   public static class PepperChange
   {
      /// <summary>The setting's key in hMailServer.ini's [Settings].</summary>
      public const string Key = "PasswordPepper"; // no-loc: an INI key

      /// <summary>The warning that stays under the field.</summary>
      public static string Warning
         => L("Changing the pepper after accounts exist stops every password stored as an Argon2id or scrypt hash from working, and each such account then needs a new password. PBKDF2 hashes, the default, are not peppered.");

      /// <summary>
      /// Whether a save asks first: a value was typed that is not the one stored,
      /// on a server that has accounts - or whose accounts could not be counted,
      /// which is asked about as if it had some.
      /// </summary>
      /// <param name="typed">What is in the box; empty keeps the stored pepper, and writes nothing.</param>
      /// <param name="stored">The pepper hMailServer.ini holds now; empty when none is set.</param>
      /// <param name="accounts">How many accounts the server has; null when they could not be counted.</param>
      public static bool AsksFirst(string typed, string stored, int? accounts)
         => !string.IsNullOrEmpty(typed)
            && !string.Equals(typed, stored ?? "", StringComparison.Ordinal)
            && (accounts == null || accounts.Value > 0);

      /// <summary>The question, with the server's number of accounts when it could be counted.</summary>
      public static string Question(int? accounts)
         => accounts == null
            ? L("Changing the pepper stops every mailbox password stored as an Argon2id or scrypt hash from working, and each such account will need a new password.")
            : F("Changing the pepper stops every mailbox password stored as an Argon2id or scrypt hash from working, and each such account will need a new password. Accounts on this server: {0}.", accounts.Value);
   }
}
