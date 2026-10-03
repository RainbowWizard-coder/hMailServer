// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using hMailServer.Shared;

namespace DBSetup
{
   /// <summary>
   /// The external database DBSetup /unattended sets up, read from its environment
   /// (docs/WindowsOnArm.md, "A new installation without a window"). The names are the
   /// ones the container's entrypoint reads for the same [Database] section
   /// (docs/EnvironmentConfiguration.md), with the same values, so one set of names
   /// describes an hMailServer database wherever the server runs; two are Windows'
   /// own: HM_CREATE_DATABASE, the container's name, here says whether to create the
   /// database or to use one that exists, and HM_DB_SERVICE names the Windows service
   /// of a database server on this machine. The installer's /dbtype, /dbhost, /dbport,
   /// /dbname, /dbuser, /dbpasswordfile, /createdatabase and /dbservice arrive here in
   /// these variables. The password is read from HM_DB_PASSWORD or from the file
   /// HM_DB_PASSWORD_FILE names, never from a command line, and is written nowhere
   /// but to the server that asks for it.
   /// </summary>
   internal sealed class UnattendedSettings
   {
      public const string TypeVariable = "HM_DB_TYPE";
      public const string HostVariable = "HM_DB_HOST";
      public const string PortVariable = "HM_DB_PORT";
      public const string NameVariable = "HM_DB_NAME";
      public const string UserVariable = "HM_DB_USER";
      public const string PasswordVariable = "HM_DB_PASSWORD";
      public const string PasswordFileVariable = "HM_DB_PASSWORD_FILE";
      public const string CreateVariable = "HM_CREATE_DATABASE";
      public const string ServiceVariable = "HM_DB_SERVICE";

      // The name the built-in database has always had.
      public const string DefaultName = "hMailServer";

      // A database the create step makes is named in SQL without quotes for SQL
      // Server and MySQL (SQLStatement::GetCreateDatabase), so its name is held to
      // what all three accept unquoted, and to PostgreSQL's 63 bytes.
      private static readonly Regex CreatableName = new Regex("^[A-Za-z_][A-Za-z0-9_]{0,62}$", RegexOptions.CultureInvariant);

      public hMailServer.eDBtype Type { get; private set; }
      public string TypeName { get; private set; }
      public string Host { get; private set; }
      public int Port { get; private set; }
      public string Name { get; private set; }
      public string User { get; private set; }
      public string Password { get; private set; }
      public bool Create { get; private set; }
      public string Service { get; private set; }

      /// <summary>
      /// The settings, or null with the reason in <paramref name="problem"/>. A
      /// variable set to the empty string is unset, as the entrypoint reads it.
      /// Nothing here contacts the server or the database: every refusal comes
      /// before anything is changed.
      /// </summary>
      public static UnattendedSettings Read(Func<string, string> variable, out string problem)
      {
         problem = null;
         var settings = new UnattendedSettings();

         string type = Value(variable, TypeVariable);
         switch (type.ToLowerInvariant())
         {
            case "postgresql":
            case "postgres":
            case "pgsql":
               settings.Type = hMailServer.eDBtype.hDBTypePostgreSQL;
               settings.TypeName = "PostgreSQL";
               break;
            case "mysql":
            case "mariadb":
               settings.Type = hMailServer.eDBtype.hDBTypeMySQL;
               settings.TypeName = "MySQL";
               break;
            case "mssql":
               settings.Type = hMailServer.eDBtype.hDBTypeMSSQL;
               settings.TypeName = "Microsoft SQL Server";
               break;
            case "":
               problem = TypeVariable + " is not set: /unattended sets up the database the HM_DB_ variables name (" + TypeVariable + ", " +
                         HostVariable + ", " + PortVariable + ", " + NameVariable + ", " + UserVariable + ", and " + PasswordVariable +
                         " or " + PasswordFileVariable + "), and none was given.";
               return null;
            default:
               problem = TypeVariable + " is '" + type + "'; it must be PostgreSQL, MySQL (or MariaDB) or MSSQL (Microsoft SQL Server). " +
                         "The built-in database is not set up by DBSetup.";
               return null;
         }

         bool postgres = settings.Type == hMailServer.eDBtype.hDBTypePostgreSQL;
         bool sqlServer = settings.Type == hMailServer.eDBtype.hDBTypeMSSQL;

         // The server passes every value on as it is given, from 6.3.6: PostgreSQL's
         // quoted and escaped as libpq's conninfo asks (PGConnection::MakeConninfo),
         // SQL Server's user and password as arguments of their own, outside the
         // connection string, and its server in the quotes ADO reads
         // (ADOConnection::MakeConnectionString). Up to 6.3.5 a single quote or a
         // backslash for PostgreSQL, and a semicolon or a quote first for SQL Server,
         // were refused here, since the server wrote them into its string as they
         // were. What is still refused is what the server itself cannot carry: a
         // control character anywhere; a server name or user name that
         // hMailServer.ini, where both are written as they are, would not give back
         // (HoldsInIni); and a SQL Server name with a double quote beside a single
         // quote or a semicolon, which no form ADO reads can hold.

         string create = Value(variable, CreateVariable);
         string createWord = create.ToLowerInvariant();
         if (createWord == "" || createWord == "1" || createWord == "yes" || createWord == "true")
            settings.Create = true;
         else if (createWord == "0" || createWord == "no" || createWord == "false")
            settings.Create = false;
         else
         {
            problem = CreateVariable + " is '" + create + "'; it must be 1 (create the database, the default) or 0 (use a database that exists).";
            return null;
         }

         settings.Host = Value(variable, HostVariable);
         if (settings.Host.Length == 0)
         {
            problem = HostVariable + " is not set: it names the database server" + (sqlServer ? ", as HOST or HOST\\INSTANCE, and HOST,PORT for another port." : ", by name or address.");
            return null;
         }
         if (!Acceptable(settings.Host, sqlServer, HostVariable, out problem))
            return null;

         string port = Value(variable, PortVariable);
         if (sqlServer)
         {
            if (port.Length > 0)
            {
               problem = PortVariable + " is set, and SQL Server takes its port in " + HostVariable + " instead, as HOST,PORT (hMailServer does not read a port for SQL Server).";
               return null;
            }
            settings.Port = 0;
         }
         else if (port.Length == 0)
            settings.Port = postgres ? 5432 : 3306;
         else if (!Regex.IsMatch(port, "^[0-9]{1,5}$", RegexOptions.CultureInvariant) ||
                  !int.TryParse(port, NumberStyles.None, CultureInfo.InvariantCulture, out int number) || number < 1 || number > 65535)
         {
            problem = PortVariable + " is '" + port + "'; it must be a TCP port, 1 to 65535.";
            return null;
         }
         else
            settings.Port = number;

         settings.Name = Value(variable, NameVariable);
         if (settings.Name.Length == 0)
            settings.Name = DefaultName;
         if (settings.Create && !CreatableName.IsMatch(settings.Name))
         {
            problem = NameVariable + " is '" + settings.Name + "'; a database DBSetup creates is named with letters, digits and underscores, " +
                      "starting with a letter or an underscore, at most 63 of them.";
            return null;
         }
         if (!settings.Create && (settings.Name.Length > 128 || settings.Name.Any(c => char.IsWhiteSpace(c) || char.IsControl(c) || c == '\'' || c == '"' || c == ';' || c == '\\')))
         {
            problem = NameVariable + " is '" + settings.Name + "'; the name of an existing database may not hold white space, a quote, a semicolon or a backslash here.";
            return null;
         }

         settings.User = Value(variable, UserVariable);
         if (settings.User.Length == 0 && !sqlServer)
         {
            problem = UserVariable + " is not set: " + settings.TypeName + " needs a user name. (SQL Server without one uses Windows authentication.)";
            return null;
         }
         if (!Acceptable(settings.User, false, UserVariable, out problem))
            return null;

         string password = variable(PasswordVariable) ?? string.Empty;
         string passwordFile = Value(variable, PasswordFileVariable);
         if (password.Length > 0 && passwordFile.Length > 0)
         {
            problem = PasswordVariable + " and " + PasswordFileVariable + " are both set; set one of them.";
            return null;
         }
         if (passwordFile.Length > 0 && !ReadPasswordFile(passwordFile, out password, out problem))
            return null;
         if (password.Length > 0 && settings.User.Length == 0)
         {
            problem = "a database password is given and " + UserVariable + " is not set: SQL Server without a user name uses Windows authentication, which takes no password.";
            return null;
         }
         // Any other character is passed on as it is: the password is kept in
         // hMailServer.ini encrypted, not as text, and reaches the database server
         // through a quoted value (PostgreSQL) or an argument of its own (SQL Server).
         if (password.Any(char.IsControl))
         {
            problem = "the database password holds a control character, which no database server here takes in a password.";
            return null;
         }
         settings.Password = password;

         settings.Service = Value(variable, ServiceVariable);
         if (settings.Service.Length == 0)
            settings.Service = null;
         else if (settings.Service.Any(c => char.IsWhiteSpace(c) || char.IsControl(c) || c == '/' || c == '\\' || c == '"'))
         {
            problem = ServiceVariable + " is '" + settings.Service + "'; it takes the service's name, as sc.exe query lists it (for example postgresql-x64-18), not its display name.";
            return null;
         }
         else if (string.Equals(settings.Service, "hMailServer", StringComparison.OrdinalIgnoreCase))
         {
            problem = ServiceVariable + " names hMailServer itself; it names the service of the database server hMailServer is to start after.";
            return null;
         }

         return settings;
      }

      /// <summary>What is being set up, in one sentence, without the password.</summary>
      public string Describe()
      {
         var sentence = new StringBuilder();
         sentence.Append(Create ? "Creating the " : "Using the existing ");
         sentence.Append(TypeName).Append(" database ").Append(Name).Append(" on ").Append(Host);
         if (Port > 0)
            sentence.Append(':').Append(Port.ToString(CultureInfo.InvariantCulture));
         if (User.Length > 0)
            sentence.Append(", as ").Append(User).Append(Password.Length > 0 ? " with a password" : " without a password");
         else
            sentence.Append(", with Windows authentication (as the hMailServer service's account)");
         if (Service != null)
            sentence.Append("; the hMailServer service to start after ").Append(Service);
         sentence.Append('.');
         return sentence.ToString();
      }

      private static string Value(Func<string, string> variable, string name)
      {
         return (variable(name) ?? string.Empty).Trim();
      }

      private static bool Acceptable(string value, bool sqlServerName, string name, out string problem)
      {
         problem = null;
         if (value.Any(char.IsControl))
            problem = name + " holds a control character.";
         else if (!HoldsInIni(value))
            problem = name + " begins and ends with the same quotation mark, which hMailServer.ini, where it is written as it is, would take off when it is read. Leave the quotes out.";
         else if (sqlServerName && value.IndexOf('"') >= 0 && (value.IndexOf('\'') >= 0 || value.IndexOf(';') >= 0))
            problem = name + " holds a double quote beside a single quote or a semicolon, which no SQL Server connection string can carry.";
         return problem == null;
      }

      // hMailServer.ini gives back a value written as it is, but for one wrapped in a
      // matching pair of single or double quotation marks, which the reading takes off
      // (GetPrivateProfileString on Windows, IniFile.cpp's reading of it on Linux), and
      // white space at either end, which Value has taken off already.
      private static bool HoldsInIni(string value)
      {
         return !(value.Length >= 2 && (value[0] == '\'' || value[0] == '"') && value[value.Length - 1] == value[0]);
      }

      // The file's text, less the line breaks that end it - as the entrypoint's $(cat
      // FILE) takes it - and less a byte order mark, which Notepad writes. UTF-8, or
      // UTF-16 with its byte order mark; anything else is refused, as the container
      // refuses it: decoded leniently, a file Windows PowerShell 5.1's Set-Content
      // wrote in the ANSI code page turned each letter outside ASCII into U+FFFD, and
      // the database server refused a password nobody had typed, after the
      // installation, rather than DBSetup refusing the file before it.
      private static bool ReadPasswordFile(string path, out string password, out string problem)
      {
         password = string.Empty;
         problem = null;
         try
         {
            string text = File.ReadAllText(path, new UTF8Encoding(false, true));
            password = text.TrimStart('﻿').TrimEnd('\r', '\n');
            return true;
         }
         catch (DecoderFallbackException)
         {
            // The exception's message quotes the bytes it could not read, which are
            // the password's: not repeated.
            problem = PasswordFileVariable + " names " + path + ", which is not UTF-8 text. Save it as UTF-8 (in Windows PowerShell 5.1, Set-Content -Encoding UTF8).";
            return false;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            problem = PasswordFileVariable + " names " + path + ", which cannot be read: " + ex.Message;
            return false;
         }
      }
   }

   /// <summary>
   /// DBSetup without its wizard: DBSetup.exe /unattended. It sets up the database
   /// UnattendedSettings reads, as the wizard's last page does - the service
   /// dependency, then CreateExternalDatabase or SetDefaultDatabase, then a
   /// reinitialise - and then asks the server which schema it opened, and upgrades an
   /// existing database that is older than this build with DBUpdater, as
   /// DBSetupQuick does. No window is shown, whatever happens: every line goes to
   /// standard output, the reason for a failure to standard error, and the exit code
   /// is the result. The installer runs it for a database given with /dbtype or
   /// HM_DB_TYPE (hMailServerInnoExtension.iss), which is how a new installation of
   /// the ARM64 build, which has no built-in database, finishes without a window.
   /// Run by an administrator after the installer's database step did not complete,
   /// it then finishes the new installation as the installer would have
   /// (NewInstallation), and needs an elevated prompt for that, which it says before
   /// it changes anything.
   /// </summary>
   internal static class UnattendedSetup
   {
      public const string Switch = "/unattended";

      // The installer reads these (hMailServerInnoExtension.iss). 1 and 3 mean what
      // they mean for the wizard; 2 is new. 4 is never the installer's to read: it
      // clears the mark of an unfinished new installation before it runs this, and
      // only with that mark are there steps after the database to finish.
      public const int ExitSuccess = 0;
      public const int ExitFailed = 1;
      public const int ExitSettingsInvalid = 2;
      public const int ExitAuthenticationFailed = 3;
      public const int ExitNotFinished = 4;

      public static bool IsRequested()
      {
         return CommandLineParser.GetArguments().Keys.Any(argument => string.Equals(argument, Switch, StringComparison.OrdinalIgnoreCase));
      }

      public static int Run(TextWriter output, TextWriter error)
      {
         // Whatever a COM call throws that the steps below do not expect is a failure
         // with its reason, never an exception out of Main: that would be a crash, and
         // Windows Error Reporting may show a window for it, which is what this mode
         // exists not to do.
         try
         {
            return RunSteps(output, error);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return Fail(error, ExitFailed, "the database step stopped: " + ex.Message);
         }
      }

      private static int RunSteps(TextWriter output, TextWriter error)
      {
         UnattendedSettings settings = UnattendedSettings.Read(Environment.GetEnvironmentVariable, out string problem);
         if (settings == null)
            return Fail(error, ExitSettingsInvalid, problem + " Nothing was changed.");

         if (settings.Service != null && !ServiceExists(settings.Service))
            return Fail(error, ExitSettingsInvalid, UnattendedSettings.ServiceVariable + " names " + settings.Service + ", and no service of that name is installed on this machine. Nothing was changed.");

         // A new installation the installer did not finish: the steps after the
         // database are this run's, and they take an elevated administrator - said
         // now, before anything is changed, rather than after the database is set up.
         PendingNewInstallation pending = PendingNewInstallation.Read();
         if (pending != null && !NewInstallation.IsElevated())
            return Fail(error, ExitSettingsInvalid, NewInstallation.NotElevated + " Nothing was changed.");

         output.WriteLine(settings.Describe());

         hMailServer.Application application;
         try
         {
            application = new hMailServer.Application();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return Fail(error, ExitFailed, "hMailServer could not be reached over COM: " + ex.Message + " Nothing was changed.");
         }

         // No dialog, whatever the answer: the empty password a new installation may
         // have, then the one the installer gave in HM_ADMIN_PASSWORD (or an
         // administrator as password:VALUE). The wizard's sweep over the raw command
         // line is not tried; it is for the wizard's own callers.
         string given = Authenticator.GetGivenPassword();
         if (!Authenticator.AuthenticateUser(application, string.Empty) &&
             (given == null || !Authenticator.AuthenticateUser(application, given)))
         {
            return Fail(error, ExitAuthenticationFailed, "the administrator password was refused" +
                        (given == null ? ", and none was given (HM_ADMIN_PASSWORD, or /adminpassword to the installer)." : ".") + " Nothing was changed.");
         }

         // The dependency first, as the wizard sets it: a refusal then changes nothing
         // else, and the database step can be run again as it was.
         if (settings.Service != null)
         {
            string why = SetDependency(application, settings.Service);
            if (why != null)
               return Fail(error, ExitFailed, "the hMailServer service could not be made to start after " + settings.Service + ": " + why + " The database was not set up.");
            output.WriteLine("The hMailServer service starts after " + settings.Service + ".");
         }

         hMailServer.Database database = application.Database;
         try
         {
            if (settings.Create)
               database.CreateExternalDatabase(settings.Type, settings.Host, settings.Port, settings.Name, settings.User, settings.Password);
            else
               database.SetDefaultDatabase(settings.Type, settings.Host, settings.Port, settings.Name, settings.User, settings.Password);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            string hint = settings.Create
               ? " If the database exists already and is hMailServer's, set " + UnattendedSettings.CreateVariable + "=0 (/createdatabase=0) to use it."
               : string.Empty;
            return Fail(error, ExitFailed, (settings.Create ? "the database could not be created: " : "the database could not be used: ") + ex.Message + hint);
         }
         output.WriteLine(settings.Create ? "The database is created, and hMailServer.ini names it." : "hMailServer.ini names the database.");

         // The server takes the [Database] section the step above wrote. An older
         // existing database is refused here, which the check below reads the
         // versions for; any other refusal is the answer.
         string restartProblem = null;
         try
         {
            application.Reinitialize();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            restartProblem = ex.Message;
         }

         int verified = Verify(application, settings, restartProblem, given, output, error);
         if (verified != ExitSuccess || pending == null)
            return verified;

         bool finished = NewInstallation.Finish(new MachineHost(), pending, false, settings.Create, output.WriteLine, line => error.WriteLine("DBSetup /unattended: " + line), out bool ran);
         if (ran)
         {
            string kept = PendingNewInstallation.Clear();
            if (kept != null)
               error.WriteLine("DBSetup /unattended: the mark of the unfinished installation could not be removed (" + kept + "): delete the value " + PendingNewInstallation.AccountValue + " under HKEY_LOCAL_MACHINE\\" + PendingNewInstallation.KeyPath + " in the 32-bit view (WOW6432Node) by hand.");
         }
         return finished ? ExitSuccess : ExitNotFinished;
      }

      private static int Verify(hMailServer.Application application, UnattendedSettings settings, string restartProblem, string credential, TextWriter output, TextWriter error)
      {
         if (!ReadVersions(application, out int current, out int required, out string readProblem))
            return Fail(error, ExitFailed, "the server cannot open the database: " + (restartProblem ?? readProblem));

         if (current == required)
         {
            if (restartProblem != null)
               return Fail(error, ExitFailed, "the database is at this build's schema, " + current + ", but the server did not restart on it: " + restartProblem);
            output.WriteLine("The database is at schema " + current + ", the one this build requires.");
            return ExitSuccess;
         }

         if (current > required)
            return Fail(error, ExitFailed, "the database is at schema " + current + ", newer than the " + required + " this build requires: it belongs to a newer hMailServer.");

         if (current <= 0)
            return Fail(error, ExitFailed, "the database " + settings.Name + " holds no hMailServer schema" +
                        (settings.Create ? ", although it was just created: the create script did not run to its end. The error log says why." :
                                           ". To create one, leave " + UnattendedSettings.CreateVariable + " unset (or 1) and name a database that does not exist yet."));

         if (settings.Create)
            return Fail(error, ExitFailed, "the database was created at schema " + current + ", not the " + required + " this build requires: its create script and the server disagree.");

         output.WriteLine("The database is at schema " + current + "; this build requires " + required + ". Upgrading it with DBUpdater.");
         int upgraded = RunDatabaseUpgrade(credential, out string upgradeProblem);
         if (upgraded != 0)
            return Fail(error, ExitFailed, "the database upgrade did not complete (DBUpdater exit code " + upgraded + ")" + (upgradeProblem != null ? ": " + upgradeProblem : ".") + " The error log says why.");

         if (!ReadVersions(application, out current, out required, out readProblem))
            return Fail(error, ExitFailed, "the database was upgraded, and the server cannot open it: " + readProblem);
         if (current != required)
            return Fail(error, ExitFailed, "DBUpdater finished, and the database is at schema " + current + ", not " + required + ".");

         output.WriteLine("The database is at schema " + current + ", the one this build requires.");
         return ExitSuccess;
      }

      private static bool ReadVersions(hMailServer.Application application, out int current, out int required, out string problem)
      {
         current = 0;
         required = 0;
         problem = null;
         try
         {
            hMailServer.Database database = application.Database;
            current = database.CurrentVersion;
            required = database.RequiredVersion;
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            problem = ex.Message;
            return false;
         }
      }

      // DBUpdater, as DBSetupQuick runs it for an upgrade: beside this executable,
      // /SilentIfOk /silent, the credential in its environment and not on its command
      // line, and nothing shown.
      private static int RunDatabaseUpgrade(string credential, out string problem)
      {
         problem = null;
         try
         {
            var start = new ProcessStartInfo
            {
               FileName = Path.Join(AppContext.BaseDirectory, "DBUpdater.exe"),
               UseShellExecute = false,
               CreateNoWindow = true
            };
            start.ArgumentList.Add("/SilentIfOk");
            start.ArgumentList.Add("/silent");
            if (credential != null)
               start.Environment[Authenticator.PasswordVariable] = credential;

            using (Process process = Process.Start(start))
            {
               if (process == null)
               {
                  problem = "DBUpdater.exe did not start";
                  return ExitFailed;
               }
               process.WaitForExit();
               return process.ExitCode;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            problem = "DBUpdater.exe could not be started: " + ex.Message;
            return ExitFailed;
         }
      }

      private static bool ServiceExists(string name)
      {
         try
         {
            return ServiceController.GetServices().Any(service => string.Equals(service.ServiceName, name, StringComparison.OrdinalIgnoreCase));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return false;
         }
      }

      // What the wizard's ucPerformTask.SetDependency does, less its windows: the
      // server's own MakeDependent, which it allows while it runs as LocalSystem - as
      // it does during an installation - and otherwise sc.exe, run as this process.
      // The installer runs DBSetup elevated; an unattended run does not ask for
      // elevation. Null when it was set, else why not.
      private static string SetDependency(hMailServer.Application application, string service)
      {
         try
         {
            application.Utilities.MakeDependent(service);
            return null;
         }
         catch (Exception refused) when (!ExceptionPolicy.IsFatal(refused))
         {
            string arguments = "config hMailServer depend= \"RPCSS/" + service + "\"";
            try
            {
               var start = new ProcessStartInfo
               {
                  FileName = Path.Combine(Environment.SystemDirectory, "sc.exe"),
                  Arguments = arguments,
                  UseShellExecute = false,
                  CreateNoWindow = true,
                  RedirectStandardOutput = true,
                  RedirectStandardError = true
               };
               using (Process process = Process.Start(start))
               {
                  if (process == null)
                     return "the server refused (" + refused.Message + "), and sc.exe did not start.";
                  string said = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
                  process.WaitForExit();
                  if (process.ExitCode == 0)
                     return null;
                  return "the server refused (" + refused.Message + "), and sc.exe " + arguments + " returned " + process.ExitCode + ": " + said.Trim();
               }
            }
            catch (Exception other) when (!ExceptionPolicy.IsFatal(other))
            {
               return "the server refused (" + refused.Message + "), and sc.exe could not be run: " + other.Message;
            }
         }
      }

      private static int Fail(TextWriter error, int code, string reason)
      {
         error.WriteLine("DBSetup /unattended: " + reason);
         return code;
      }
   }
}
