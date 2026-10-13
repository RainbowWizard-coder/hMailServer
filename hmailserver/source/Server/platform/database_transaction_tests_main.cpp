// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The database layer's transaction scope (DatabaseTransaction, Common/SQL) held to
// what it promises on a real database, as a program of its own for the POSIX
// build - DatabaseTransactionTester, which ClassTester runs inside the Windows
// service on a SQL Server Compact database of its own.
//
//   database-transaction-tests <hMailServer.ini>
//
// against the database the file's [Database] section names, on a pool and tables
// of the tester's own (aatxnprobe, aatxnprobeb, and aatxnprobem on MySQL), which it
// makes, empties and removes. So it can run against a database the server has made,
// before the server starts on it - the Linux suite against its PostgreSQL
// (build/ci/linux-suite.sh), the database matrix against MySQL and MariaDB
// (build/lab/db-suite.sh) - and never against one a server is using: it ends its
// own pool's sessions on purpose. It provokes failures, which are reported to an
// error log as any failed statement is, so it reads the [Database] section alone
// into a configuration of its own, in a directory of its own under the system's
// temp directory, whose folders - the log folder among them - are that directory,
// removed again at the end; the server's log folder is never written.
//
// Without a file it says SKIPPED and exits 0: linux-build.sh runs every tester with
// no arguments, and there is no database there. One line a check, and a non-zero
// exit on a failure.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/SQL/DatabaseSettings.h"
#include "../Common/SQL/DatabaseTransaction.h"

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

   int Run(int argc, char *argv[]);
}

// Everything in Run, so that nothing a string or a path throws on the way leaves
// main: it is a failure of the run, said as one.
int
main(int argc, char *argv[])
{
   try
   {
      return Run(argc, argv);
   }
   catch (const std::exception &failure)
   {
      std::printf("FAIL  %s\n", failure.what());
      return 1;
   }
   catch (...)
   {
      std::printf("FAIL  the run ended in an exception that is not a std::exception\n");
      return 1;
   }
}

// The run itself, declared in the unnamed namespace above.
namespace
{
int
Run(int argc, char *argv[])
{
   if (argc < 2)
   {
      std::printf("SKIPPED  the transaction scope: no database was given (database-transaction-tests <hMailServer.ini>)\n");
      return 0;
   }

   std::string section;
   if (!ReadDatabaseSection(argv[1], section))
   {
      std::printf("REFUSED  %s could not be read, or has no [Database] section\n", argv[1]);
      return 2;
   }

   boost::system::error_code error;
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
      ("hm-database-transaction-tests-" + std::to_string(static_cast<long long>(::getpid())));
   if (!error)
      boost::filesystem::create_directories(scratch, error);
   if (!error)
      boost::filesystem::permissions(scratch, boost::filesystem::owner_all, error);

   if (error)
   {
      std::printf("REFUSED  no directory of its own could be made under the system's temp directory\n");
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
         std::printf("REFUSED  %s could not be written\n", file.string().c_str());
         boost::filesystem::remove_all(scratch, error);
         return 2;
      }
   }

   int result = 0;

   HM::IniFileSettings *settingsFile = HM::IniFileSettings::Instance();
   settingsFile->SetInitializationFile(HM::String(file.string().c_str()));
   settingsFile->LoadSettings();

   const HM::DatabaseSettings::SQLDBType type = settingsFile->GetDatabaseType();
   if (type != HM::DatabaseSettings::TypePGServer && type != HM::DatabaseSettings::TypeMYSQLServer)
   {
      std::printf("REFUSED  [Database] in %s names no PostgreSQL or MySQL database, the backends of this build\n", argv[1]);
      result = 2;
   }
   else
   {
      std::shared_ptr<HM::DatabaseSettings> settings(new HM::DatabaseSettings(settingsFile->GetDatabaseProvider(), settingsFile->GetDatabaseServer(),
         settingsFile->GetDatabaseName(), settingsFile->GetUsername(), settingsFile->GetPassword(), settingsFile->GetDatabaseDirectory(),
         settingsFile->GetDatabaseServerFailoverPartner(), type, settingsFile->GetDatabasePort()));

      try
      {
         HM::DatabaseTransactionTester tester;
         tester.Test(settings, [](const HM::String &line) { std::printf("ok    %s\n", HM::AnsiString(line).c_str()); }, true);
         std::printf("ok    the transaction scope on %s: every check passed\n", type == HM::DatabaseSettings::TypePGServer ? "PostgreSQL" : "MySQL or MariaDB");
      }
      catch (const std::exception &failure)
      {
         std::printf("FAIL  %s\n", failure.what());
         result = 1;
      }
   }

   boost::filesystem::remove_all(scratch, error);
   return result;
}
}
