// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Which account a name given at sign-in names: the one rule every sign-in path
// that looks an account up by a name the client gave applies, and the one key the
// per-name lockout and the compromise hold count and ask under.
//
// The rule. The name is resolved as every sign-in resolves it - domain aliases,
// then the default domain - and the account found under it, by the cache or by
// the database, is the account only if its stored address IS that name, compared
// in lower case as String::ToLower lowers. Nothing else of the database's own
// comparison is taken. Up to 6.3.7 the account was whichever one the database's
// comparison found, and the databases find an account under spellings the
// lockout's key counted apart - MySQL and MariaDB an accented letter for a plain
// one; SQL Server and SQL Server Compact full-width letters, a NUL and characters
// their comparison ignores - and under padding the key trims. So each such
// spelling had a lockout threshold of its own, and an account the compromise
// detection held could be signed into under another spelling of its name with its
// password. A name the rule refuses is no account: the
// sign-in fails as it fails for a name that does not exist, with the same reply
// and after the same work (one lookup, no password verification), and the
// failure is counted as any failure is.
//
// The key. Key is Canonical of the name trimmed, which is what
// AccountLockout::NormalizeKey_ always computed; it is computed here, by the same
// function the rule compares with, so that the two can never disagree: whatever
// name the rule takes for an account has the key of the account's own address, so
// the lockout and the hold asked of the name are asked of the account. A name with
// spaces around it, which the rule refuses, still counts under the account's key.
//
// What is folded is what String::ToLower folds, in the C locale the server runs in
// (it never sets another): A to Z on Windows and with glibc, more with musl. A name
// that differs from the stored address in the case of a letter the C library does
// not lower is therefore refused, where MySQL or PostgreSQL would have taken it; it
// was always counted apart by the lockout.
//
// A lookup that is not a sign-in - an administrator's route, a delivery - keeps
// the database's comparison: there the account found is the one acted on, and a
// check that found none where the action finds one would allow what it refuses.

#pragma once

#include <functional>

namespace HM
{
   class Account;
   class Domain;

   class LogonName
   {
   public:
      // The name in lower case, as String::ToLower lowers.
      static String Fold(const String &text);

      // The name as a sign-in resolves it before it looks the account up: domain
      // aliases, then the default domain, nothing else changed. When the caches
      // cannot be read the name comes back as it was.
      static String Resolve(const String &name);

      // Fold(Resolve(Fold(name))): what the rule compares and the key is made of.
      static String Canonical(const String &name);

      // The per-name lockout's and the compromise hold's key: Canonical of the
      // name trimmed. Empty for a name of spaces alone.
      static String Key(const String &name);

      // The rule: true when the account stored under storedAddress is the one
      // name names - Canonical(name) is Key(storedAddress), character for
      // character over the whole string, and name holds no NUL.
      static bool Names(const String &name, const String &storedAddress);

      // The account a sign-in names, or none: the account the cache or the
      // database finds under Resolve(name), when Names(name, its address).
      static std::shared_ptr<const Account> FindAccount(const String &name);

      // The domain of the account a sign-in names, or none: the domain found
      // under the domain part of Resolve(name), when it is the account's own
      // domain and its name is that part, folded.
      static std::shared_ptr<const Domain> FindDomain(const String &name, const std::shared_ptr<const Account> &account);

      // The pure forms, with the aliases and the default domain given rather than
      // read from the caches and the configuration: what the four above compute.
      typedef std::function<String(const String &)> AliasFunction;
      static String CanonicalWith(const String &name, const AliasFunction &aliases, const String &defaultDomain);
      static String KeyWith(const String &name, const AliasFunction &aliases, const String &defaultDomain);
      static bool NamesWith(const String &name, const String &storedAddress, const AliasFunction &aliases, const String &defaultDomain);

   private:
      static String ResolveWith_(const String &name, const AliasFunction &aliases, const String &defaultDomain);
      static bool Same_(const String &left, const String &right);
      static bool HoldsNul_(const String &text);
   };

   // The rule and the key, without a server: for a stored address, its own
   // spelling, other case, the default domain and a domain alias are taken, and
   // every spelling the four databases were measured to find the account under -
   // accented, dotted and dotless letters, full-width letters, the characters SQL
   // Server's and SQL Server Compact's comparison ignores, inside and at the end,
   // a NUL at the end, inside and before more text, spaces and tabs before and
   // after - is refused; and whatever the rule takes has the key of the stored
   // address. Run by ClassTester on Windows and by sign-in-budget-tests on Linux.
   // Every check runs; a std::logic_error at the end names every one that did not
   // hold.
   class LogonNameTester
   {
   public:
      void Test();
   };
}
