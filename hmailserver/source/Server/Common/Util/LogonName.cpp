// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See LogonName.h.

#include "StdAfx.h"

#include "LogonName.h"

#include "../Application/ObjectCache.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/DomainAliases.h"
#include "../Cache/CacheContainer.h"

#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   String
   LogonName::Fold(const String &text)
   {
      String folded = text;
      folded.ToLower();
      return folded;
   }

   bool
   LogonName::Same(const String &left, const String &right)
   {
      // Over the whole of both strings: String's own comparisons read them as C
      // strings, which end at the first NUL.
      const std::basic_string<wchar_t> &a = left;
      const std::basic_string<wchar_t> &b = right;
      return a == b;
   }

   bool
   LogonName::HoldsNul(const String &text)
   {
      const std::basic_string<wchar_t> &whole = text;
      return whole.find(static_cast<wchar_t>(0)) != std::basic_string<wchar_t>::npos;
   }

   String
   LogonName::ResolveWith_(const String &name, const AliasFunction &aliases, const String &defaultDomain)
   {
      // Aliases first, then the default domain, as DefaultDomain::ApplyDefaultDomain
      // appends it: to a name with no @, and only when there is one.
      String resolved = aliases ? aliases(name) : name;

      if (!resolved.IsEmpty() && resolved.Find(_T("@")) < 0 && !defaultDomain.IsEmpty())
         resolved = resolved + _T("@") + defaultDomain;

      return resolved;
   }

   String
   LogonName::CanonicalWith(const String &name, const AliasFunction &aliases, const String &defaultDomain)
   {
      String canonical = Fold(name);

      if (canonical.IsEmpty())
         return canonical;

      return Fold(ResolveWith_(canonical, aliases, defaultDomain));
   }

   String
   LogonName::KeyWith(const String &name, const AliasFunction &aliases, const String &defaultDomain)
   {
      String trimmed = name;
      trimmed.Trim();
      return CanonicalWith(trimmed, aliases, defaultDomain);
   }

   bool
   LogonName::NamesWith(const String &name, const String &storedAddress, const AliasFunction &aliases, const String &defaultDomain)
   {
      // A NUL ends the name where String's comparisons read it - the domain alias
      // match among them, which would otherwise resolve "local@alias", a NUL and
      // anything after it to the account - and where SQL Server and SQL Server
      // Compact read a bound value. No stored address holds one.
      if (HoldsNul(name) || Padded_(name))
         return false;

      const String stored = KeyWith(storedAddress, aliases, defaultDomain);
      if (stored.IsEmpty())
         return false;

      return Same(CanonicalWith(name, aliases, defaultDomain), stored);
   }

   bool
   LogonName::Padded_(const String &name)
   {
      // White space at either end, as Trim takes it off. The key is the canonical
      // form of the name trimmed, and the rule compares the canonical form of the
      // name as given, so a name the trim changes is refused: then for every name
      // the rule takes the two are made of the same string, and its key is the
      // account's whatever the alias table holds. Without this a domain alias whose
      // stored name ends in white space - which an administrator could save - took
      // the name with that white space after it for the account while its key was
      // the alias's name without it.
      String trimmed = name;
      trimmed.Trim();
      return !Same(trimmed, name);
   }

   String
   LogonName::Resolve(const String &name)
   {
      try
      {
         std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();

         AliasFunction aliases;
         if (domainAliases)
            aliases = [domainAliases](const String &address) { return domainAliases->ApplyAliasesOnAddress(address); };

         return ResolveWith_(name, aliases, Configuration::Instance()->GetDefaultDomain());
      }
      catch (...)
      {
         // The caches or the configuration could not be read - very early at
         // start, or a failure inside the alias collection. The name as given is
         // the safe answer: a name that would have resolved to an account then
         // names none, and it never names another account than it would have.
         return name;
      }
   }

   String
   LogonName::Canonical(const String &name)
   {
      String canonical = Fold(name);

      if (canonical.IsEmpty())
         return canonical;

      return Fold(Resolve(canonical));
   }

   String
   LogonName::Key(const String &name)
   {
      String trimmed = name;
      trimmed.Trim();
      return Canonical(trimmed);
   }

   bool
   LogonName::Names(const String &name, const String &storedAddress)
   {
      if (HoldsNul(name) || Padded_(name))
         return false;

      const String stored = Key(storedAddress);
      if (stored.IsEmpty())
         return false;

      return Same(Canonical(name), stored);
   }

   std::shared_ptr<const Account>
   LogonName::FindAccount(const String &name)
   {
      // A name that holds a NUL is no account's, and no lookup is made with it:
      // MySQL, MariaDB and PostgreSQL fail a statement that carries one, and SQL
      // Server and SQL Server Compact read the bound value only as far as it.
      // Refusing it here tells nothing about any account.
      if (HoldsNul(name))
         return std::shared_ptr<const Account>();

      // Otherwise one lookup whatever the name, as for a name that does not
      // exist: a spelling the rule refuses costs what a wrong name costs, and is
      // answered as one.
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(Resolve(name));

      if (!account || !Names(name, account->GetAddress()))
         return std::shared_ptr<const Account>();

      return account;
   }

   std::shared_ptr<const Domain>
   LogonName::FindDomain(const String &name, const std::shared_ptr<const Account> &account)
   {
      if (!account || HoldsNul(name))
         return std::shared_ptr<const Domain>();

      const String domainName = StringParser::ExtractDomain(Resolve(name));
      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainName);

      if (!domain || domain->GetID() != account->GetDomainID())
         return std::shared_ptr<const Domain>();

      if (HoldsNul(domainName) || !Same(Fold(domainName), Fold(domain->GetName())))
         return std::shared_ptr<const Domain>();

      return domain;
   }

   bool
   LogonName::NamesDirectoryUser(const String &principalUser, const String &principalRealm,
                                 const String &directoryUser, const String &directoryDomain)
   {
      if (HoldsNul(principalUser) || HoldsNul(principalRealm) || HoldsNul(directoryUser) || HoldsNul(directoryDomain))
         return false;

      const String user = Fold(principalUser);
      if (user.IsEmpty() || !Same(Fold(directoryUser), user))
         return false;

      // The realm, or its first label, as the directory's NetBIOS name is
      // commonly the realm's first label (CORP for CORP.EXAMPLE.COM).
      const String realm = Fold(principalRealm);
      String netbios = realm;
      const int dot = netbios.Find(_T("."));
      if (dot > 0)
         netbios = netbios.Mid(0, dot);

      const String domain = Fold(directoryDomain);
      return Same(domain, realm) || Same(domain, netbios);
   }

   namespace
   {
      String Char(unsigned int code)
      {
         return String(1, static_cast<wchar_t>(code));
      }

      bool Equal(const String &left, const String &right)
      {
         const std::basic_string<wchar_t> &a = left;
         const std::basic_string<wchar_t> &b = right;
         return a == b;
      }

      std::string Narrow(const String &text)
      {
         // For the failure message only: each character outside ASCII as U+XXXX.
         std::string out;
         const std::basic_string<wchar_t> &whole = text;
         static const char digits[] = "0123456789ABCDEF";
         for (wchar_t c : whole)
         {
            if (c >= L' ' && c <= L'~')
            {
               out += static_cast<char>(c);
            }
            else
            {
               out += "<U+";
               for (int shift = 12; shift >= 0; shift -= 4)
                  out += digits[(c >> shift) & 0xF];
               out += ">";
            }
         }
         return out;
      }
   }

   void
   LogonNameTester::Test()
   {
      std::vector<std::string> failures;

      // The alias alias.example of example.test, matched as
      // DomainAliases::ApplyAliasesOnAddress matches it: the part after the last @,
      // with String::CompareNoCase, which reads a C string and so ends at a NUL.
      const LogonName::AliasFunction aliases = [](const String &address) -> String
      {
         const String domain = StringParser::ExtractDomain(address);
         if (domain.CompareNoCase(_T("alias.example")) == 0)
            return StringParser::ExtractAddress(address) + _T("@example.test");
         return address;
      };

      const String defaultDomain = _T("example.test");
      const String stored = _T("alice@example.test");

      auto names = [&](const String &name) { return LogonName::NamesWith(name, stored, aliases, defaultDomain); };
      auto key = [&](const String &name) { return LogonName::KeyWith(name, aliases, defaultDomain); };

      // Taken: the address itself, in other case, the bare local part under the
      // default domain, and under a domain alias.
      std::vector<String> taken =
      {
         _T("alice@example.test"), _T("ALICE@EXAMPLE.TEST"), _T("Alice@Example.Test"), _T("alice"), _T("ALICE"),
         _T("alice@alias.example"), _T("ALICE@Alias.Example")
      };

      for (const String &name : taken)
      {
         if (!names(name))
            failures.push_back("the name '" + Narrow(name) + "' is not taken for alice@example.test");
      }

      // Refused: every other spelling. Each the databases were measured to find
      // the account under is among them.
      std::vector<String> refused;

      // Accented letters for plain ones, which MySQL and MariaDB's
      // utf8mb4_general_ci takes; and in the domain.
      refused.push_back(Char(0xE0) + _T("lice@example.test"));
      refused.push_back(Char(0xC0) + _T("LICE@example.test"));
      refused.push_back(Char(0xE5) + _T("lice@example.test"));
      refused.push_back(_T("alic") + Char(0xE9) + _T("@example.test"));
      refused.push_back(_T("al") + Char(0xEF) + _T("ce@example.test"));
      refused.push_back(_T("ali") + Char(0xE7) + _T("e@example.test"));
      refused.push_back(_T("alice@ex") + Char(0xE0) + _T("mple.test"));
      refused.push_back(_T("alice@alias.ex") + Char(0xE0) + _T("mple"));

      // Full-width letters, which SQL Server's and SQL Server Compact's
      // comparison takes for the ASCII ones.
      refused.push_back(Char(0xFF41) + _T("lice@example.test"));
      refused.push_back(Char(0xFF21) + _T("LICE@example.test"));
      refused.push_back(_T("alice@exampl") + Char(0xFF45) + _T(".test"));

      // Characters SQL Server's and SQL Server Compact's comparison ignores, inside
      // the name and at its end, once and 75 times.
      const unsigned int ignored[] =
      {
         0x00AD, 0x034F, 0x061C, 0x17B4, 0x17B5, 0x180B, 0x180C, 0x180D, 0x180E,
         0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0x202A, 0x202B, 0x202C, 0x202D, 0x202E,
         0x2060, 0x2061, 0x2062, 0x2063, 0x2064, 0x206A, 0x206B, 0x206C, 0x206D, 0x206E, 0x206F,
         0xFE00, 0xFE0F, 0xFEFF, 0xFFF9, 0xFFFA, 0xFFFB, 0xFFFC, 0xFFFD
      };
      for (unsigned int code : ignored)
      {
         refused.push_back(_T("al") + Char(code) + _T("ice@example.test"));
         refused.push_back(_T("alice@example.test") + Char(code));
         refused.push_back(_T("alice@exam") + Char(code) + _T("ple.test"));
         refused.push_back(_T("alice@alias.example") + Char(code));
         refused.push_back(_T("ali") + Char(code) + _T("ce"));
      }
      {
         String many = _T("alice@example.test");
         for (int i = 0; i < 75; i++)
            many += Char(0xFEFF);
         refused.push_back(many);
      }

      // A NUL, which ends a bound value on SQL Server and a C string everywhere:
      // at the end, inside, before more text, and after an alias's name, where the
      // alias match reads only as far as the NUL.
      refused.push_back(_T("alice@example.test") + Char(0));
      refused.push_back(_T("ali") + Char(0) + _T("ce@example.test"));
      refused.push_back(_T("alice@example.test") + Char(0) + _T("junk"));
      refused.push_back(_T("alice@alias.example") + Char(0) + _T("junk"));
      refused.push_back(_T("alice") + Char(0));

      // Padding, which MySQL, MariaDB, SQL Server and SQL Server Compact take at
      // the end of a name; and spaces, tabs and a no-break space anywhere else.
      std::vector<String> padded =
      {
         _T(" alice@example.test"), _T("alice@example.test "), _T("\talice@example.test"), _T("alice@example.test\t"),
         _T("alice@example.test\r\n"), _T("alice "), _T(" alice"), _T("alice@alias.example ")
      };
      {
         String long_ = _T("alice@example.test");
         for (int i = 0; i < 69; i++)
            long_ += _T(" ");
         padded.push_back(long_);
      }
      for (const String &name : padded)
         refused.push_back(name);
      refused.push_back(_T("alice @example.test"));
      refused.push_back(_T("alice@ example.test"));
      refused.push_back(_T("alice@example.test") + Char(0xA0));

      // Other names altogether.
      refused.push_back(_T(""));
      refused.push_back(_T(" "));
      refused.push_back(_T("bob@example.test"));
      refused.push_back(_T("alice@example.tes"));
      refused.push_back(_T("alice@example.test.example"));
      refused.push_back(_T("alice@other.example"));
      refused.push_back(_T("alic@example.test"));

      for (const String &name : refused)
      {
         if (names(name))
            failures.push_back("the spelling '" + Narrow(name) + "' is taken for alice@example.test");
      }

      // The two that the C library may lower to the plain letter (a dotted capital
      // I, and the Kelvin sign for k): whichever way it lowers them, the rule and
      // the key agree, which is what is asked of every name below.
      std::vector<String> either =
      {
         _T("al") + Char(0x130) + _T("ce@example.test"), _T("al") + Char(0x131) + _T("ce@example.test"),
         _T("alice@example.tes") + Char(0x212A)
      };

      // Whatever the rule takes has the key of the stored address, so that the
      // lockout and the hold asked of the name are asked of the account; and a
      // padded name, which the rule refuses, still counts under that key.
      const String storedKey = key(stored);
      std::vector<String> all = taken;
      all.insert(all.end(), refused.begin(), refused.end());
      all.insert(all.end(), either.begin(), either.end());
      for (const String &name : all)
      {
         if (names(name) && !Equal(key(name), storedKey))
            failures.push_back("the rule takes '" + Narrow(name) + "' while its key '" + Narrow(key(name)) + "' is not the account's '" + Narrow(storedKey) + "'");
      }
      for (const String &name : padded)
      {
         if (!Equal(key(name), storedKey))
            failures.push_back("the padded name '" + Narrow(name) + "' is not counted under the account's key");
      }

      // A domain alias whose stored name has white space at its end, which an
      // administrator could save before the server trimmed it, and which the alias
      // match takes only with that white space: a name ending in it is refused, as
      // every padded name is, and whatever the rule takes under such an alias table
      // still has the account's key.
      const LogonName::AliasFunction paddedAliases = [](const String &address) -> String
      {
         const String domain = StringParser::ExtractDomain(address);
         if (domain.CompareNoCase(_T("alias.example ")) == 0 || domain.CompareNoCase(_T("tab.example\t")) == 0)
            return StringParser::ExtractAddress(address) + _T("@example.test");
         return address;
      };
      {
         const std::vector<String> paddedAliasNames =
         {
            _T("alice@alias.example "), _T("ALICE@Alias.Example "), _T("alice@alias.example  "), _T(" alice@alias.example "),
            _T("alice@tab.example\t"), _T("alice@alias.example"), _T("alice@tab.example")
         };
         const String paddedStoredKey = LogonName::KeyWith(stored, paddedAliases, defaultDomain);
         if (!Equal(paddedStoredKey, storedKey))
            failures.push_back("under an alias whose name ends in white space the stored address's key is '" + Narrow(paddedStoredKey) + "'");

         for (const String &name : paddedAliasNames)
         {
            if (LogonName::NamesWith(name, stored, paddedAliases, defaultDomain))
               failures.push_back("under an alias whose name ends in white space the spelling '" + Narrow(name) + "' is taken for alice@example.test");
         }

         std::vector<String> every = all;
         every.insert(every.end(), paddedAliasNames.begin(), paddedAliasNames.end());
         for (const String &name : every)
         {
            if (LogonName::NamesWith(name, stored, paddedAliases, defaultDomain) &&
                !Equal(LogonName::KeyWith(name, paddedAliases, defaultDomain), paddedStoredKey))
               failures.push_back("under an alias whose name ends in white space the rule takes '" + Narrow(name) + "' while its key '" +
                                  Narrow(LogonName::KeyWith(name, paddedAliases, defaultDomain)) + "' is not the account's");
         }

         // The account's own spellings are still taken under that alias table.
         for (const String &name : { String(_T("alice@example.test")), String(_T("ALICE@Example.Test")), String(_T("alice")) })
         {
            if (!LogonName::NamesWith(name, stored, paddedAliases, defaultDomain))
               failures.push_back("under an alias whose name ends in white space the name '" + Narrow(name) + "' is not taken for alice@example.test");
         }
      }

      // The stored address in other case is the same account; a stored address
      // that is empty is no account; with no default domain a bare local part
      // names none.
      if (!LogonName::NamesWith(_T("alice@example.test"), _T("Alice@Example.TEST"), aliases, defaultDomain))
         failures.push_back("a stored address in other case does not take its own name");
      if (LogonName::NamesWith(_T(""), _T(""), aliases, defaultDomain) || LogonName::NamesWith(_T(" "), _T(" "), aliases, defaultDomain))
         failures.push_back("an empty name is taken for an empty stored address");
      if (LogonName::NamesWith(_T("alice"), stored, aliases, _T("")))
         failures.push_back("a bare local part is taken with no default domain");
      if (!LogonName::NamesWith(_T("alice"), stored, LogonName::AliasFunction(), defaultDomain) ||
          LogonName::NamesWith(_T("alice@alias.example"), stored, LogonName::AliasFunction(), defaultDomain))
         failures.push_back("with no aliases, the default domain is not applied alone");

      // The key is what the lockout always computed: trimmed, lower case, aliases,
      // the default domain.
      if (!Equal(key(_T("  ALICE@Alias.Example\t")), _T("alice@example.test")) || !Equal(key(_T(" Alice ")), _T("alice@example.test")) ||
          !key(_T("   ")).IsEmpty() || !Equal(key(_T("bob@else.example")), _T("bob@else.example")))
         failures.push_back("the key is not the name trimmed, in lower case, with the aliases and the default domain applied");

      // The directory user rule, for an account linked to the directory user
      // jsmith of CORP.EXAMPLE (NetBIOS name CORP): the principal's user part and
      // realm in any case, the realm or its first label, are taken; every other
      // spelling of either, which a database's comparison may take, is refused.
      struct DirectoryCase { String user; String realm; String directoryUser; String directoryDomain; };
      std::vector<DirectoryCase> directoryTaken =
      {
         { _T("jsmith"), _T("CORP.EXAMPLE"), _T("JSmith"), _T("corp.example") },
         { _T("JSMITH"), _T("corp.example"), _T("jsmith"), _T("CORP") },
         { _T("jsmith"), _T("Corp.Example"), _T("jsmith"), _T("Corp.Example") },
         { _T("jsmith"), _T("corp"), _T("jsmith"), _T("corp") }
      };
      for (const DirectoryCase &c : directoryTaken)
      {
         if (!LogonName::NamesDirectoryUser(c.user, c.realm, c.directoryUser, c.directoryDomain))
            failures.push_back("the principal '" + Narrow(c.user) + "@" + Narrow(c.realm) + "' is not taken for the directory user '" +
                               Narrow(c.directoryUser) + "' of '" + Narrow(c.directoryDomain) + "'");
      }

      std::vector<DirectoryCase> directoryRefused =
      {
         { _T("jsm") + Char(0xEF) + _T("th"), _T("corp.example"), _T("jsmith"), _T("corp") },
         { _T("j") + Char(0xDF) + _T("mith"), _T("corp.example"), _T("jssmith"), _T("corp") },
         { Char(0xFF4A) + _T("smith"), _T("corp.example"), _T("jsmith"), _T("corp") },
         { _T("jsmith "), _T("corp.example"), _T("jsmith"), _T("corp") },
         { _T("jsmith"), _T("corp.example"), _T("jsmith "), _T("corp") },
         { _T("jsmith") + Char(0), _T("corp.example"), _T("jsmith"), _T("corp") },
         { _T("jsmith") + Char(0) + _T("x"), _T("corp.example"), _T("jsmith"), _T("corp") },
         { _T("jsmith"), _T("corp.ex") + Char(0xE0) + _T("mple"), _T("jsmith"), _T("corp.example") },
         { _T("jsmith"), _T("c") + Char(0xFF4F) + _T("rp.example"), _T("jsmith"), _T("corp") },
         { _T("jsmith"), _T("corp.example"), _T("jsmith"), _T("corp ") },
         { _T("jsmith"), _T("corp.example") + Char(0), _T("jsmith"), _T("corp.example") },
         { _T("jsmith"), _T("corp.example"), _T("jsmith"), _T("corp.other") },
         { _T("jsmith"), _T("other.example"), _T("jsmith"), _T("corp") },
         { _T("jsmit"), _T("corp.example"), _T("jsmith"), _T("corp") },
         { _T(""), _T("corp.example"), _T(""), _T("corp") }
      };
      for (unsigned int code : ignored)
      {
         directoryRefused.push_back({ _T("js") + Char(code) + _T("mith"), _T("corp.example"), _T("jsmith"), _T("corp") });
         directoryRefused.push_back({ _T("jsmith") + Char(code), _T("corp.example"), _T("jsmith"), _T("corp") });
         directoryRefused.push_back({ _T("jsmith"), _T("corp") + Char(code) + _T(".example"), _T("jsmith"), _T("corp") });
      }
      for (const DirectoryCase &c : directoryRefused)
      {
         if (LogonName::NamesDirectoryUser(c.user, c.realm, c.directoryUser, c.directoryDomain))
            failures.push_back("the principal '" + Narrow(c.user) + "@" + Narrow(c.realm) + "' is taken for the directory user '" +
                               Narrow(c.directoryUser) + "' of '" + Narrow(c.directoryDomain) + "'");
      }

      if (!failures.empty())
      {
         std::string message = "LogonNameTester: " + std::to_string(failures.size()) + " check(s) did not hold: ";
         for (size_t i = 0; i < failures.size(); i++)
            message += (i ? "; " : "") + failures[i];
         throw std::logic_error(message);
      }
   }
}
