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
// A name that holds a NUL (U+0000) is no account's either, and is refused before
// any lookup is made with it: no stored address holds one, MySQL, MariaDB and
// PostgreSQL fail a statement that carries one (and the server reported that as a
// database error), and SQL Server and SQL Server Compact read the bound value only
// as far as it. Refusing it early tells nothing about any account.
//
// The key. Key is Canonical of the name trimmed, which is what
// AccountLockout::NormalizeKey_ always computed; it is computed here, by the same
// function the rule compares with, so that the two can never disagree. The rule
// refuses a name with white space at either end, so for every name it takes the
// key and the compared form are made of the same string: whatever name the rule
// takes for an account has the key of the account's own address, whatever the
// domain alias table holds, and the lockout and the hold asked of the name are
// asked of the account. A name with spaces around it, which the rule refuses and
// for which no password is verified, still counts under the account's key, unless
// a domain alias's own stored name has white space at its end (which the server
// no longer saves): there the trimmed name is the alias's name without it, which
// is no account's, and such a name is counted as a name of no account is.
//
// What is folded is what String::ToLower folds, in the C locale the server runs in
// (it never sets another), and that differs by platform. With glibc it is A to Z
// alone. On Windows (the C runtime's classic locale, which lowers by the system's
// simple lower-case mapping, one UTF-16 unit at a time) it is A to Z and most
// letters from U+0100 up that have a lower case - Latin Extended, Greek, Cyrillic
// and full-width letters among them - but not Latin-1's own capitals, U+00C0 to
// U+00DE, and not every letter that has a lower case: U+0130, the Kelvin, Ohm and
// Angstrom signs, U+1E9E, title-case letters such as U+01C5, letters added in
// recent versions of Unicode and any letter outside the Basic Multilingual Plane
// are among those it leaves as they are. With musl it is nearly every letter that
// has a lower case, U+0130 and the Kelvin sign (to i and k) among them, so there a
// spelling with one of those for an i or a k names the account, and is counted
// under the account's own key. A name that differs from the stored address in
// the case of a letter the platform does not lower is therefore refused, where the
// database would have taken it; it was always counted apart by the lockout. The
// rule and the key are made by this one function on every platform, so they agree
// wherever it folds differently.
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
      // character over the whole string, name holds no NUL and has no white
      // space at either end (so that Key(name) is Canonical(name)).
      static bool Names(const String &name, const String &storedAddress);

      // The account a sign-in names, or none: the account the cache or the
      // database finds under Resolve(name), when Names(name, its address). None,
      // with no lookup made, for a name that holds a NUL.
      static std::shared_ptr<const Account> FindAccount(const String &name);

      // The domain of the account a sign-in names, or none: the domain found
      // under the domain part of Resolve(name), when it is the account's own
      // domain and its name is that part, folded. None, with no lookup made, for
      // a name that holds a NUL.
      static std::shared_ptr<const Domain> FindDomain(const String &name, const std::shared_ptr<const Account> &account);

      // The rule for a Kerberos principal and an account an administrator linked
      // to a directory user: true when the account's directory user name is the
      // principal's user part and its directory domain is the principal's realm
      // or the realm's first label (its NetBIOS name), each folded and compared
      // over the whole string, and none of them holds a NUL. The database's
      // comparison only proposes the account; this decides.
      static bool NamesDirectoryUser(const String &principalUser, const String &principalRealm,
                                     const String &directoryUser, const String &directoryDomain);

      // True when text holds a NUL (U+0000) anywhere. Such a name is no account's,
      // and every sign-in path refuses it before it looks anything up with it.
      static bool HoldsNul(const String &text);

      // The two strings are the same, character for character over the whole of
      // both: String's own comparisons read them as C strings, which end at the
      // first NUL.
      static bool Same(const String &left, const String &right);

      // The pure forms, with the aliases and the default domain given rather than
      // read from the caches and the configuration: what the four above compute.
      typedef std::function<String(const String &)> AliasFunction;
      static String CanonicalWith(const String &name, const AliasFunction &aliases, const String &defaultDomain);
      static String KeyWith(const String &name, const AliasFunction &aliases, const String &defaultDomain);
      static bool NamesWith(const String &name, const String &storedAddress, const AliasFunction &aliases, const String &defaultDomain);

   private:
      static String ResolveWith_(const String &name, const AliasFunction &aliases, const String &defaultDomain);

      // True when the name has white space at either end, as Trim takes it off.
      static bool Padded_(const String &name);
   };

   // The rule and the key, without a server: for a stored address, its own
   // spelling, other case, the default domain and a domain alias are taken, and
   // every spelling the four databases were measured to find the account under -
   // accented, dotted and dotless letters, full-width letters, the characters SQL
   // Server's and SQL Server Compact's comparison ignores, inside and at the end,
   // a NUL at the end, inside and before more text, spaces and tabs before and
   // after - is refused; and whatever the rule takes has the key of the stored
   // address, under an alias whose stored name ends in white space as well, where
   // the padded name is refused. And the directory user rule: a principal whose user part and realm
   // are an account's directory user name and domain, in other case and with the
   // NetBIOS name, is taken, and the same spellings of either are refused. Run by
   // ClassTester on Windows and by sign-in-budget-tests on Linux.
   // Every check runs; a std::logic_error at the end names every one that did not
   // hold.
   class LogonNameTester
   {
   public:
      void Test();
   };
}
