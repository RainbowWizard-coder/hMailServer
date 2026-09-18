// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Account;

   // What an attempt offers in place of the one-time code an account with a
   // second factor needs, and what came of it.
   //
   // It exists so that the recovery code is checked INSIDE
   // PasswordValidator::ValidatePassword rather than by the caller ahead of it.
   // That is not tidiness: AccountLogon::Logon deliberately puts the per-name
   // lockout and the domain country policy in front of every password
   // verification, and a caller that verified the password itself in order to
   // decide whether to look at a code would be in front of both - a locked name
   // would still cost an Argon2id verification a request, and the ten
   // comparisons a matching password then costs would be a timing oracle for
   // precisely the accounts a second factor is there to protect.
   struct SecondFactorOffer
   {
      // The caller has already verified a one-time code for this account. Only
      // the COM authentication path can: a mail client has nowhere to type one.
      bool satisfied = false;

      // One of the account's recovery codes, offered instead (AccountRecovery.h).
      // Empty for every attempt that offers none.
      String recovery_code;

      // Out: the row of the recovery code that matched, for the caller to spend
      // once the WHOLE attempt has been accepted. Nothing is spent in here, so a
      // refusal anywhere after this burns nothing.
      __int64 matched_recovery_code = 0;
   };

   class PasswordValidator
   {
   public:
      PasswordValidator(void);
      ~PasswordValidator(void);

      static std::shared_ptr<const Account> ValidatePassword(const String &sUsername, const String &sPassword);

      // Resolves a login name to an account WITHOUT checking any credential - domain
      // aliases and the default domain applied exactly as the validation paths apply
      // them, and the same active-account and active-domain rules. Exists for the one
      // caller that must know something about the account before it can validate: the
      // COM path needs the TOTP secret in order to check the code that decides whether
      // the password may be tried at all.
      static std::shared_ptr<const Account> LookupAccount(const String &sUsername);
      // secondFactor is passed straight to the overload below, which is where it
      // decides anything: the account's own password is refused for an account
      // with a second factor enrolled unless one has been proved.
      static std::shared_ptr<const Account> ValidatePassword(const String &sMasqname, const String &sUsername, const String &sPassword,
                                                             bool *matchedAppPassword = nullptr,
                                                             String *matchedAppPasswordName = nullptr,
                                                             SecondFactorOffer *secondFactor = nullptr);

      // The account's own password and nothing else: never an app password,
      // whatever credential the request that carries it came in on. For a
      // route that has to know the person is the account holder rather than
      // a program the holder once gave a password to - making an app
      // password, above all. A directory-linked account is asked of the
      // directory, as at logon. No script hook, no expiry check: this is a
      // proof, not a logon.
      static bool ValidateAccountPasswordOnly(std::shared_ptr<const Account> pAccount, const String &sPassword);

      // Validates the user password. Return the account if validation is OK. 

      // secondFactor, when given, says what this attempt offers in place of the
      // one-time code an enrolled account needs: a code the CALLER has already
      // verified (only the COM authentication path can - a mail client has
      // nowhere to type one), or one of the account's recovery codes, which is
      // checked here. It defaults to nullptr and every protocol leaves it alone:
      // an account with a second factor enrolled authenticates in a mail client
      // through an app password and not through its own password.
      //
      // A recovery code is compared only AFTER the account's own password has
      // matched, because each comparison is a deliberately expensive hash and a
      // code is compared against every unused code the account has - checking
      // the code first would let anybody who can name a mailbox spend ten of
      // them a request. Nothing is spent here; the row that matched is reported
      // in secondFactor->matched_recovery_code for the caller to spend once the
      // whole attempt has been accepted.
      //
      // matchedAppPassword, when given, is set to whether it was one of the
      // account's app passwords that matched rather than the account's own -
      // a caller that must treat the two differently learns it here, from
      // the one comparison, instead of paying for a second.
      //
      // matchedAppPasswordName, when given, is that app password's NAME - the
      // label its owner gave it. Only the name: never the hash and never
      // anything derived from the secret. It exists so that the record of a
      // sign-in can say which of the account's credentials a client is using,
      // which is the question somebody looking at a client they do not
      // recognise actually has.
      static bool ValidatePassword(std::shared_ptr<const Account> pAccount, const String &sPassword,
                                   SecondFactorOffer *secondFactor = nullptr, bool *matchedAppPassword = nullptr,
                                   String *matchedAppPasswordName = nullptr);

      // Validates the user password. Return true if the password is correct.

   private:

      // The account's own password: the Active Directory path, the stored-hash
      // comparison, the opportunistic re-hash and the minimum-hash policy. Split out
      // of ValidatePassword so that a failure here can fall through to an app
      // password rather than being the end of the attempt.
      static bool ValidateAccountPassword_(std::shared_ptr<const Account> pAccount, const String &sPassword);

      // Any of the account's app passwords. Only ever called after the account's own
      // password has failed, and it returns false immediately - without a database
      // query - on a server where none has been issued.
      static bool ValidateAppPassword_(std::shared_ptr<const Account> pAccount, const String &sPassword,
                                       String *matchedName = nullptr);

      // Throttles the diagnostic for a stored app password whose hash scheme this
      // server could not have written. True the first time each row is seen.
      static bool ReportUnusableAppPasswordOnce_(__int64 appPasswordID);

      // Brings the account's stored password up to the configured preferred hash
      // scheme. Must only ever be called with a password that has just been verified
      // against the account: that is the one moment the clear text is available, and
      // the only moment a write is justified.
      //
      // Returns the encryption type the stored password uses after the attempt - the
      // type it already had if nothing needed doing or if the upgrade failed.
      static int UpgradeStoredPasswordHash_(std::shared_ptr<const Account> pAccount, const String &sPassword, int currentEncryption);
   };
}