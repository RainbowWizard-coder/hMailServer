// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The database layer's row counts, claims, leases and UTC clock, held on a real
// PostgreSQL, MySQL or MariaDB through the server's own connections - a program of
// its own for the POSIX build, the counterpart of the class test that runs inside
// the Windows server on a SQL Server Compact database of its own (ClassTester,
// which Utilities.RunTestSuite reaches). DatabaseClaimTester
// (Common/SQL/DatabaseClaimTester.h) says what each check holds; they are what
// ActiveActive.md's claims and leases rest on (decisions D1 and D3).
//
//   database-claim-tests                      says SKIPPED and exits 0: no database
//   database-claim-tests <hMailServer.ini>    the checks against the database whose
//                                             [Database] section that file holds
//
// One of the two testers that need a database, with database-transaction-tests, so
// without one it is skipped: that is how linux-build.sh, which runs every *-tests
// program with no arguments, runs it.
// build/ci/linux-suite.sh runs it against the suite's PostgreSQL and
// build/lab/db-suite.sh against the matrix's MySQL, MariaDB or PostgreSQL, each with
// the server's own configuration file, after --create-database and before the
// server starts.
//
// Only the file's [Database] section is read. It is copied into a configuration of
// this program's own, in a directory of its own under the system's temp directory
// (mode 0700, the file 0600: the section holds the database's password), whose
// folders are that directory - so that the error this program provokes on purpose (a
// statement on a table that does not exist reports HM5032, as any failed statement
// does) goes to a log of its own and not to the server's, whose error log the suite
// asserts empty. The directory is removed again. On the database: two tables of its
// own, made, emptied and removed; nothing of the server's is touched, and the only
// session it ends is its own pool's.
//
// One line a check, prefixed ok or FAIL; exit 0 when every check held, 1 when one
// did not or the database could not be reached, 2 when it was given nothing it can
// use - a file it cannot read, no [Database] section, or a backend this build lacks.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/SQL/DatabaseClaimTester.h"
#include "../Common/SQL/DatabaseSettings.h"

namespace
{
   std::string Trimmed(const std::string &text)
   {
      const size_t first = text.find_first_not_of(" \t\r");
      if (first == std::string::npos)
         return std::string();

      const size_t last = text.find_last_not_of(" \t\r");
      return text.substr(first, last - first + 1);
   }

   // The [Database] section of a configuration file, its lines as they are. False
   // when the file cannot be read or has no such section.
   bool ReadDatabaseSection(const std::string &path, std::string &section)
   {
      std::ifstream in(path.c_str());
      if (!in)
         return false;

      bool inside = false;
      bool found = false;
      std::string line;
      while (std::getline(in, line))
      {
         const std::string trimmed = Trimmed(line);
         if (!trimmed.empty() && trimmed[0] == '[')
         {
            std::string name = trimmed;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char) std::tolower(c); });
            inside = name == "[database]";
            found = found || inside;
            continue;
         }

         if (inside)
            section += trimmed + "\n";
      }

      return found;
   }
}

int
main(int argc, char *argv[])
{
   if (argc < 2)
   {
      printf("SKIPPED  the database's row counts, claims, leases and UTC clock: no database was given (database-claim-tests <hMailServer.ini>)\n");
      return 0;
   }

   std::string section;
   if (!ReadDatabaseSection(argv[1], section))
   {
      printf("REFUSED  %s could not be read, or has no [Database] section\n", argv[1]);
      return 2;
   }

   boost::system::error_code error;
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
      ("hm-database-claim-tests-" + std::to_string(static_cast<long long>(::getpid())));
   if (!error)
      boost::filesystem::create_directories(scratch, error);
   if (!error)
      boost::filesystem::permissions(scratch, boost::filesystem::owner_all, error);

   if (error)
   {
      printf("REFUSED  no directory of its own could be made under the system's temp directory\n");
      return 2;
   }

   const std::string directory = scratch.string();
   const boost::filesystem::path file = scratch / "hMailServer.ini";
   {
      const mode_t previous = ::umask(077);
      std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::trunc);
      ini << "[Directories]\n"
          << "ProgramFolder=" << directory << "\n"
          << "DataFolder=" << directory << "\n"
          << "LogFolder=" << directory << "\n"
          << "TempFolder=" << directory << "\n"
          << "EventFolder=" << directory << "\n"
          << "DatabaseFolder=" << directory << "\n"
          << "\n"
          << "[Database]\n"
          << section;
      const bool written = static_cast<bool>(ini);
      ini.close();
      ::umask(previous);

      if (!written)
      {
         printf("REFUSED  %s could not be written\n", file.string().c_str());
         boost::filesystem::remove_all(scratch, error);
         return 2;
      }
   }

   int result = 0;

   try
   {
      HM::IniFileSettings *settings = HM::IniFileSettings::Instance();
      settings->SetInitializationFile(HM::String(file.string().c_str()));
      settings->LoadSettings();

      const HM::DatabaseSettings::SQLDBType type = settings->GetDatabaseType();
      if (type != HM::DatabaseSettings::TypePGServer && type != HM::DatabaseSettings::TypeMYSQLServer)
      {
         printf("REFUSED  the [Database] section of %s names no PostgreSQL, MySQL or MariaDB database, the backends of the POSIX build\n", argv[1]);
         boost::filesystem::remove_all(scratch, error);
         return 2;
      }

      std::shared_ptr<HM::DatabaseSettings> database(new HM::DatabaseSettings(settings->GetDatabaseProvider(), settings->GetDatabaseServer(),
         settings->GetDatabaseName(), settings->GetUsername(), settings->GetPassword(), settings->GetDatabaseDirectory(),
         settings->GetDatabaseServerFailoverPartner(), type, settings->GetDatabasePort()));

      HM::DatabaseClaimTester tester;
      tester.Test(database, [](const HM::String &line)
      {
         printf("ok    %s\n", HM::AnsiString(line).c_str());
         // Each line out as it is made, in case a check after it hangs. std::flush
         // and not fflush, whose answer would go unread: the streams are kept in
         // step with stdio, so it writes out what printf buffered all the same.
         std::cout << std::flush;
      }, true);

      printf("PASSED  the database's row counts, claims, leases and UTC clock, through the server's own connections\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   boost::filesystem::remove_all(scratch, error);
   return result;
}
