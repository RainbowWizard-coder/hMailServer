// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Installation
{
   /// <summary>
   ///    Static assertions about the Inno Setup scripts in hmailserver\installation.
   ///
   ///    The installer is the one artefact this project cannot exercise from the
   ///    suite: running it hijacks the service path and the COM registration of the
   ///    machine the tests run on, which is why the only end-to-end check is the
   ///    installer smoke test on a throwaway VM (build/ci/installer-smoke-run.sh,
   ///    GitLab CI's installer-smoke job; the GitHub workflow installer-smoke.yml
   ///    until 18 September 2026). Everything in
   ///    between - "is the script self-consistent", "does it still agree with the
   ///    server it installs" - has had nothing watching it, and the cost of that
   ///    showed up immediately:
   ///
   ///    * The .NET 10 migration replaced the runtime probe's glob
   ///      Microsoft.WindowsDesktop.App\8.* with the same string containing a raw
   ///      0x08 byte where "\10" should have been - almost certainly a "\10" read as
   ///      an octal escape by whatever performed the replacement. The commit message
   ///      and the roadmap both say the probe "now globs 10.*". It never matched
   ///      anything, and no editor, diff or review would show the difference.
   ///    * The schema version moved to 6006 the same week. If the shipped upgrade
   ///      scripts and the version the server demands ever disagree, the installed
   ///      server refuses to start (Application::OnDatabaseConnected rejects a
   ///      database that is either too old or too new) - after the installer has
   ///      already replaced the binaries.
   ///
   ///    Not every test here is tied to a defect being fixed. Those that guard a
   ///    currently-correct invariant say so in their Description, so nobody mistakes
   ///    a green run for evidence that something was repaired.
   /// </summary>
   [TestFixture]
   public class InstallerPackaging : TestFixtureBase
   {
      // TestDirectory is <repo>\hmailserver\test\RegressionTests\bin\x64\Debug.
      // Six levels up is the repository root - the same relative walk
      // MIME\FuzzHarness.cs and SSL\SslSetup.cs use.
      private static string RepositoryRoot
      {
         get
         {
            return Path.GetFullPath(Paths.Combine(TestContext.CurrentContext.TestDirectory,
                                                 @"..\..\..\..\..\.."));
         }
      }

      private static string InstallationRoot
      {
         get { return Paths.Combine(RepositoryRoot, @"hmailserver\installation"); }
      }

      private static string SourceRoot
      {
         get { return Paths.Combine(RepositoryRoot, @"hmailserver\source"); }
      }

      private static string ReadRepositoryFile(string relativePath)
      {
         string path = Paths.Combine(RepositoryRoot, relativePath);
         Assert.IsTrue(File.Exists(path), "Missing file: " + path);
         return File.ReadAllText(path);
      }

      private static string ReadInstallerScript(string fileName)
      {
         return ReadRepositoryFile(Paths.Combine(@"hmailserver\installation", fileName));
      }

      private static IEnumerable<FileInfo> InstallerScripts()
      {
         var directory = new DirectoryInfo(InstallationRoot);
         Assert.IsTrue(directory.Exists, "Missing installer directory: " + InstallationRoot);
         return directory.GetFiles("*.iss", SearchOption.TopDirectoryOnly).OrderBy(file => file.Name);
      }

      /// <summary>
      ///    Entry lines of an Inno Setup section - everything that starts with
      ///    "Filename:", in file order, with comment lines dropped. Order matters for
      ///    [UninstallRun], which Inno executes top to bottom.
      /// </summary>
      private static List<string> EntryLines(string script)
      {
         return script.Split('\n')
                      .Select(line => line.Trim())
                      .Where(line => line.StartsWith("Filename:", StringComparison.OrdinalIgnoreCase))
                      .ToList();
      }

      #region ISPP

      /// <summary>
      ///    Enough of the Inno Setup preprocessor to resolve the {#NAME} references
      ///    the scripts use: #define of a string literal, or of literals and other
      ///    defines joined with '+'. Anything it cannot work out (GetEnv, arithmetic)
      ///    is left unresolved rather than guessed at, because a test that silently
      ///    substitutes the wrong value is worse than one that cannot substitute.
      /// </summary>
      private static Dictionary<string, string> PreprocessorDefines()
      {
         var defines = new Dictionary<string, string>();

         // Every define lives in the top-level script, which is included first.
         string script = ReadInstallerScript("hMailServer64.iss");

         foreach (Match match in Regex.Matches(script, @"^\s*#define\s+(\w+)\s*=?\s*(.+?)\s*$",
                                               RegexOptions.Multiline))
         {
            string value = EvaluateDefine(match.Groups[2].Value, defines);

            if (value != null)
               defines[match.Groups[1].Value] = value;
         }

         return defines;
      }

      private static string EvaluateDefine(string expression, Dictionary<string, string> known)
      {
         var value = new StringBuilder();

         // No literal in these scripts contains a '+', so splitting on it is safe.
         foreach (string term in expression.Split('+').Select(rawTerm => rawTerm.Trim()))
         {
            if (term.Length >= 2 && term.StartsWith("\"") && term.EndsWith("\""))
               value.Append(term, 1, term.Length - 2);
            else if (known.TryGetValue(term, out string knownValue))
               value.Append(knownValue);
            else
               return null;
         }

         return value.ToString();
      }

      private static string ResolveIspp(string text, Dictionary<string, string> defines)
      {
         return Regex.Replace(text, @"\{#(\w+)\}",
                              match => defines.TryGetValue(match.Groups[1].Value, out string defined)
                                          ? defined
                                          : match.Value);
      }

      #endregion

      [Test]
      [Description("No installer script contains a control character. This is the negative control for the 0x08 that broke the .NET runtime probe.")]
      public void TestInstallerScriptsContainNoControlCharacters()
      {
         var offenders = new List<string>();

         foreach (FileInfo script in InstallerScripts())
         {
            byte[] bytes = File.ReadAllBytes(script.FullName);

            for (int i = 0; i < bytes.Length; i++)
            {
               byte value = bytes[i];

               // Tab, CR and LF are the only control bytes these files may contain.
               if (value < 0x20 && value != 0x09 && value != 0x0A && value != 0x0D)
               {
                  offenders.Add(script.Name + " byte " + i + " = 0x" + value.ToString("x2"));
                  break;
               }
            }
         }

         Assert.IsEmpty(offenders,
            "Control characters in installer scripts: " + string.Join(", ", offenders) +
            ". A control character inside a path or a glob is invisible in every editor and every diff, " +
            "and it silently stops the surrounding expression from ever matching.");
      }

      [Test]
      [Description("Every reference to the bundled .NET Desktop Runtime installer names the same file.")]
      public void TestBundledDotNetRuntimeIsNamedConsistently()
      {
         Dictionary<string, string> defines = PreprocessorDefines();

         string[] scriptsThatReferenceIt = { "section_files_64.iss", "section_run.iss", "hMailServerInnoExtension.iss" };
         var referenced = new List<string>();

         foreach (string fileName in scriptsThatReferenceIt)
         {
            string script = ResolveIspp(ReadInstallerScript(fileName), defines);

            var names = Regex.Matches(script, @"windowsdesktop-runtime-[0-9.]+-win-x64\.exe")
                             .Cast<Match>()
                             .Select(match => match.Value)
                             .Distinct()
                             .ToList();

            Assert.IsNotEmpty(names,
               fileName + " no longer references the bundled .NET Desktop Runtime installer. " +
               "The server component needs it: DBSetup, DBSetupQuick and DBUpdater run during " +
               "installation and cannot start without the runtime.");

            referenced.AddRange(names);
         }

         Assert.AreEqual(1, referenced.Distinct().Count(),
            "The bundled .NET Desktop Runtime is referenced under more than one name (" +
            string.Join(", ", referenced.Distinct()) + "). One of the references is to a file that " +
            "will not be in {tmp}, so either the runtime is never installed or the installer stops with " +
            "a missing-file error. Define the name once in hMailServer64.iss and reference it.");
      }

      [Test]
      [Description("The installed-runtime probe matches the major version of the runtime the installer actually bundles.")]
      public void TestDotNetRuntimeProbeMatchesTheBundledMajorVersion()
      {
         Dictionary<string, string> defines = PreprocessorDefines();
         string extension = ResolveIspp(ReadInstallerScript("hMailServerInnoExtension.iss"), defines);

         Match bundled = Regex.Match(extension + ResolveIspp(ReadInstallerScript("section_run.iss"), defines),
                                     @"windowsdesktop-runtime-(\d+)\.");
         Assert.IsTrue(bundled.Success, "Could not find the bundled .NET runtime installer's version.");
         string major = bundled.Groups[1].Value;

         // The glob DotNetDesktopMissing() hands to FindFirst.
         Match probe = Regex.Match(extension, @"FindFirst\(ExpandConstant\('([^']*)'\)");
         Assert.IsTrue(probe.Success,
            "DotNetDesktopMissing() no longer probes for the installed runtime with FindFirst. " +
            "Without a probe the bundled runtime is either always installed or never installed.");

         string glob = probe.Groups[1].Value;

         // The shared framework records one directory per installed version under
         // Microsoft.WindowsDesktop.App, so the version has to be matched inside that
         // folder. Matching one level up cannot tell .NET 8 from .NET 10 - and
         // Windows' "name.*" wildcard matches an extension-less name too, so a glob
         // ending at the folder itself is satisfied by any runtime of any version.
         string expectedTail = @"\Microsoft.WindowsDesktop.App\" + major + ".*";

         Assert.IsTrue(glob.EndsWith(expectedTail, StringComparison.OrdinalIgnoreCase),
            "DotNetDesktopMissing() globs \"" + glob + "\" but the installer bundles the .NET " + major +
            " Desktop Runtime, so the glob must end with \"" + expectedTail + "\". As written it cannot " +
            "distinguish the required major version, and a probe that always answers the same way makes " +
            "the bundled runtime either useless or unavoidable. When it wrongly answers \"installed\", " +
            "DBSetupQuick and DBUpdater cannot run, and the upgrade finishes with the new server binary " +
            "against the old database schema.");
      }

      [Test]
      [Description("The upgrade scripts the installer ships can take a database to the schema version the server demands, for every engine.")]
      public void TestShippedUpgradeScriptsReachTheRequiredSchemaVersion()
      {
         Match required = Regex.Match(ReadRepositoryFile(@"hmailserver\source\Server\Common\Application\Constants.h"),
                                      @"#define\s+REQUIRED_DB_VERSION\s+(\d+)");
         Assert.IsTrue(required.Success, "Could not read REQUIRED_DB_VERSION from Constants.h.");
         int requiredVersion = int.Parse(required.Groups[1].Value);

         // DBUpdater's upgrade path. The installer never runs DBUpdater directly: it
         // runs DBSetupQuick, which shells DBUpdater.exe /SilentIfOk and propagates
         // its exit code - so this list is what an in-place upgrade actually executes.
         var steps = Regex.Matches(ReadRepositoryFile(@"hmailserver\source\Tools\DBUpdater\formMain.cs"),
                                   @"new\s+UpgradeScript\(\s*(\d+)\s*,\s*(\d+)\s*\)")
                          .Cast<Match>()
                          .Select(match => new
                          {
                             From = int.Parse(match.Groups[1].Value),
                             To = int.Parse(match.Groups[2].Value)
                          })
                          .ToList();

         Assert.GreaterOrEqual(steps.Count, 40,
            "Found only " + steps.Count + " upgrade steps in DBUpdater\\formMain.cs. The declaration format " +
            "probably changed and this test is no longer reading the upgrade path.");

         Assert.AreEqual(requiredVersion, steps.Last().To,
            "The last upgrade step in DBUpdater ends at schema " + steps.Last().To + " but the server requires " +
            requiredVersion + " (REQUIRED_DB_VERSION). An upgrade would run to completion, report success, and " +
            "leave a database the new server refuses to open.");

         // Contiguous: every step must start where the previous one finished, or a
         // database sitting on the version in the gap can never be upgraded at all.
         for (int i = 1; i < steps.Count; i++)
         {
            Assert.AreEqual(steps[i - 1].To, steps[i].From,
               "Gap in DBUpdater's upgrade path: " + steps[i - 1].From + "->" + steps[i - 1].To +
               " is followed by " + steps[i].From + "->" + steps[i].To + ".");
         }

         // Engine suffixes as the script files spell them. MSSQLCE reuses the MSSQL
         // create script but has its own upgrade scripts, because SQL CE rejects most
         // of the DDL the full product accepts.
         string[] engines = { "MSSQL", "MSSQLCE", "MySQL", "PGSQL" };
         string scriptDirectory = Paths.Combine(SourceRoot, "DBScripts");
         var missing = new List<string>();

         // 5002 is where the four-engine era starts; before that only MSSQL and
         // MySQL scripts exist, and no supported installation is that old.
         foreach (var step in steps.Where(step => step.To >= 5002))
         {
            foreach (string engine in engines)
            {
               string name = "Upgrade" + step.From + "to" + step.To + engine + ".sql";

               if (!File.Exists(Paths.Combine(scriptDirectory, name)))
                  missing.Add(name);
            }
         }

         Assert.IsEmpty(missing,
            "DBUpdater declares upgrade steps whose scripts are not in source\\DBScripts: " +
            string.Join(", ", missing) + ". The installer ships that directory wholesale, so a missing " +
            "script is a database that cannot be upgraded on the customer's machine.");

         // The final step must actually write the new version, and a fresh database
         // must be created at it. These are the two ways a database ends up on a
         // version the server rejects even though every script ran.
         foreach (string engine in engines)
         {
            string upgrade = File.ReadAllText(Paths.Combine(scriptDirectory,
               "Upgrade" + steps.Last().From + "to" + steps.Last().To + engine + ".sql"));

            Assert.IsTrue(Regex.IsMatch(upgrade, @"hm_dbversion\s+set\s+value\s*=\s*" + requiredVersion),
               "The " + engine + " upgrade script for the final step does not set hm_dbversion to " +
               requiredVersion + ".");
         }

         foreach (string createScript in new[] { "CreateTablesMSSQL.sql", "CreateTablesMySQL.sql", "CreateTablesPGSQL.sql" })
         {
            string create = File.ReadAllText(Paths.Combine(scriptDirectory, createScript));

            Assert.IsTrue(Regex.IsMatch(create, @"hm_dbversion\s+values\s*\(\s*" + requiredVersion + @"\s*\)"),
               createScript + " does not create hm_dbversion at " + requiredVersion +
               ", so a brand new installation would come up on a schema version the server rejects.");
         }

         Assert.IsTrue(ReadInstallerScript("section_files_common.iss").Contains(@"..\source\DBScripts\*.sql"),
            "The installer no longer ships source\\DBScripts, so DBUpdater would find no scripts to run.");
      }

      [Test]
      [Description("The installer writes no administrator password of its own - no [INI] entry and no MD5 - and has the server write it as PBKDF2 through hMailServer.exe --set-admin-password, only when one was collected and the file has none, before the database step, with the password in the environment and never on a command line.")]
      public void TestAdministratorPasswordIsOnlyWrittenWhenOneWasCollected()
      {
         string iniSection = ReadInstallerScript("section_ini.iss");

         string entry = EntryLines(iniSection)
            .FirstOrDefault(line => line.IndexOf("AdministratorPassword", StringComparison.OrdinalIgnoreCase) >= 0);

         ClassicAssert.IsNull(entry,
            "The [INI] section writes Security\\AdministratorPassword again: " + entry + ". What the installer " +
            "wrote there until 25 September 2026 was GetMD5OfString of the password - an unsalted MD5 digest, " +
            "which the server accepted and nothing re-hashed. The hash is the server's to make, through " +
            "hMailServer.exe --set-admin-password (WriteAdministratorPassword).");

         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         // The code, without its comments, which name the old MD5 to say why it went.
         string code = Regex.Replace(extension, @"//[^\n]*", "");

         ClassicAssert.IsFalse(code.IndexOf("GetMD5OfString", StringComparison.OrdinalIgnoreCase) >= 0,
            "The installer's code computes an MD5 again. An MD5 of the administrator password is what the " +
            "server authenticates against without a salt or a work factor, and an MD5 of the empty string " +
            "is a password the server accepts an empty password against.");

         ClassicAssert.IsTrue(Regex.IsMatch(code, @"function\s+HasAdministratorPassword\s*\(\s*\)\s*:\s*Boolean",
                                     RegexOptions.IgnoreCase),
            "HasAdministratorPassword is no longer defined in [Code].");

         Match function = Regex.Match(code, @"function\s+WriteAdministratorPassword\s*\(\s*\)\s*:\s*Boolean\s*;(.*?)\nend;",
                                      RegexOptions.Singleline | RegexOptions.IgnoreCase);
         ClassicAssert.IsTrue(function.Success, "WriteAdministratorPassword is no longer defined in [Code].");
         string body = function.Groups[1].Value;

         // Written only when one was collected - without that, every silent install
         // would write a password nobody chose - and only when the file has none, as
         // an upgrade never changes the administrator password.
         StringAssert.Contains("HasAdministratorPassword()", body,
            "WriteAdministratorPassword no longer asks whether a password was collected.");
         StringAssert.Contains("GetAdministratorPassword()", body,
            "WriteAdministratorPassword no longer leaves a password the file already has alone.");
         StringAssert.Contains("--set-admin-password", body,
            "WriteAdministratorPassword no longer has the server write the hash.");

         // The password goes in the environment and comes out of it again; no
         // command line carries it, where anybody listing processes reads it.
         ClassicAssert.IsTrue(Regex.IsMatch(body, @"SetProcessEnvironmentVariable\(\s*'HM_ADMIN_PASSWORD'\s*,\s*g_szAdminPassword\s*\)"),
            "The password is no longer handed to the verb in HM_ADMIN_PASSWORD.");
         ClassicAssert.IsTrue(Regex.IsMatch(body, @"finally\s+SetProcessEnvironmentVariable\(\s*'HM_ADMIN_PASSWORD'\s*,\s*''\s*\)"),
            "HM_ADMIN_PASSWORD is not emptied again in a finally, so every process the installer starts afterwards inherits the password.");
         foreach (Match exec in Regex.Matches(body, @"Exec\(([^;]*)\);"))
         {
            StringAssert.DoesNotContain("g_szAdminPassword", exec.Groups[1].Value,
               "The administrator password is on hMailServer.exe --set-admin-password's command line.");
         }

         // Before the database step, which signs in with the password and starts the
         // service - and the service reads the file when it starts.
         int tasks = code.IndexOf("function RunPostInstallTasks", StringComparison.OrdinalIgnoreCase);
         ClassicAssert.Greater(tasks, -1, "RunPostInstallTasks is no longer defined in [Code].");
         int write = code.IndexOf("WriteAdministratorPassword()", tasks, StringComparison.OrdinalIgnoreCase);
         int database = code.IndexOf("DBSetupQuick.exe", tasks, StringComparison.OrdinalIgnoreCase);
         ClassicAssert.Greater(write, -1, "RunPostInstallTasks no longer writes the administrator password.");
         ClassicAssert.Less(write, database,
            "RunPostInstallTasks runs the database step before it writes the administrator password, so the " +
            "database tools sign in against a file with no password, and the service they start reads none.");

         ClassicAssert.IsTrue(extension.IndexOf("{param:adminpassword", StringComparison.OrdinalIgnoreCase) >= 0,
            "There is no way to supply the administrator password to an unattended install. Without one, " +
            "every silent installation is left with no administrator password at all.");
      }

      [Test]
      [Description("The installer makes the stored-secrets key on a new installation only - hMailServer.exe --create-stored-secrets-key, after the database step and before the service is started - and never on an upgrade, where an administrator turns sealing on; and it says to export the key.")]
      public void TheInstallerMakesTheStoredSecretsKeyOnANewInstallationOnly()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string code = Regex.Replace(extension, @"//[^\n]*", "");

         Match procedure = Regex.Match(code, @"procedure\s+CreateStoredSecretsKey\s*\(\s*\)\s*;(.*?)\nend;",
                                       RegexOptions.Singleline | RegexOptions.IgnoreCase);
         ClassicAssert.IsTrue(procedure.Success, "CreateStoredSecretsKey is no longer defined in [Code].");
         string body = procedure.Groups[1].Value;

         ClassicAssert.IsTrue(Regex.IsMatch(body, @"IsNewDatabase\(\)\s*=\s*false\)\s*then\s*Exit", RegexOptions.IgnoreCase),
            "CreateStoredSecretsKey no longer leaves an upgrade alone: a key made by an upgrade would be made on whichever node was upgraded first.");
         StringAssert.Contains("'--create-stored-secrets-key'", body, "CreateStoredSecretsKey no longer runs the verb.");
         StringAssert.Contains("--export-stored-secrets-key", body, "CreateStoredSecretsKey no longer says to export the key it made.");

         int tasks = code.IndexOf("function RunPostInstallTasks", StringComparison.OrdinalIgnoreCase);
         ClassicAssert.Greater(tasks, -1, "RunPostInstallTasks is no longer defined in [Code].");
         int database = code.IndexOf("DBSetupQuick.exe", tasks, StringComparison.OrdinalIgnoreCase);
         int key = code.IndexOf("CreateStoredSecretsKey()", tasks, StringComparison.OrdinalIgnoreCase);
         int start = code.IndexOf("START hMailServer", tasks, StringComparison.OrdinalIgnoreCase);
         ClassicAssert.Greater(key, database, "RunPostInstallTasks makes the key before the database it is recorded in exists.");
         ClassicAssert.Less(key, start, "RunPostInstallTasks makes the key after it starts the service, whose first start would then have none.");
      }

      [Test]
      [Description("hMailServer.exe's verbs that Setup runs elevated write nothing to the error log (hMailServer.cpp, ForbidServiceFolderWrites), so Setup takes what they say from their own output into its log and its messages: --apply-new-install-defaults, --apply-profile, --create-stored-secrets-key and /Register all run through RunServerVerbReported, and no message sends anybody to the error log for what one of them did.")]
      public void WhatSetupsServerVerbsSayGoesIntoItsLogNotTheErrorLog()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string code = Regex.Replace(extension, @"//[^\n]*", "");

         string runner = CodeBody(code, "RunServerVerbReported");
         StringAssert.Contains("2>&1", runner, "RunServerVerbReported does not take the verb's standard error.");
         StringAssert.Contains("Log(", runner, "RunServerVerbReported does not put what the verb said into the setup log.");

         foreach (string verb in new[] { "--apply-new-install-defaults", "--apply-profile ", "--create-stored-secrets-key" })
         {
            ClassicAssert.IsFalse(Regex.IsMatch(code, @"Exec\(ExpandConstant\('\{app\}\\Bin\\hMailServer\.exe'\),\s*'" + Regex.Escape(verb)),
               verb + " is run without its output: what it says goes nowhere, since it writes nothing to the error log.");
            ClassicAssert.IsTrue(Regex.IsMatch(code, @"RunServerVerbReported\('" + Regex.Escape(verb)),
               verb + " is not run through RunServerVerbReported.");
         }
         StringAssert.Contains("RunServerVerbReported(szParameters", CodeBody(code, "RegisterHMailServerService"),
            "/Register is run without its output.");

         foreach (string name in new[] { "ApplyChosenProfile", "ApplyNewInstallDefaults", "CreateStoredSecretsKey", "RegisterHMailServerService" })
            ClassicAssert.IsFalse(Regex.IsMatch(CodeBody(code, name), @"error log", RegexOptions.IgnoreCase),
               name + " sends the administrator to the error log, where the verb no longer writes.");

         // And the server holds the other half: every command line but the service's
         // and the two developer modes forbids its writes.
         string server = ReadRepositoryFile(@"hmailserver\source\Server\hMailServer\hMailServer.cpp");
         ClassicAssert.IsTrue(Regex.IsMatch(server, @"if \(!IsServerCommandLine\(\)\)\s*ForbidServiceFolderWrites\(\);"),
            "hMailServer.exe no longer forbids an administrator's verb its writes in the service's folders.");
      }

      [Test]
      [Description("When the administrator running Setup cannot open the database - SQL Server with Windows authentication, LocalSystem a SQL Server administrator and that person not - the secure defaults and the stored-secrets key are applied by the service at the start Setup makes (sc start hMailServer --apply-new-install-defaults), which the server reads in ServiceMain: InitializeApplication hands FinishNewInstallationAtStart to that start and to no other, and Application::InitInstanceAndStart runs it once InitInstance has opened the database, starts again (Reinitialize_) when it changed the server and else starts the servers; and a key Setup's verb found already held is not said to have been made.")]
      public void ANewInstallationItsAdministratorCannotOpenIsFinishedByTheServicesStart()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string code = Regex.Replace(extension, @"//[^\n]*", "");
         string server = ReadRepositoryFile(@"hmailserver\source\Server\hMailServer\hMailServer.cpp");

         // The defaults' exit 2 and the key's could-not-open both leave it to the start.
         string defaults = CodeBody(code, "ApplyNewInstallDefaults");
         ClassicAssert.IsTrue(Regex.IsMatch(defaults, @"2:\s*begin\s*g_bFinishNewInstallationAtStart\s*:=\s*true", RegexOptions.Singleline),
            "ApplyNewInstallDefaults does not leave the defaults to the service's start when the verb could not open the database (exit 2).");
         string key = CodeBody(code, "CreateStoredSecretsKey");
         const string notOpened = "The database could not be opened";
         StringAssert.Contains("Pos('" + notOpened + "', szSaid)", key, "CreateStoredSecretsKey does not tell a database it could not open from any other refusal.");
         StringAssert.Contains("\"" + notOpened, server, "hMailServer.exe --create-stored-secrets-key no longer says \"" + notOpened + "\", which Setup looks for.");

         // A key found is said as found.
         const string found = "already holds the stored-secrets key";
         StringAssert.Contains("Pos('" + found + "', szSaid)", key, "CreateStoredSecretsKey says it made a key the verb only found.");
         StringAssert.Contains(found, server, "hMailServer.exe --create-stored-secrets-key no longer says \"" + found + "\", which Setup looks for.");

         // The start that finishes it, and the server's half.
         string tasks = CodeBody(code, "RunPostInstallTasks");
         ClassicAssert.IsTrue(Regex.IsMatch(tasks, @"if\s+g_bFinishNewInstallationAtStart\s+then\s+bStarted\s*:=\s*StartServiceToFinishNewInstallation\(", RegexOptions.IgnoreCase),
            "RunPostInstallTasks does not start the service to finish the installation when the verbs could not.");
         StringAssert.Contains("'start hMailServer --apply-new-install-defaults'", CodeBody(code, "StartServiceToFinishNewInstallation"));
         // ServiceMain reads the ask into the flag, in its code and not in a
         // comment, before it starts (InitializeApplication), and nothing between
         // the two sets the flag again.
         string serviceMain = CppFunctionCode(server, "ServiceMain(", "hMailServer.cpp");
         ClassicAssert.IsTrue(Regex.IsMatch(serviceMain, @"argument\.CompareNoCase\(_T\(""--apply-new-install-defaults""\)\) == 0\)\s*g_finishNewInstallationAtStart = true;(?:(?!g_finishNewInstallationAtStart\s*=(?!=))[\s\S])*\n\s*InitializeApplication\(\);"),
            "ServiceMain does not read --apply-new-install-defaults as the start's ask to finish a new installation before it starts, or sets the ask again first.");

         // InitializeApplication hands the finish to the start that was asked for
         // it, and to no other. Since 10 October 2026 the start itself is
         // Application::InitInstanceAndStart, which holds the servers' lock
         // throughout, and the finish is the function InitializeApplication gives it.
         string initialize = CppFunctionCode(server, "void InitializeApplication(", "hMailServer.cpp");
         ClassicAssert.IsTrue(Regex.IsMatch(initialize, @"^(?:(?!g_finishNewInstallationAtStart\s*=(?!=))[\s\S])*const bool finish = g_finishNewInstallationAtStart;\s*g_finishNewInstallationAtStart = false;"),
            "InitializeApplication no longer takes whether to finish a new installation from what ServiceMain read, or sets the flag before it takes it.");
         ClassicAssert.IsTrue(Regex.IsMatch(initialize, @"InitInstanceAndStart\(sErrorMessage,\s*finish \? std::function<bool\(\)>\(&FinishNewInstallationAtStart\) : std::function<bool\(\)>\(\)\);"),
            "InitializeApplication does not hand FinishNewInstallationAtStart to a start asked to finish a new installation, or hands it to every start.");

         // The finish applies the defaults and makes the key, and answers true when
         // that may have changed the server.
         string finisher = CppFunctionCode(server, "static bool FinishNewInstallationAtStart(", "hMailServer.cpp");
         ClassicAssert.IsTrue(Regex.IsMatch(finisher, @"HM::NewInstallDefaults::ApplyAtServiceStart\(\)") && Regex.IsMatch(finisher, @"HM::StoredSecrets::CreateKey\(") &&
                              Regex.IsMatch(finisher, @"return defaults == 0 \|\| defaults == 1 \|\| keyMade;\s*\}$"),
            "FinishNewInstallationAtStart does not apply the defaults and make the key, or does not answer true when it changed the server.");

         // The start runs it once InitInstance has opened the database, starts
         // again, listeners and all, when it changed the server (Reinitialize_,
         // given the message indexer since the indexer's own thread may ask for a
         // reinitialisation), and else starts the servers - at its end, and
         // nowhere else.
         string application = ReadRepositoryFile(@"hmailserver\source\Server\Common\Application\Application.cpp");
         string start = CppFunctionCode(application, "Application::InitInstanceAndStart(", "Application.cpp");
         ClassicAssert.IsTrue(Regex.IsMatch(start, @"if \(!InitInstance\(sErrorMessage\)\)\s*return false;\s*if \(finish && finish\(\)\)\s*\{\s*sErrorMessage = Reinitialize_\(\*MessageIndexer::Instance\(\)\);\s*return sErrorMessage\.IsEmpty\(\);\s*\}"),
            "Application::InitInstanceAndStart does not run the finish it is given once InitInstance has opened the database, or does not start again (Reinitialize_) when the finish changed the server.");
         ClassicAssert.IsTrue(Regex.IsMatch(start, @"return StartServers_\(\);\s*\}$"),
            "Application::InitInstanceAndStart does not start the servers when it was given no finish or the finish changed nothing.");
         ClassicAssert.AreEqual(1, Regex.Matches(start, @"\bStartServers_\(\)").Count,
            "Application::InitInstanceAndStart starts the servers somewhere other than at its end, so a start can begin them before InitInstance has opened the database.");
      }

      [Test]
      [Description("/GrantServiceAccess that cannot read the database - so does not know the certificate and key files it names - ends in 7 unless an administrator confirmed, and the installer moves the service only on 0; a service already moved is not held to it at an upgrade.")]
      public void AGrantThatCouldNotReadTheDatabaseDoesNotMoveTheServiceSilently()
      {
         string account = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         ClassicAssert.IsTrue(Regex.IsMatch(account, @"if \(!confirmed && !databaseWhy\.IsEmpty\(\)\)\s*return 7;"),
            "/GrantServiceAccess ends in 0 when it could not read the database, and the move goes on without knowing what it names.");

         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string code = Regex.Replace(extension, @"//[^\n]*", "");
         StringAssert.Contains("7: Result :=", CodeBody(code, "GrantOutcomeSentence"), "The installer does not say what the grant's 7 means.");
         StringAssert.Contains("(ResultCode = 7)", CodeBody(code, "GrantServiceAccess"), "A service already moved fails its upgrade when the account running setup cannot read the database.");
         ClassicAssert.IsTrue(Regex.IsMatch(CodeBody(code, "MoveServiceToVirtualAccount"), @"else if \(ResultCode <> 0\) then"),
            "The move no longer waits for a clean grant.");
      }

      [Test]
      [Description("The uninstaller stops the service before deleting it, and tolerates files an old installation never had.")]
      public void TestUninstallStepsAreOrderedAndOptional()
      {
         List<string> entries = EntryLines(ReadInstallerScript("section_uninstallrun.iss"));

         Assert.IsNotEmpty(entries, "section_uninstallrun.iss has no entries.");

         int stopIndex = entries.FindIndex(line =>
            line.IndexOf("net.exe", StringComparison.OrdinalIgnoreCase) >= 0 &&
            Regex.IsMatch(line, @"STOP\s+hMailServer""", RegexOptions.IgnoreCase));

         int unregisterIndex = entries.FindIndex(line =>
            line.IndexOf("/Unregister", StringComparison.OrdinalIgnoreCase) >= 0);

         Assert.GreaterOrEqual(stopIndex, 0, "The uninstaller no longer stops the hMailServer service.");
         Assert.GreaterOrEqual(unregisterIndex, 0, "The uninstaller no longer unregisters hMailServer.");

         Assert.IsTrue(stopIndex < unregisterIndex,
            "The uninstaller runs hMailServer.exe /Unregister before stopping the service. DeleteService on " +
            "a running service only marks it for deletion and leaves the process running, so the uninstaller " +
            "then starts deleting a directory whose files the server still has open, and COM is unregistered " +
            "from under a live server. Stopping first is what makes the rest safe - with the wait for the process " +
            "that CurUninstallStepChanged does before these entries run, because net stop returns when the service " +
            "reports STOPPED and the process lives about a second longer. build/check-installer-scripts.py holds " +
            "that wait, and a RunOnceId on every entry.");

         var unguarded = entries
            .Where(line => line.IndexOf("{app}", StringComparison.OrdinalIgnoreCase) >= 0 &&
                           line.IndexOf("skipifdoesntexist", StringComparison.OrdinalIgnoreCase) < 0)
            .ToList();

         Assert.IsEmpty(unguarded,
            "[UninstallRun] entries run a file under {app} without skipifdoesntexist: " +
            string.Join(" | ", unguarded) + ". Inno raises an error dialog for each one that is not there, " +
            "and the entries kept for hMailServer 3 and 4 installations (mysqld-nt, hSMTPServer, hPOP3Server) " +
            "are never present on a current one.");
      }

      [Test]
      [Description("Guards a currently-correct invariant: uninstall must not remove mail, the database or the configuration.")]
      public void TestUninstallDoesNotRemoveMailData()
      {
         // Inno's uninstaller only deletes what it logged, and it removes the
         // directories it created only when they are empty - so Data, Database and
         // Logs survive by construction. An [UninstallDelete] entry is the one way to
         // break that, and it would destroy every message on the server.
         string[] protectedPaths = { @"{app}\Data", @"{app}\Database", @"{app}\Bin\hMailServer.ini" };

         foreach (FileInfo script in InstallerScripts())
         {
            string text = File.ReadAllText(script.FullName);

            int sectionStart = text.IndexOf("[UninstallDelete]", StringComparison.OrdinalIgnoreCase);
            if (sectionStart < 0)
               continue;

            // To the next section header, or the end of the file.
            Match nextSection = Regex.Match(text.Substring(sectionStart + 1), @"^\[", RegexOptions.Multiline);
            string section = nextSection.Success
               ? text.Substring(sectionStart, nextSection.Index + 1)
               : text.Substring(sectionStart);

            foreach (string entry in section.Split('\n').Select(line => line.Trim())
               .Where(line => line.StartsWith("Type:", StringComparison.OrdinalIgnoreCase) ||
                              line.StartsWith("Name:", StringComparison.OrdinalIgnoreCase)))
            {
               bool recursive = entry.IndexOf("filesandordirs", StringComparison.OrdinalIgnoreCase) >= 0;

               foreach (string protectedPath in protectedPaths)
               {
                  Assert.IsFalse(entry.IndexOf(protectedPath, StringComparison.OrdinalIgnoreCase) >= 0,
                     script.Name + " has an [UninstallDelete] entry covering " + protectedPath + ": " + entry +
                     ". Uninstalling hMailServer must not delete mail, the database or the configuration.");
               }

               Assert.IsFalse(recursive && Regex.IsMatch(entry, @"Name:\s*""\{app\}\\?""", RegexOptions.IgnoreCase),
                  script.Name + " has an [UninstallDelete] entry that recursively deletes {app}: " + entry +
                  ". That is the whole mail store.");
            }
         }
      }

      [Test]
      [Description("Guards a currently-correct invariant: an upgrade must not rewrite the database connection settings or the paths an administrator has moved.")]
      public void TestUpgradePreservesExistingConfiguration()
      {
         List<string> entries = EntryLines(ReadInstallerScript("section_ini.iss"));

         Assert.IsNotEmpty(entries, "section_ini.iss has no entries.");

         // The Database section holds the engine, host, database name, user and
         // password. DBSetup writes it; the installer must never touch it, or an
         // upgrade points a working server at nothing.
         var databaseEntries = entries
            .Where(line => Regex.IsMatch(line, @"Section:\s*""Database""", RegexOptions.IgnoreCase))
            .ToList();

         Assert.IsEmpty(databaseEntries,
            "The [INI] section writes into the ini's Database section: " + string.Join(" | ", databaseEntries) +
            ". Those keys are the database connection an upgrade has to preserve.");

         // Directory keys are defaults for a fresh installation only (the
         // administrator password is no longer an [INI] entry: WriteAdministratorPassword
         // writes it only into a file that has none). ProgramFolder is the deliberate
         // exception: it has to follow the installation directory, which the user may
         // have changed.
         foreach (string entry in entries.Where(entry => !Regex.IsMatch(entry, @"Key:\s*""(ProgramFolder|ValidLanguages)""", RegexOptions.IgnoreCase)))
         {
            Assert.IsTrue(entry.IndexOf("createkeyifdoesntexist", StringComparison.OrdinalIgnoreCase) >= 0,
               "[INI] entry overwrites an existing value on upgrade: " + entry +
               ". Add createkeyifdoesntexist, or an administrator's customised setting is replaced by the " +
               "installer's default every time they upgrade.");
         }
      }

      [Test]
      [Description("The service the installer creates is given failure recovery actions.")]
      public void TestServiceIsGivenRecoveryActions()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         // ServiceManager::RegisterService creates the service with auto-start and a
         // dependency on RPCSS, and never calls ChangeServiceConfig2 - so unless the
         // installer sets them, the recovery configuration is Windows' default of
         // "take no action" and a mail server that dies stays dead.
         Assert.IsTrue(Regex.IsMatch(extension, @"sc\.exe'\)\s*,\s*'failure\s+hMailServer", RegexOptions.IgnoreCase),
            "The installer does not configure the hMailServer service's failure actions. Nothing else does " +
            "either: ServiceManager::RegisterService never calls ChangeServiceConfig2.");

         Assert.IsTrue(Regex.IsMatch(extension, @"failure\s+hMailServer[^']*reset=\s*\d+", RegexOptions.IgnoreCase),
            "sc.exe failure is invoked without reset=, so the failure count never clears and the escalating " +
            "restart delays accumulate over the life of the machine.");

         Assert.IsTrue(Regex.IsMatch(extension, @"failure\s+hMailServer[^']*actions=\s*restart/", RegexOptions.IgnoreCase),
            "sc.exe failure is invoked without a restart action.");
      }

      /// <summary>
      ///    The body of one [Code] function or procedure of hMailServerInnoExtension.iss:
      ///    from its header to the "end;" that closes it at the start of a line.
      /// </summary>
      private static string CodeBody(string extension, string name)
      {
         Match header = Regex.Match(extension, @"^(?:function|procedure)\s+" + Regex.Escape(name) + @"\s*\(", RegexOptions.Multiline | RegexOptions.IgnoreCase);
         Assert.IsTrue(header.Success, name + " is not defined in hMailServerInnoExtension.iss.");

         Match close = Regex.Match(extension.Substring(header.Index), @"^end;", RegexOptions.Multiline);
         Assert.IsTrue(close.Success, name + " has no end.");
         return extension.Substring(header.Index, close.Index + close.Length);
      }

      /// <summary>
      ///    The code of one C++ function definition of a server source file, its
      ///    comments dropped - "//" to the end of the line and "/*" to "*/" - so
      ///    that a comment naming a call cannot stand in for the call: from the
      ///    line that starts with its declarator ("void InitializeApplication(",
      ///    "Application::InitInstanceAndStart("), a declaration passed over, to
      ///    the first "}" at that line's indentation, which closes it in this
      ///    tree's style. A string literal holding "//" or "/*" would be cut as a
      ///    comment; none of the functions read through this holds one.
      /// </summary>
      private static string CppFunctionCode(string source, string declarator, string file)
      {
         Match header = Regex.Match(source, @"^([ \t]*)" + Regex.Escape(declarator) + @"[^;{]*\{", RegexOptions.Multiline);
         Assert.IsTrue(header.Success, declarator + "...) is not defined in " + file + ".");

         Match close = Regex.Match(source.Substring(header.Index + header.Length), @"^" + Regex.Escape(header.Groups[1].Value) + @"\}", RegexOptions.Multiline);
         Assert.IsTrue(close.Success, declarator + "...) has no end in " + file + ".");
         string definition = source.Substring(header.Index, header.Length + close.Index + close.Length);
         return Regex.Replace(definition, @"/\*[\s\S]*?\*/|//[^\n]*", "");
      }

      [Test]
      [Description("A new installation registers the service as the virtual account NT SERVICE\\hMailServer, an upgrade keeps the account the service has, the page offers the way to LocalSystem in every case, and a silent installation takes /serviceaccount= (docs/ServiceAccount.md).")]
      public void TestNewInstallationDefaultsToTheVirtualAccount()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         string choice = CodeBody(extension, "GetServiceAccountChoice");
         Assert.IsTrue(Regex.IsMatch(choice, @"if\s+IsNewService\(\)\s+then\s+Result\s*:=\s*PendingAccountDefault\(\)\s+else\s+Result\s*:=\s*'keep'", RegexOptions.IgnoreCase),
            "GetServiceAccountChoice does not default a new installation to the virtual account and an upgrade to the account the service has. " +
            "An upgrade that moved an existing service would change what it can reach without anybody choosing it.");
         // A new installation's default is the virtual account, unless an unfinished
         // one this finishes chose LocalSystem (its mark, PendingNewInstallation).
         string pendingDefault = CodeBody(extension, "PendingAccountDefault");
         Assert.IsTrue(Regex.IsMatch(pendingDefault, @"else\s+Result\s*:=\s*'virtual'"),
            "A new installation's default account is not the virtual account.");
         Assert.IsTrue(Regex.IsMatch(pendingDefault, @"if\s+PendingNewInstallation\(\)\s+and"),
            "A new installation with no mark of an unfinished one could default to LocalSystem.");
         StringAssert.Contains("{param:serviceaccount|}", choice,
            "A silent installation has no page, so /serviceaccount= is its only way to choose.");
         foreach (string word in new[] { "'virtual'", "'localsystem'", "'keep'" })
            StringAssert.Contains(word, choice, "GetServiceAccountChoice does not know " + word + ".");

         // IsNewService is read from the service's own entry, before this
         // installation registers anything.
         string before = CodeBody(extension, "GetServiceAccountBefore");
         StringAssert.Contains(@"SYSTEM\CurrentControlSet\Services\hMailServer", before);
         StringAssert.Contains("'ObjectName'", before);
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "CurStepChanged"), @"ssInstall[\s\S]*GetServiceAccountBefore\(\)", RegexOptions.IgnoreCase),
            "The account the service had is not read at ssInstall, before the registration changes it.");

         // The page: three forms, each with a second choice that leads away from the
         // first, LocalSystem among them.
         string pages = CodeBody(extension, "CreateWizardPages");
         Assert.AreEqual(3, Regex.Matches(pages, @"g_pageServiceAccount\s*:=\s*CreateInputOptionPage\(").Count,
            "The service account page is not made in its three forms: a new installation, an upgrade of a LocalSystem service, and an upgrade of a service with an account of its own.");
         StringAssert.Contains("'NT SERVICE\\hMailServer - a virtual account of its own (recommended)'", pages);
         StringAssert.Contains("'LocalSystem - the most privileged account, as earlier versions ran it'", pages);
         StringAssert.Contains("'Move it back to LocalSystem'", pages,
            "An upgrade of a service that is not LocalSystem is offered no way back to it.");
         Assert.IsTrue(Regex.IsMatch(pages, @"g_pageServiceAccount\.SelectedValueIndex\s*:=\s*0"),
            "The page's first choice - the virtual account, or the account the service has - is not the default.");

         // What the page chose reaches the server, as one word before /Register -
         // first the account the database step runs under, then, for a move, the
         // virtual account.
         string register = CodeBody(extension, "RegisterHMailServerService");
         Assert.IsTrue(Regex.IsMatch(register, @"'--service-account\s*'\s*\+\s*szAccountChoice\s*\+\s*'\s*/Register'"),
            "RegisterHMailServerService does not pass the account it is given to hMailServer.exe /Register.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "RunPostInstallTasks"), @"RegisterHMailServerService\(\s*FirstRegistrationChoice\(\s*GetServiceAccountChoice\(\)\s*\)\s*\)"),
            "RunPostInstallTasks does not register the service with the account the page chose, as the database step needs it.");

         // And the server knows each word the installer can pass.
         string server = ReadRepositoryFile(@"hmailserver\source\Server\hMailServer\hMailServer.cpp");
         StringAssert.Contains("_T(\"--service-account\")", server, "hMailServer.exe does not read --service-account.");
         foreach (string word in new[] { "virtual", "localsystem", "keep" })
         {
            StringAssert.Contains("serviceAccountWord.CompareNoCase(_T(\"" + word + "\"))", server,
               "The installer passes --service-account " + word + " and hMailServer.exe does not know it; /Register would refuse and there would be no service.");
         }

         // /Register takes the account from its command line and nowhere else:
         // ServiceAccountName is a row of the settings store, which the service
         // and hMailServer administrators write, and a registration that read it
         // would let either make the next elevated /Register give the service
         // LocalSystem back (docs/ServiceAccount.md).
         string manager = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceManager.cpp");
         Assert.IsFalse(manager.Contains("GetServiceAccountName") || manager.Contains("GetServiceAccountPassword"),
            "ServiceManager reads the service account from hMailServer.INI again.");
         Assert.IsFalse(server.Contains("GetServiceAccountName"),
            "hMailServer.exe reads the service account from hMailServer.INI again.");
      }

      [Test]
      [Description("A service moved to the virtual account is moved after the database step, which writes hMailServer.INI and the service's dependencies through a LocalSystem service, and only after a grant that ended cleanly - with the question the grant asks in an attended installation; one that already runs as another account is granted its folders again before the database step, and a grant that fails there fails the installation in its own words.")]
      public void TestTheServiceAccountIsGrantedItsFoldersBeforeTheServiceStarts()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string tasks = CodeBody(extension, "RunPostInstallTasks");

         int registered = tasks.IndexOf("RegisterHMailServerService(FirstRegistrationChoice(", StringComparison.Ordinal);
         int granted = tasks.IndexOf("GrantServiceAccess()", StringComparison.Ordinal);
         int database = tasks.IndexOf("DBSetupQuick.exe", StringComparison.Ordinal);
         int moved = tasks.IndexOf("MoveServiceToVirtualAccount()", StringComparison.Ordinal);
         int started = tasks.IndexOf("'START hMailServer'", StringComparison.Ordinal);

         Assert.GreaterOrEqual(registered, 0, "RunPostInstallTasks no longer registers the service.");
         Assert.GreaterOrEqual(granted, 0, "RunPostInstallTasks does not grant a service that already has an account of its own its folders.");
         Assert.GreaterOrEqual(moved, 0, "RunPostInstallTasks does not move the service to the virtual account.");
         Assert.Less(registered, granted, "The folders are granted before the service is registered.");
         Assert.Less(granted, database,
            "A service that already runs as an account of its own is granted its folders after DBSetupQuick, which reaches the server over COM and so starts the service.");
         Assert.Less(database, moved,
            "The service is moved to the virtual account before the database step, which writes [Database] into hMailServer.INI through the service - a file the virtual account may not write.");
         Assert.Less(moved, started, "The service is moved after it is started, so it runs as the old account until the next start.");

         Assert.IsTrue(Regex.IsMatch(tasks, @"if\s+bServiceRegistered\s+and\s+ServiceNeedsGrants\(GetServiceAccountChoice\(\)\)\s+and\s+\(MoveServiceAfterDatabase\(GetServiceAccountChoice\(\)\)\s*=\s*false\)\s+then", RegexOptions.IgnoreCase),
            "The grant before the database step is not tied to a service that already runs as an account of its own.");
         Assert.IsTrue(Regex.IsMatch(tasks, @"if\s*\(\s*GrantServiceAccess\(\)\s*=\s*false\s*\)\s*then\s*Result\s*:=\s*false", RegexOptions.IgnoreCase),
            "A grant that fails does not fail the installation, which then ends on Completed with a service that may not start.");
         Assert.IsTrue(Regex.IsMatch(tasks, @"if\s+bServiceRegistered\s+and\s+Result\s+and\s+MoveServiceAfterDatabase\(GetServiceAccountChoice\(\)\)\s+then", RegexOptions.IgnoreCase),
            "The move is not tied to a database step that succeeded.");

         string needs = CodeBody(extension, "ServiceNeedsGrants");
         Assert.IsTrue(Regex.IsMatch(needs, @"Choice\s*=\s*'localsystem'\s+then\s+Result\s*:=\s*false", RegexOptions.IgnoreCase),
            "A service registered as LocalSystem is granted folders it has no use for - and the first grant of a large message store takes minutes.");

         // Moved after the database: a new installation, or an upgrade of a
         // LocalSystem service, registered as LocalSystem - or kept on it - first.
         string after = CodeBody(extension, "MoveServiceAfterDatabase");
         Assert.IsTrue(Regex.IsMatch(after, @"Choice\s*=\s*'virtual'\)\s*and\s*\(IsNewService\(\)\s+or\s+IsLocalSystemAccount\(GetServiceAccountBefore\(\)\)\)", RegexOptions.IgnoreCase),
            "MoveServiceAfterDatabase does not cover a new installation and an upgrade of a LocalSystem service that chose the virtual account.");
         string first = CodeBody(extension, "FirstRegistrationChoice");
         Assert.IsTrue(Regex.IsMatch(first, @"if\s+IsNewService\(\)\s+then\s+Result\s*:=\s*'localsystem'\s+else\s+Result\s*:=\s*'keep'", RegexOptions.IgnoreCase),
            "A service to be moved is not registered as LocalSystem, or kept on it, for the database step.");

         // The move: the grant first, and the registration only on its 0.
         string move = CodeBody(extension, "MoveServiceToVirtualAccount");
         int moveGrant = move.IndexOf("RunGrantServiceAccess(ResultCode)", StringComparison.Ordinal);
         int moveRegister = move.IndexOf("RegisterHMailServerService('virtual')", StringComparison.Ordinal);
         Assert.GreaterOrEqual(moveGrant, 0, "The move does not grant the folders.");
         Assert.Less(moveGrant, moveRegister, "The service is registered as the virtual account before its folders are granted.");
         Assert.IsTrue(Regex.IsMatch(move, @"else\s+if\s*\(\s*ResultCode\s*<>\s*0\s*\)\s*then", RegexOptions.IgnoreCase),
            "The move goes ahead after a grant that did not end in 0 - listed folders not approved, key files outside the grants, a refusal, or No.");
         StringAssert.Contains("StopServiceForMove()", move, "The service is not stopped before it is moved.");

         // Asked in an attended installation, never in a silent one.
         string run = CodeBody(extension, "RunGrantServiceAccess");
         StringAssert.Contains("/GrantServiceAccess", run);
         Assert.IsTrue(Regex.IsMatch(run, @"if\s+not\s+WizardSilent\(\)\s+then\s+szSwitches\s*:=\s*'--confirm\s*'", RegexOptions.IgnoreCase),
            "An attended installation does not let the grant ask about the folders and key files the configuration names outside the grants.");

         foreach (string name in new[] { "GrantServiceAccess", "MoveServiceToVirtualAccount", "RunGrantServiceAccess" })
         {
            Assert.IsFalse(Regex.IsMatch(CodeBody(extension, name), @"(?<!Suppressible)MsgBox\("),
               name + " uses a message box that /SUPPRESSMSGBOXES does not suppress, which a silent installation waits on for ever.");
         }

         // A grant that fails the installation says so, not that the database did.
         StringAssert.Contains("g_bGrantFailed", CodeBody(extension, "CurStepChanged"),
            "A failed grant ends the installation with the database's message.");

         // And the server has the verb and the switches the installer and the
         // Control Panel pass.
         string server = ReadRepositoryFile(@"hmailserver\source\Server\hMailServer\hMailServer.cpp");
         foreach (string word in new[] { "_T(\"/GrantServiceAccess\")", "_T(\"--confirm\")", "_T(\"--approve-modify\")", "_T(\"--approve-read\")", "_T(\"--report-only\")" })
            StringAssert.Contains(word, server, "hMailServer.exe does not know " + word + ".");
      }

      [Test]
      [Description("Every installation and upgrade of the server makes the program's Data, Logs, Temp, Database and Events folders and hMailServer.INI private to SYSTEM, the Administrators and the service - hMailServer.exe /ProtectFolders, whatever account the service runs as, before the database step. Until 6.3.6 they inherited BUILTIN\\Users' Read from Program Files, and every user of the computer could read the mail, the logs, the built-in database and the database's password (the installer smoke VM, 3 October 2026).")]
      public void TheProgramsSecretsAreMadePrivateAtEveryInstallation()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string tasks = CodeBody(extension, "RunPostInstallTasks");

         int registered = tasks.IndexOf("RegisterHMailServerService(FirstRegistrationChoice(", StringComparison.Ordinal);
         int protect = tasks.IndexOf("ProtectFolders();", StringComparison.Ordinal);
         int granted = tasks.IndexOf("GrantServiceAccess()", StringComparison.Ordinal);
         int database = tasks.IndexOf("DBSetupQuick.exe", StringComparison.Ordinal);
         Assert.GreaterOrEqual(protect, 0, "RunPostInstallTasks does not make the program's folders and hMailServer.INI private.");
         Assert.Less(registered, protect, "The folders are made private before the service is registered, which a failed registration then skips nothing of - keep the order the steps are said in.");
         Assert.Less(protect, granted, "The folders are made private after the grant, so a grant that fails the installation leaves them readable by every user.");
         Assert.Less(protect, database, "The folders are made private after the database step, which makes the built-in database and starts the service that writes the logs.");

         // Whatever account the service runs as: no condition stands between the
         // statement before it and the call - a LocalSystem service's folders were
         // as readable as the virtual account's.
         int statementBefore = tasks.LastIndexOf(";", protect - 1, StringComparison.Ordinal);
         string between = Regex.Replace(tasks.Substring(statementBefore + 1, protect - statementBefore - 1), @"//[^\n]*", "");
         Assert.IsFalse(Regex.IsMatch(between, @"\b(if|then|else|ServiceNeedsGrants|MoveServiceAfterDatabase)\b", RegexOptions.IgnoreCase),
            "/ProtectFolders runs only under a condition; it is to run at every installation and upgrade of the server.");

         string run = CodeBody(extension, "ProtectFolders");
         StringAssert.Contains("/ProtectFolders", run);
         Assert.IsFalse(Regex.IsMatch(run, @"(?<!Suppressible)MsgBox\("),
            "ProtectFolders uses a message box that /SUPPRESSMSGBOXES does not suppress, which a silent installation waits on for ever.");
         StringAssert.Contains("Log('hMailServer.exe /ProtectFolders returned", run, "What /ProtectFolders said does not go into the setup log.");

         string server = ReadRepositoryFile(@"hmailserver\source\Server\hMailServer\hMailServer.cpp");
         StringAssert.Contains("_T(\"/ProtectFolders\")", server, "hMailServer.exe does not know /ProtectFolders, which the installer runs.");

         // The verb runs elevated and writes nothing where the service can (R3);
         // it takes its folders from where the program is, never from
         // hMailServer.INI, holds each from the drive's root while it changes it,
         // and gives the service Read on hMailServer.INI, which it reads under
         // every account.
         string account = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         Match verb = Regex.Match(account, @"ServiceAccount::RunProtectCommand\([^)]*\)\s*\{(.*?)\n   \}", RegexOptions.Singleline);
         Assert.IsTrue(verb.Success, "ServiceAccount::RunProtectCommand is not there.");
         string body = verb.Groups[1].Value;
         Assert.IsTrue(Regex.IsMatch(body, @"^(?:\s*//[^\n]*\n)*\s*Logger::ForbidWritesForThisProcess\(\);"),
            "RunProtectCommand does not forbid the log writes of its elevated process first.");
         StringAssert.Contains("ProgramFolderOfThisBinary_(program, executable)", body, "/ProtectFolders does not find the program folder from where it is.");
         foreach (string folder in new[] { "_T(\"Data\")", "_T(\"Logs\")", "_T(\"Temp\")", "_T(\"Database\")", "_T(\"Events\")", "_T(\"hMailServer.INI\")" })
            StringAssert.Contains(folder, body, "/ProtectFolders does not make " + folder + " private.");
         StringAssert.Contains("pin.Pin(target, detail, &guarded)", body, "/ProtectFolders does not hold a folder from the drive's root, through a link only the administrators control, while it changes it.");
         StringAssert.Contains("ProtectFile_(target, sid, secret.serviceReads", body, "hMailServer.INI is not made private through a handle opened without following links.");
         Assert.IsTrue(Regex.IsMatch(body, @"_T\(""hMailServer\.INI""\)\),[^\n]*,\s*false,\s*true\s*\}\);"),
            "hMailServer.INI is made private without the service's Read, and a service moved off LocalSystem could not read its configuration.");
      }

      [Test]
      [Description("Every installation and upgrade of the server hardens the program folder, Bin and ControlPanel - hMailServer.exe /ProtectFolders gives each SYSTEM's and the Administrators' full control and BUILTIN\\Users' Read & execute, inheriting nothing, with the Administrators as owner, after the six are private, and then holds the installation to the rule of an administrator's install of an update - and the installer makes the program folder with those permissions, or hardens one that is there, Bin and ControlPanel with it, before any file is copied (work item 78's R1 to R4, each held by a test of its own below), and says, without refusing it for its permissions, when the chosen folder lets every user change what is in it. Until 6.3.7 an installation at a drive's root inherited Authenticated Users' Modify on a Windows client, so every user of the computer could replace the programs in Bin that the service and the administrators run (work item 78).")]
      public void TheProgramFoldersAreHardenedAtEveryInstallation()
      {
         string account = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         Match verb = Regex.Match(account, @"ServiceAccount::RunProtectCommand\([^)]*\)\s*\{(.*?)\n   \}", RegexOptions.Singleline);
         Assert.IsTrue(verb.Success, "ServiceAccount::RunProtectCommand is not there.");
         string body = verb.Groups[1].Value;

         // The three, found from where the program is, each held from the drive's
         // root while it is hardened.
         StringAssert.Contains("programFolders.push_back({ program,", body, "/ProtectFolders does not harden the program folder.");
         StringAssert.Contains("programFolders.push_back({ bin,", body, "/ProtectFolders does not harden Bin.");
         StringAssert.Contains("programFolders.push_back({ FileUtilities::Combine(program, _T(\"ControlPanel\"))", body, "/ProtectFolders does not harden ControlPanel.");
         StringAssert.Contains("Harden(pin.GrantPath(target), sid, true,", body,
            "The program's folders are not hardened through the pin that holds them from the drive's root, through a link only the administrators control.");

         // After the six, whose own permissions keep the program folder's change
         // from being handed down through the message store - and the program
         // folder not at all while one of them could not be made private.
         int secrets = body.IndexOf("for (const Secret_ &secret : secrets)", StringComparison.Ordinal);
         int hardened = body.IndexOf("for (const ProgramFolder_ &folder : programFolders)", StringComparison.Ordinal);
         Assert.GreaterOrEqual(secrets, 0, "RunProtectCommand no longer makes the six private in one loop.");
         Assert.Greater(hardened, secrets, "The program's folders are hardened before the six are made private, so the program folder's change is handed down through the message store.");
         StringAssert.Contains("secretFolderFailed && SamePath_(target, program)", body,
            "The program folder is hardened while a folder in it could not be made private.");

         // Then the rule of an administrator's install of an update: what the
         // hardening makes passes it, and what it leaves is a CHECK.
         int rule = body.IndexOf("AdminUpdateChecks::CheckInstallation(", StringComparison.Ordinal);
         Assert.Greater(rule, hardened, "/ProtectFolders does not hold the hardened installation to the rule of an administrator's install of an update.");

         // The shape: a fresh Read & execute for BUILTIN\Users, whatever the folder
         // above handed them, where every user could read the folder before
         // (EveryUserReads_), so that the Control Panel and DBSetup still run for
         // them and nothing becomes every user's to read that was not;
         // nothing inherited; and the Administrators as the owner of a folder somebody
         // else owns, since an owner may always change the permissions again.
         Match rebuilt = Regex.Match(account, @"PACL RebuiltAcl_\([^)]*\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.IsTrue(rebuilt.Success, "RebuiltAcl_ is not there.");
         StringAssert.Contains("WinBuiltinUsersSid", rebuilt.Groups[1].Value);
         StringAssert.Contains("AddAccessAllowedAceEx(acl, revision, handDown, ServiceAccount::RightsReadExecute, users)", rebuilt.Groups[1].Value,
            "A hardened folder does not give BUILTIN\\Users Read & execute, and the Control Panel and DBSetup would no longer run for them.");
         Match harden = Regex.Match(account, @"ServiceAccount::Harden\([^)]*\)\s*\{(.*?)\n   \}", RegexOptions.Singleline);
         Assert.IsTrue(harden.Success, "ServiceAccount::Harden is not there.");
         foreach (string word in new[] { "HardenedAcl_(", "PROTECTED_DACL_SECURITY_INFORMATION", "OWNER_SECURITY_INFORMATION", "AlreadyHardened_(" })
            StringAssert.Contains(word, harden.Groups[1].Value, "ServiceAccount::Harden does not use " + word + ".");

         // The installer: its message names what the verb now hardens.
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         StringAssert.Contains("the program folder, Bin and ControlPanel", CodeBody(extension, "ProtectFolders"),
            "The installer's message for a /ProtectFolders that did not end in 0 does not name the program folder, Bin and ControlPanel.");

         // Before the files: PrepareToInstall makes the program folder with those
         // permissions, or hardens it, Bin and ControlPanel, as its last step
         // (PrepareProgramFolders; work item 78's R1 to R4, each held by a test of its
         // own below) - not [Dirs] or ssInstall, after Inno Setup has made the folder.
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "PrepareToInstall"), @"Result\s*:=\s*PrepareProgramFolders\(\);\s*if\s+Result\s*<>\s*''\s*then\s*Log\('Refused: '\s*\+\s*Result\);\s*end;\s*$"),
            "PrepareToInstall does not make or harden the program folder, Bin and ControlPanel as its last step, before any file is copied.");

         // The chosen folder is checked and said - in a box that /SUPPRESSMSGBOXES
         // suppresses, and in the setup log - and never refused for its permissions.
         StringAssert.Contains("CheckProgramFolderPermissions(WizardDirValue(), true)", CodeBody(extension, "NextButtonClick"),
            "The folder page does not say when the chosen folder lets every user change what is in it.");
         StringAssert.Contains("CheckProgramFolderPermissions(szApp, false)", CodeBody(extension, "PrepareProgramFolders"),
            "A silent installation, or an upgrade, whose folder page was not shown does not say in the setup log when the folder lets every user change what is in it.");
         Assert.IsTrue(Regex.IsMatch(extension, @"^procedure\s+CheckProgramFolderPermissions\s*\(", RegexOptions.Multiline),
            "CheckProgramFolderPermissions is not a procedure: it says, and does not refuse the folder.");
         string warn = CodeBody(extension, "CheckProgramFolderPermissions");
         Assert.IsFalse(Regex.IsMatch(warn, @"(?<!Suppressible)MsgBox\("),
            "CheckProgramFolderPermissions uses a message box that /SUPPRESSMSGBOXES does not suppress, which a silent installation waits on for ever.");
         StringAssert.Contains("Log('The program folder: '", warn, "What CheckProgramFolderPermissions says does not go into the setup log.");
         StringAssert.Contains(HmTrustedInstallerSid, extension,
            "The installer does not take TrustedInstaller for a trusted owner, as ServiceAccount.cpp's IsTrustedWriter_ does.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "IsTrustedSid"), @"SidIs\(pSid,\s*'S-1-5-18'\)\s*or\s*SidIs\(pSid,\s*'S-1-5-32-544'\)\s*or\s*SidIs\(pSid,\s*HM_TRUSTED_INSTALLER_SID\)"),
            "IsTrustedSid does not take SYSTEM, the Administrators and TrustedInstaller, ServiceAccount.cpp's IsTrustedWriter_.");
         StringAssert.Contains("Result := IsTrustedSid(pOwner);", CodeBody(extension, "FolderOwnerIsTrusted"),
            "FolderOwnerIsTrusted does not take SYSTEM, the Administrators and TrustedInstaller for a trusted owner.");
      }

      private const string HmTrustedInstallerSid = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

      /// <summary>
      ///    The new functions of work item 78's R1 to R4 in the installer, each of
      ///    which an attended installation and a silent one both run: none may use a
      ///    message box that /SUPPRESSMSGBOXES does not suppress.
      /// </summary>
      private static void AssertNoPlainMessageBox(string extension, params string[] names)
      {
         foreach (string name in names)
         {
            Assert.IsFalse(Regex.IsMatch(CodeBody(extension, name), @"(?<!Suppressible)MsgBox\("),
               name + " uses a message box that /SUPPRESSMSGBOXES does not suppress, which a silent installation waits on for ever.");
         }
      }

      [Test]
      [Description("Work item 78, R1: setup makes a new program folder itself in PrepareToInstall, before Inno Setup makes or copies anything, with permissions of its own from the moment it is made - CreateDirectoryW with a security descriptor that gives SYSTEM and the Administrators full control, Users Read & execute where every user could read a folder made there, inherits nothing and makes the Administrators the owner - and the folders above it that were not there with it; the uninstaller is given each to remove when empty (section_dirs.iss), and those still empty are removed when the installation does not reach its files. In 6.3.7 and earlier Inno Setup made the folder with what the folder above handed down, and icacls replaced that about 45 ms later (the installer smoke VM, 6 October 2026).")]
      public void ANewProgramFolderIsMadeWithPermissionsOfItsOwnFromTheStart()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         // CreateDirectoryW with a SECURITY_ATTRIBUTES, and the descriptor by SID.
         Assert.IsTrue(Regex.IsMatch(extension, @"function\s+MakeFolderWithSecurity\(lpPathName:\s*String;\s*var\s+lpSecurityAttributes:\s*HM_SECURITY_ATTRIBUTES\):\s*Boolean;\s*external\s+'CreateDirectoryW@kernel32\.dll stdcall';"),
            "The installer does not declare CreateDirectoryW with a SECURITY_ATTRIBUTES, the only way a folder is made with permissions of its own from its first moment.");
         string make = CodeBody(extension, "MakeProgramFolder");
         foreach (string word in new[] { "'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)'", "'(A;OICI;0x1200a9;;;BU)'", "EveryUserMayReadMadeIn(szPath)", "SecurityDescriptorOfText(szDescriptor, 1, pDescriptor, 0)",
                                         "attributes.lpSecurityDescriptor := pDescriptor;", "MakeFolderWithSecurity(missing[index], attributes)", "g_aFoldersMade", "HoldForSetup(missing[index])",
                                         "HM_ERROR_ALREADY_EXISTS", "g_bProgramFolderNew := Result;" })
            StringAssert.Contains(word, make, "MakeProgramFolder does not use " + word + ".");
         // Every folder not there, deepest found first and made from the top down.
         Assert.IsTrue(Regex.IsMatch(make, @"while\s*\(szPath\s*<>\s*''\)\s*and\s*\(not\s+DirExists\(szPath\)\)\s*do") && Regex.IsMatch(make, @"index\s*:=\s*count\s*-\s*1;\s*while\s+Result\s+and\s+\(index\s*>=\s*0\)\s+do"),
            "MakeProgramFolder does not make every folder above the program folder that is not there, from the top down.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_ERROR_ALREADY_EXISTS\s*=\s*183;"), "ERROR_ALREADY_EXISTS is not 183: a folder somebody else made since setup looked would be taken.");
         StringAssert.Contains("(A;OICI;0x1200a9;;;BU)", make);
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "EveryUserMayReadMadeIn"), @"HM_CONTAINER_INHERIT\)\s*<>\s*0"),
            "Whether every user could read a folder made there is not read from what the folder above hands down to folders.");

         // Made in PrepareToInstall's last step, when the folder is not there.
         string prepare = CodeBody(extension, "PrepareProgramFolders");
         Assert.IsTrue(Regex.IsMatch(prepare, @"if\s+not\s+DirExists\(szReal\)\s+then\s+begin\s+if\s+not\s+MakeProgramFolder\(szReal,\s*szWhy\)\s+then"),
            "PrepareProgramFolders does not make a program folder that is not there.");

         // Inno Setup no longer makes it, and nothing hardens it after it is made.
         Assert.IsFalse(Regex.IsMatch(extension, @"^procedure\s+HardenNewProgramFolder\s*\(", RegexOptions.Multiline),
            "HardenNewProgramFolder is back: the folder Inno Setup made would carry what the folder above hands down until icacls ran.");
         Assert.IsFalse(Regex.IsMatch(CodeBody(extension, "CurStepChanged"), @"g_bProgramFolderNew\s*:="),
            "Whether this installation made the program folder is decided at ssInstall again, after PrepareToInstall made it.");

         // [Dirs]: the folders setup made, for the uninstaller - the eight above the
         // program folder, shallowest first, then the program folder - each recorded
         // although it was there when [Dirs] ran, and only when this setup made it.
         string dirs = ReadInstallerScript("section_dirs.iss");
         List<Match> entries = Regex.Matches(dirs, @"^Name:\s*""([^""]+)""([^\r\n]*)", RegexOptions.Multiline).Cast<Match>().ToList();
         Assert.GreaterOrEqual(entries.Count, 9, "section_dirs.iss has too few entries.");
         for (int index = 0; index < 8; index++)
         {
            Assert.AreEqual("{code:FolderMadeBySetup|" + index + "}", entries[index].Groups[1].Value, "Entry " + index + " of [Dirs] is not the folder above the program folder setup made, " + index + " from the top.");
            StringAssert.Contains("Flags: uninsalwaysuninstall", entries[index].Groups[2].Value, "A folder setup made above the program folder is not recorded for the uninstaller.");
            StringAssert.Contains("Check: SetupMadeFolder(" + index + ")", entries[index].Groups[2].Value, "A folder above the program folder that setup did not make would be removed by the uninstaller.");
         }
         Assert.AreEqual("{app}", entries[8].Groups[1].Value, "The program folder is not recorded after the folders above it, which the uninstaller removes after it.");
         StringAssert.Contains("Flags: uninsalwaysuninstall", entries[8].Groups[2].Value, "The program folder setup made is not recorded for the uninstaller.");
         StringAssert.Contains("Check: SetupMadeProgramFolder", entries[8].Groups[2].Value, "A program folder that was there before would be recorded for the uninstaller.");
         Assert.IsFalse(dirs.Contains("AfterInstall:"), "[Dirs] gives a folder its permissions after Inno Setup has made it.");
         foreach (string name in new[] { "FolderMadeBySetup", "SetupMadeFolder", "SetupMadeProgramFolder" })
            CodeBody(extension, name);
         StringAssert.Contains("Result := ProgramFolderMadeAsNamed();", CodeBody(extension, "SetupMadeProgramFolder"));
         // Made where an administrators' link leads, the program folder is given to the
         // uninstaller by its own path, not as {app}, whose removal would take the link.
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "ProgramFolderMadeAsNamed"), @"CompareText\(g_szProgramFolderReal,\s*RemoveBackslashUnlessRoot\(ExpandConstant\('\{app\}'\)\)\)\s*=\s*0"),
            "A program folder made where a link leads is given to the uninstaller as {app}.");

         // Not reaching the files - a cancel, a failure, a stop before them - removes
         // what setup made that is still empty, deepest first; and going back to the
         // folder page and on again removes what the earlier pass made.
         string deinitialize = CodeBody(extension, "DeinitializeSetup");
         Assert.IsTrue(Regex.IsMatch(deinitialize, @"ReleaseHeldFolders\(\);\s*if\s+not\s+g_bFilesInstalled\s+then\s+RemoveFoldersMadeHere\(\);"),
            "DeinitializeSetup does not let the held folders go and remove the empty folders setup made when the files were not installed.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "CurStepChanged"), @"CurStep\s*=\s*ssPostInstall\s+then\s+begin\s+(//[^\n]*\n\s*)*g_bFilesInstalled\s*:=\s*true;"),
            "The installation does not mark its files installed at ssPostInstall, so DeinitializeSetup would remove a folder it made, or leave one empty.");
         string remove = CodeBody(extension, "RemoveFoldersMadeHere");
         Assert.IsTrue(Regex.IsMatch(remove, @"for\s+index\s*:=\s*GetArrayLength\(g_aFoldersMade\)\s*-\s*1\s+downto\s+0\s+do") && remove.Contains("RemoveDir(g_aFoldersMade[index])"),
            "RemoveFoldersMadeHere does not remove the folders setup made deepest first, and only when empty (RemoveDir).");
         Assert.IsTrue(Regex.IsMatch(prepare, @"^\s*ReleaseHeldFolders\(\);\s*RemoveFoldersMadeHere\(\);", RegexOptions.Multiline),
            "A second pass of PrepareToInstall, after the folder page was gone back to, keeps what the first held and made.");

         AssertNoPlainMessageBox(extension, "MakeProgramFolder", "RemoveFoldersMadeHere", "DeinitializeSetup");
      }

      [Test]
      [Description("Work item 78, R2: a program folder, Bin and ControlPanel that are there before the installation - a server installed into a folder made beforehand, an upgrade, a repair, the Control Panel alone - are hardened in PrepareToInstall, before any file is copied and before setup runs any program of the installation; the program folder of a server installation only when its Data, Logs, Temp, Database and Events inherit nothing from it. One another account owns is given to the Administrators with the entries of its own that give no right to write kept, the service's Read & execute on Bin among them, as ServiceAccount.cpp's RebuiltAcl_ keeps them. In 6.3.7 and earlier an upgrade copied Bin's files and ran hMailServer.exe /Register from it before /ProtectFolders hardened it (the installer smoke VM, 6 October 2026).")]
      public void FoldersThatAreThereAreHardenedBeforeTheFiles()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string prepare = CodeBody(extension, "PrepareProgramFolders");

         // Every installation: no condition on the components, the server's included.
         Assert.IsFalse(prepare.Contains("IsComponentSelected("),
            "PrepareProgramFolders depends on the components chosen; a server installation into a folder that is there would not be hardened before its files.");
         int bin = prepare.IndexOf("HardenHeldFolder(szBin)", StringComparison.Ordinal);
         int controlPanel = prepare.IndexOf("HardenHeldFolder(szControlPanel)", StringComparison.Ordinal);
         int app = prepare.IndexOf("HardenHeldFolder(szReal)", StringComparison.Ordinal);
         Assert.GreaterOrEqual(bin, 0, "Bin is not hardened before its files are copied.");
         Assert.GreaterOrEqual(controlPanel, 0, "ControlPanel is not hardened before its files are copied.");
         Assert.GreaterOrEqual(app, 0, "The program folder is not hardened before the files.");

         // The program folder of a server installation only when the data folders
         // inherit nothing from it, so that its change is not handed down through the
         // message store.
         Assert.IsTrue(Regex.IsMatch(prepare, @"if\s+DirExists\(AddBackslash\(szReal\)\s*\+\s*'Data'\)\s+or\s+FileExists\(AddBackslash\(szBin\)\s*\+\s*'hMailServer\.INI'\)\s+then\s+szInherits\s*:=\s*DataFolderThatInherits\(szReal\);\s*if\s+szInherits\s*=\s*''\s+then\s+HardenHeldFolder\(szReal\)"),
            "The program folder of a server installation is hardened before the files whatever its data folders inherit, or never.");
         string inherits = CodeBody(extension, "DataFolderThatInherits");
         foreach (string folder in new[] { "'Data'", "'Logs'", "'Temp'", "'Database'", "'Events'" })
            StringAssert.Contains(folder, inherits, "DataFolderThatInherits does not look at " + folder + ".");
         StringAssert.Contains("InheritsNothing(szFolder)", inherits);
         string nothing = CodeBody(extension, "InheritsNothing");
         StringAssert.Contains("ControlOfDescriptor(pDescriptor, control, revision)", nothing);
         StringAssert.Contains("HM_DACL_PROTECTED", nothing);
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_DACL_PROTECTED\s*=\s*\$1000;"), "SE_DACL_PROTECTED is not $1000.");

         // In PrepareToInstall, before any file: nothing of it is left at ssInstall,
         // which comes after Inno Setup has started on the folders.
         string step = CodeBody(extension, "CurStepChanged");
         Assert.IsFalse(Regex.IsMatch(step, @"Harden\w*\("), "A program folder that is there is hardened at ssInstall again rather than in PrepareToInstall.");

         // A folder another account owns: rebuilt, the entries that give no right to
         // write kept, the Administrators the owner - not a list of three entries,
         // which drops the service's Read & execute on Bin and would stop the service.
         string held = CodeBody(extension, "HardenHeldFolder");
         StringAssert.Contains("Result := RebuildFolderOfUntrustedOwner(Folder, bUsers);", held,
            "HardenHeldFolder does not rebuild a folder another account owns with the entries that give no right to write kept.");
         StringAssert.DoesNotContain("O:BAD:PAI", held, "HardenHeldFolder gives a folder another account owns a list of SYSTEM, the Administrators and Users alone, dropping the service's entries.");
         foreach (string word in new[] { "/inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F", "*S-1-5-32-545:(OI)(CI)RX", "EveryUserMayRead(Folder)", "FolderOwnerIsTrusted(Folder, bKnown)" })
            StringAssert.Contains(word, held, "HardenHeldFolder does not use " + word + ".");
         string rebuild = CodeBody(extension, "RebuildFolderOfUntrustedOwner");
         Assert.IsTrue(Regex.IsMatch(rebuild, @"if\s+pass\s*=\s*0\s+then\s+begin\s+if\s+aceType\s*=\s*HM_ACCESS_DENIED\s+then\s+ok\s*:=\s*AddAsOwnEntry\(pNew,\s*pAce\);"),
            "RebuildFolderOfUntrustedOwner does not keep every deny entry, first.");
         Assert.IsTrue(Regex.IsMatch(rebuild, @"else\s+if\s+writes\s+and\s+\(not\s+IsTrustedSid\(pAce\s*\+\s*8\)\)\s+then\s+szDropped\s*:=[^;]*;?\s*else\s+ok\s*:=\s*AddAsOwnEntry\(pNew,\s*pAce\)"),
            "RebuildFolderOfUntrustedOwner does not keep the entries of the folder's own that give no right to write, or keeps one that lets another account write.");
         Assert.IsTrue(Regex.IsMatch(rebuild, @"SidIs\(pAce\s*\+\s*8,\s*HM_SERVICE_SID\)\s+and\s+\(not\s+writes\)\s+then\s+ok\s*:=\s*AddAsOwnEntry\(pNew,\s*pAce\)"),
            "RebuildFolderOfUntrustedOwner does not keep the service's inherited Read, as RebuiltAcl_ does.");
         foreach (string word in new[] { "HM_FOLDER_WRITE_RIGHTS", "HM_OWNER_AND_PROTECTED_DACL", "SidOfText('S-1-5-32-544', pAdministrators)", "HM_RIGHTS_READ_EXECUTE", "HM_FILE_ALL_ACCESS" })
            StringAssert.Contains(word, rebuild, "RebuildFolderOfUntrustedOwner does not use " + word + ".");
         StringAssert.Contains("HM_INHERITED_FLAG_BIT", CodeBody(extension, "AddAsOwnEntry"), "An entry kept is not made the folder's own.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_INHERITED_FLAG_BIT\s*=\s*\$1000;") && Regex.IsMatch(extension, @"HM_ACCESS_DENIED\s*=\s*1;") && Regex.IsMatch(extension, @"HM_FILE_ALL_ACCESS\s*=\s*\$1F01FF;") &&
                       Regex.IsMatch(extension, @"HM_RIGHTS_READ_EXECUTE\s*=\s*\$1200A9;"),
            "A constant of the rebuilt list is not the Windows value.");

         // The service's SID is the one the server computes for NT SERVICE\hMailServer.
         string account = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         Match serviceSid = Regex.Match(account, @"ServiceAccount::ServiceSid\(_T\(""hMailServer""\)\)\s*!=\s*_T\(""(S-1-5-80-[0-9-]+)""\)");
         Assert.IsTrue(serviceSid.Success, "ServiceAccountTester no longer holds NT SERVICE\\hMailServer's SID to a value.");
         Assert.IsTrue(extension.Contains("HM_SERVICE_SID              = '" + serviceSid.Groups[1].Value + "';"),
            "HM_SERVICE_SID is not NT SERVICE\\hMailServer's SID, " + serviceSid.Groups[1].Value + ".");

         AssertNoPlainMessageBox(extension, "PrepareProgramFolders", "HardenHeldFolder", "RebuildFolderOfUntrustedOwner", "DataFolderThatInherits", "InheritsNothing");
      }

      [Test]
      [Description("Work item 78, R3: setup holds the program folder, Bin and ControlPanel from the root of their volume down, as itself and not shared for deleting, from PrepareToInstall until DeinitializeSetup, and refuses - before anything changes, exit code 7 when silent - a junction, symbolic link or mount point on the way that anybody but SYSTEM, the Administrators and TrustedInstaller owns or may change or remove (ProtectedFile.cpp's OnlyAdministratorsControl, read through the handle that holds it), saying how to make it the administrators' alone; one they alone control is gone through and the path walked again from where it leads. In 6.3.7 and earlier setup installed through any link (the installer smoke VM, 6 October 2026: through a junction a standard account owned).")]
      public void ALinkOnTheWayThatAnybodyButTheAdministratorsControlsIsRefused()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         // The hold: listing, traversing, the attributes and READ_CONTROL - so that a
         // link's owner and permissions are read through it - shared for reading and
         // writing, not deleting, the link opened itself.
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_HOLD_FOLDER_ACCESS\s*=\s*\$200A1;"),
            "The hold does not ask for READ_CONTROL, so a link's owner and permissions are not read through the handle that holds it.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_SHARE_ALL_BUT_DELETE\s*=\s*\$3;") && Regex.IsMatch(extension, @"HM_HOLD_FOLDER_FLAGS\s*=\s*\$02200000;"),
            "A folder is held shared for deleting, or with its link followed.");
         string hold = CodeBody(extension, "HoldForSetup");
         StringAssert.Contains("HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0)", hold);
         StringAssert.Contains("g_aHeldFolders", hold, "A held folder is not kept until DeinitializeSetup.");
         StringAssert.Contains("ReleaseHeldFolders();", CodeBody(extension, "DeinitializeSetup"), "The held folders are not let go at the end.");

         // The chain: every folder held from the root down; a link only through the
         // rule, and then walked again from where it leads, at most eight deep.
         string chain = CodeBody(extension, "HoldFolderChain");
         foreach (string word in new[] { "HoldForSetup(szRoot)", "HoldForSetup(szNext)", "HM_REPARSE_POINT", "LinkIsAdministratorsAlone(handle, parent, szWhy)", "LinkTarget(handle, szNext, szTarget, szWhy)",
                                         "HoldFolderChain(szTarget, Depth + 1, Real, Why)", "Depth >= 8", "LinkRemedy(szNext)", "KindOfDrive(szRoot) = HM_DRIVE_REMOTE", "parent := handle;" })
            StringAssert.Contains(word, chain, "HoldFolderChain does not use " + word + ".");
         Assert.IsTrue(chain.IndexOf("HoldForSetup(szNext)", StringComparison.Ordinal) < chain.IndexOf("LinkIsAdministratorsAlone(", StringComparison.Ordinal),
            "A link is looked at before it is held.");
         string remedy = CodeBody(extension, "LinkRemedy");
         StringAssert.Contains("/L /setowner *S-1-5-32-544", remedy, "The refusal does not say how to give the link to the Administrators.");
         StringAssert.Contains("/L /inheritance:r /grant:r *S-1-5-18:F *S-1-5-32-544:F *S-1-5-32-545:RX", remedy, "The refusal does not say how to give the link a list only the administrators may change.");

         // The rule, through the held handles: the owner and the list of the link and
         // of the folder it is in.
         string rule = CodeBody(extension, "LinkIsAdministratorsAlone");
         Assert.AreEqual(2, Regex.Matches(rule, @"ReadHeldSecurity\(h(Link|Parent),\s*HM_FILE_OBJECT,\s*HM_OWNER_AND_DACL_INFORMATION").Count,
            "The link's and its folder's owner and permissions are not read through the handles that hold them.");
         Assert.IsFalse(Regex.IsMatch(rule, @"ReadFolder(Permissions|Owner)\("), "A link's permissions are read by its path, which names whatever is there by then.");
         Assert.AreEqual(2, Regex.Matches(rule, @"not\s+IsTrustedSid\(pOwner\)").Count, "The owner of the link, or of the folder it is in, is not held to SYSTEM, the Administrators and TrustedInstaller.");
         StringAssert.Contains("AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho)", rule);
         StringAssert.Contains("AclLetsOthers(pDacl, HM_REMOVE_FROM_FOLDER_RIGHTS, true, szWho)", rule);
         Assert.IsTrue(Regex.IsMatch(extension, @"external\s+'GetSecurityInfo@advapi32\.dll stdcall';"), "GetSecurityInfo, which reads through a handle, is not declared.");

         // The rights: every right on the link that ProtectedFile.cpp's
         // OnlyAdministratorsControl counts, and on its folder removing it or taking
         // the folder's permissions.
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_FOLDER_WRITE_RIGHTS\s*=\s*\$500D0156;"), "HM_FOLDER_WRITE_RIGHTS is not $500D0156.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_REMOVE_FROM_FOLDER_RIGHTS\s*=\s*\$100C0040;"), "HM_REMOVE_FROM_FOLDER_RIGHTS is not FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL.");
         string protectedFile = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ProtectedFile.cpp");
         string onIt = CppFunctionCode(protectedFile, "OnlyAdministratorsControl(", "ProtectedFile.cpp");
         var rights = new Dictionary<string, uint> { { "FILE_WRITE_DATA", 0x2 }, { "FILE_APPEND_DATA", 0x4 }, { "FILE_WRITE_EA", 0x10 }, { "FILE_WRITE_ATTRIBUTES", 0x100 },
                                                     { "FILE_DELETE_CHILD", 0x40 }, { "DELETE", 0x10000 }, { "WRITE_DAC", 0x40000 }, { "WRITE_OWNER", 0x80000 } };
         Match linkMask = Regex.Match(onIt, @"onIt\s*=\s*([A-Z_ |]+);");
         Assert.IsTrue(linkMask.Success, "OnlyAdministratorsControl no longer names the rights it counts on the link.");
         foreach (string name in linkMask.Groups[1].Value.Split('|').Select(part => part.Trim()))
         {
            Assert.IsTrue(rights.ContainsKey(name), "OnlyAdministratorsControl counts " + name + ", which this test does not know.");
            Assert.AreNotEqual(0u, 0x500D0156u & rights[name], "The installer does not count " + name + " on a link, which the server does.");
         }

         // Where a link leads, read from the link (FSCTL_GET_REPARSE_POINT), not from
         // an open through it, which follows every link after it unasked; a share is
         // not gone to.
         string target = CodeBody(extension, "LinkTarget");
         StringAssert.Contains("ReadReparseData(hLink, HM_GET_REPARSE_POINT", target);
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_GET_REPARSE_POINT\s*=\s*\$000900A8;"), "FSCTL_GET_REPARSE_POINT is not $000900A8.");
         StringAssert.Contains("'\\??\\UNC\\'", target, "A link to a share is gone through.");
         Assert.IsTrue(target.Contains("(tagHigh = $A000) and (tagLow = $0003)") && target.Contains("(tagHigh = $A000) and (tagLow = $000C)"),
            "LinkTarget does not know a mount point's and a symbolic link's tags, and nothing else.");

         // OWNER RIGHTS is the owner's, trusted only with a trusted owner.
         StringAssert.Contains("if not (OwnerTrusted and SidIs(pAce + 8, 'S-1-3-4')) then", CodeBody(extension, "AclLetsOthers"));

         // PrepareToInstall refuses - its answer, exit 7 when silent - for the program
         // folder, Bin and ControlPanel alike, before it changes anything.
         string prepare = CodeBody(extension, "PrepareProgramFolders");
         foreach (string path in new[] { "szApp, 0, szReal, szWhy", "AddBackslash(szApp) + 'Bin', 0, szBin, szWhy", "AddBackslash(szApp) + 'ControlPanel', 0, szControlPanel, szWhy" })
            StringAssert.Contains("if not HoldFolderChain(" + path + ") then", prepare, "PrepareProgramFolders does not hold and check " + path + ".");
         int lastChain = prepare.LastIndexOf("HoldFolderChain(", StringComparison.Ordinal);
         foreach (string change in new[] { "MakeProgramFolder(", "HardenHeldFolder(", "SayForeignOwners(" })
            Assert.Greater(prepare.IndexOf(change, StringComparison.Ordinal), lastChain, change + " comes before every link on the way is checked.");
         Assert.AreEqual(3, Regex.Matches(prepare, @"Result\s*:=\s*'Setup does not install into '\s*\+\s*szApp\s*\+\s*':\s*'\s*\+\s*szWhy\s*\+\s*'\.\s*Nothing has been changed\.';").Count,
            "A refusal of a link does not say that nothing has been changed.");
         Assert.IsFalse(Regex.IsMatch(prepare, @"ReleaseFolder\("), "PrepareProgramFolders lets a folder go before the installation ends.");

         AssertNoPlainMessageBox(extension, "HoldFolderChain", "LinkIsAdministratorsAlone", "LinkTarget", "HoldForSetup");
      }

      [Test]
      [Description("Work item 78, the volumes setup does not install into: a program folder on a share of another computer or a network drive, whose permissions that computer sets, or on a volume that keeps no permissions (FAT, exFAT), where every account may change every program - refused in PrepareToInstall before anything changes, with what to do. In 6.3.7 and earlier setup installed there and could not harden anything.")]
      public void AProgramFolderNoPermissionsCanProtectIsRefused()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string prepare = CodeBody(extension, "PrepareProgramFolders");
         Assert.IsTrue(Regex.IsMatch(prepare, @"if\s+not\s+VolumeKeepsPermissions\(handle\)\s+then\s+begin\s+Result\s*:=\s*'Setup does not install into '"),
            "A program folder on a volume that keeps no permissions is not refused.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "VolumeKeepsPermissions"), @"\(flags\s+and\s+HM_PERSISTENT_ACLS\)\s*<>\s*0") && Regex.IsMatch(extension, @"HM_PERSISTENT_ACLS\s*=\s*\$8;"),
            "Whether a volume keeps permissions is not FILE_PERSISTENT_ACLS.");
         Assert.IsTrue(Regex.IsMatch(extension, @"external\s+'GetVolumeInformationByHandleW@kernel32\.dll stdcall';"), "The volume is not read through the held folder.");
         string split = CodeBody(extension, "SplitAtRoot");
         Assert.IsTrue(Regex.IsMatch(split, @"else\s+if\s+Copy\(Path,\s*1,\s*2\)\s*=\s*'\\\\'\s+then\s+Why\s*:="), "A program folder on a share is not refused.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_DRIVE_REMOTE\s*=\s*4;"), "DRIVE_REMOTE is not 4.");
         StringAssert.Contains("Nothing has been changed.", prepare);
      }

      [Test]
      [Description("Work item 78, R4: what an administrator is to look at in a program folder that is there is said in the setup log, and in one box in an attended installation, with what to do: an entry of a folder's own that lets a group of every user write, which setup keeps on a folder the administrators own as ServiceAccount::Harden keeps it, and a file or folder directly inside Bin or ControlPanel owned by an account other than SYSTEM, the Administrators or TrustedInstaller, read through a handle on each before anything changes. In 6.3.7 and earlier neither was said.")]
      public void WhatAnAdministratorIsToLookAtInTheProgramFolderIsSaid()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         // An entry of its own that lets every user write, after the hardening that
         // keeps it.
         string held = CodeBody(extension, "HardenHeldFolder");
         Assert.Greater(held.IndexOf("SayBroadEntriesKept(Folder);", StringComparison.Ordinal), held.IndexOf("icacls hardened", StringComparison.Ordinal),
            "The entries a trusted owner's folder keeps through icacls are not said after icacls.");
         string broad = CodeBody(extension, "SayBroadEntriesKept");
         foreach (string word in new[] { "IsBroadSid(pAce + 8)", "HM_FOLDER_WRITE_RIGHTS", "Log('The program folder: ' + szText);", "g_szFolderFindings", "/remove:g *" })
            StringAssert.Contains(word, broad, "SayBroadEntriesKept does not use " + word + ".");

         // What another account owns directly inside Bin and ControlPanel, read before
         // the hardening.
         string prepare = CodeBody(extension, "PrepareProgramFolders");
         int binOwners = prepare.IndexOf("SayForeignOwners(szBin);", StringComparison.Ordinal);
         int controlPanelOwners = prepare.IndexOf("SayForeignOwners(szControlPanel);", StringComparison.Ordinal);
         Assert.GreaterOrEqual(binOwners, 0, "The owners of what is in Bin are not looked at.");
         Assert.GreaterOrEqual(controlPanelOwners, 0, "The owners of what is in ControlPanel are not looked at.");
         int firstHarden = prepare.IndexOf("HardenHeldFolder(", StringComparison.Ordinal);
         Assert.Less(binOwners, firstHarden, "The owners in Bin are read after setup has changed the folders.");
         Assert.Less(controlPanelOwners, firstHarden, "The owners in ControlPanel are read after setup has changed the folders.");
         string owners = CodeBody(extension, "SayForeignOwners");
         foreach (string word in new[] { "FindFirst(AddBackslash(Folder) + '*', rec)", "HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0)",
                                         "ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_INFORMATION", "not IsTrustedSid(pOwner)", "Log('The program folder: ' + szText);", "/setowner *S-1-5-32-544", "FindClose(rec)" })
            StringAssert.Contains(word, owners, "SayForeignOwners does not use " + word + ".");

         // Said in one box when somebody is there to read it.
         Assert.IsTrue(Regex.IsMatch(prepare, @"if\s+\(g_szFolderFindings\s*<>\s*''\)\s+and\s+\(not\s+WizardSilent\(\)\)\s+then\s+SuppressibleMsgBox\("),
            "An attended installation does not show what setup found in the program folder.");
         AssertNoPlainMessageBox(extension, "SayBroadEntriesKept", "SayForeignOwners");
      }

      [Test]
      [Description("Before the move to the virtual account the installer gives NT SERVICE\\hMailServer its login on SQL Server with Windows authentication (hMailServer.exe --grant-database-login), and a service it moved is held to opening its database and moved back to LocalSystem when it does not, failing the installation in words of its own. Until 6.3.6 a new installation on SQL Server with Windows authentication ended with exit 0 on a server that could not open its database (the installer smoke VM, 2 October 2026).")]
      public void TheMovedServiceIsGivenItsSqlServerLoginAndHeldToOpeningItsDatabase()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         string move = CodeBody(extension, "MoveServiceToVirtualAccount");
         int login = move.IndexOf("GrantServiceDatabaseLogin()", StringComparison.Ordinal);
         int stop = move.IndexOf("StopServiceForMove()", StringComparison.Ordinal);
         int register = move.IndexOf("RegisterHMailServerService('virtual')", StringComparison.Ordinal);
         Assert.GreaterOrEqual(login, 0, "The move no longer gives the virtual account its login on SQL Server first.");
         Assert.Less(login, stop, "The login on SQL Server is given after the service is stopped for the move.");
         Assert.Less(login, register, "The service is registered as the virtual account before its login on SQL Server is given.");

         string grant = CodeBody(extension, "GrantServiceDatabaseLogin");
         StringAssert.Contains("DatabaseIsMssqlIntegrated()", grant, "The login step is not tied to SQL Server with Windows authentication.");
         StringAssert.Contains("RunDatabaseLoginVerb(", grant);
         StringAssert.Contains("g_bDatabaseLoginFailed := true", grant, "A login that could not be given does not fail the installation.");
         StringAssert.Contains("--grant-database-login", CodeBody(extension, "RunDatabaseLoginVerb"));

         string tasks = CodeBody(extension, "RunPostInstallTasks");
         Assert.IsTrue(Regex.IsMatch(tasks, @"bMoved\s*:=\s*MoveServiceToVirtualAccount\(\)"),
            "RunPostInstallTasks no longer knows whether it moved the service, so it cannot hold the moved service to opening its database.");
         int started = tasks.IndexOf("'START hMailServer'", StringComparison.Ordinal);
         int check = tasks.IndexOf("CheckServiceAfterMove()", StringComparison.Ordinal);
         Assert.Greater(check, started, "The moved service is not checked after its start, which is when it first opens the database as the virtual account.");

         string after = CodeBody(extension, "CheckServiceAfterMove");
         StringAssert.Contains("ReadDatabaseVersion(", after, "The moved service's start is taken for its opening the database.");
         StringAssert.Contains("RegisterHMailServerService('localsystem')", after, "A moved service that does not open its database is not moved back.");
         StringAssert.Contains("g_bDatabaseLoginFailed := true", after, "A moved service that does not open its database does not fail the installation.");
         Assert.IsFalse(Regex.IsMatch(after, @"(?<!Suppressible)MsgBox\("), "CheckServiceAfterMove uses a message box /SUPPRESSMSGBOXES does not suppress.");

         StringAssert.Contains("g_bDatabaseLoginFailed", CodeBody(extension, "CurStepChanged"),
            "A login that could not be given ends the installation with another step's message.");

         string server = ReadRepositoryFile(@"hmailserver\source\Server\hMailServer\hMailServer.cpp");
         StringAssert.Contains("_T(\"--grant-database-login\")", server, "hMailServer.exe does not know --grant-database-login, which the installer runs.");

         // The verb and the grant run elevated, and write nothing where the service
         // can (R3); the verb makes no user and changes no role in a database with DDL
         // triggers of its own, which would run with the administrator's rights.
         string account = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         foreach (string verb in new[] { "RunDatabaseLoginCommand", "RunGrantCommand" })
         {
            Match body = Regex.Match(account, @"ServiceAccount::" + verb + @"\([^)]*\)\s*\{(?:\s*//[^\n]*\n)*\s*Logger::ForbidWritesForThisProcess\(\);");
            Assert.IsTrue(body.Success, verb + " does not forbid the log writes of its elevated process first, so the database layer can write the error log and the Application log as an administrator.");
         }
         Match loginVerb = Regex.Match(account, @"ServiceAccount::RunDatabaseLoginCommand\(\)(.*?)\n   \}", RegexOptions.Singleline);
         Assert.IsTrue(loginVerb.Success);
         int triggers = loginVerb.Groups[1].Value.IndexOf("sys.triggers where parent_class = 0", StringComparison.Ordinal);
         int createUser = loginVerb.Groups[1].Value.IndexOf("create user {0} for login {0}", StringComparison.Ordinal);
         Assert.GreaterOrEqual(triggers, 0, "--grant-database-login does not look for the database's own DDL triggers before it changes a user or a role there.");
         Assert.Less(triggers, createUser, "--grant-database-login looks for the database's DDL triggers after it has made the user.");
         Assert.AreEqual(2, Regex.Matches(loginVerb.Groups[1].Value, @"else\s+if\s+\(!noChange\.IsEmpty\(\)\)").Count,
            "--grant-database-login makes the user or changes db_owner in a database with DDL triggers of its own.");
      }

      [Test]
      [Description("A new installation whose database step did not complete is finished as a new one: setup run again takes a LocalSystem service with no database, or the mark the installer leaves, for a new installation's, and DBSetup reads the same mark; and the last error after DBSetup's step names setup and DBSetup, never DBSetupQuick, which has nothing to retry there. Until 6.3.6 setup run again kept LocalSystem, and DBSetup set the database up and nothing else (the installer smoke VM, 2 October 2026).")]
      public void AnUnfinishedNewInstallationIsFinishedAsANewOne()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         string newService = CodeBody(extension, "IsNewService");
         StringAssert.Contains("PendingNewInstallation()", newService, "Setup run again does not take the mark for a new installation's service.");
         StringAssert.Contains("NoDatabaseConfiguredBefore()", newService, "Setup run again does not take a LocalSystem service with no database for a new installation's.");
         // The earlier installation's folder, read before {app} is known: a 64-bit
         // installation's uninstall entry is in the 64-bit view, and a look in the
         // 32-bit view alone found no settings file on any of them, so that every
         // LocalSystem service upgraded was taken for an unfinished new installation's
         // and moved to the virtual account (the smoke VM, 3 October 2026).
         string folder = CodeBody(extension, "PreviousInstallationFolder");
         StringAssert.Contains(@"'SOFTWARE\hMailServer', 'InstallLocation'", folder, "The earlier installation's folder is not read from the InstallLocation every installation writes.");
         StringAssert.Contains("HKLM64", folder, "The earlier installation's folder is not looked for in the 64-bit view, where a 64-bit installation's uninstall entry is.");
         string noDatabase = CodeBody(extension, "NoDatabaseConfiguredBefore");
         StringAssert.Contains("PreviousInstallationFolder()", noDatabase);
         Assert.IsTrue(Regex.IsMatch(noDatabase, @"Result\s*:=\s*\(szFile\s*<>\s*''\)\s*and"),
            "An earlier installation whose settings file is not found is taken for one with no database, which moves a LocalSystem service it cannot read to the virtual account.");
         StringAssert.Contains("PendingNewInstallation()", CodeBody(extension, "IsNewDatabase"),
            "A database DBSetup set up for an unfinished new installation is not taken for a new installation's.");

         string steps = CodeBody(extension, "CurStepChanged");
         Assert.IsTrue(Regex.IsMatch(steps, @"IsNewService\(\)"), "Whether the service is a new installation's is not read at ssInstall, before the database step names a database.");
         Assert.IsTrue(Regex.IsMatch(steps, @"RecordPendingNewInstallation\(\s*GetServiceAccountChoice\(\)\s*,\s*GetChosenProfile\(\)\s*\)"),
            "A new installation whose database step did not complete leaves no mark.");
         Match afterDBSetup = Regex.Match(steps, @"else\s+if\s+g_bDatabaseStepByDBSetup\s+then\s+RaiseException\('([^']*)'\)");
         Assert.IsTrue(afterDBSetup.Success, "The last error after DBSetup's database step is not one of its own.");
         StringAssert.DoesNotContain("DBSetupQuick", afterDBSetup.Groups[1].Value,
            "The last error after DBSetup's step sends people to DBSetupQuick, which has nothing to retry on an external database and does not start without SQL Server Compact.");
         Match newBuiltIn = Regex.Match(steps, @"else\s+if\s+IsNewDatabase\(\)\s+and\s+\(g_bNewInstallationFinished\s*=\s*false\)\s+then\s+RaiseException\('([^']*)'\)");
         Assert.IsTrue(newBuiltIn.Success, "The last error after a new installation's built-in database step has no words of its own.");
         StringAssert.DoesNotContain("DBSetupQuick", newBuiltIn.Groups[1].Value,
            "The last error after a new installation's built-in database step sends people to DBSetupQuick, which sets the database up and nothing after it.");
         string advice = CodeBody(extension, "QuickStepRetryAdvice");
         StringAssert.Contains("IsNewDatabase()", advice, "DBSetupQuick's retry advice is the same for a new installation as for an upgrade.");
         Assert.AreEqual(3, Regex.Matches(CodeBody(extension, "RunPostInstallTasks"), @"QuickStepRetryAdvice\(\)").Count,
            "A message after DBSetupQuick's step does not take its retry advice from QuickStepRetryAdvice.");

         string tasks = CodeBody(extension, "RunPostInstallTasks");
         int cleared = tasks.IndexOf("ClearPendingNewInstallation()", StringComparison.Ordinal);
         Assert.GreaterOrEqual(cleared, 0, "The mark is not cleared before the installation's own database step.");
         Assert.Less(cleared, tasks.IndexOf("DBSetupQuick.exe", StringComparison.Ordinal),
            "The mark is cleared after the database step, so the DBSetup the installer runs would finish the installation a second time.");
         StringAssert.Contains("ClearPendingNewInstallation()", CodeBody(extension, "CurUninstallStepChanged"), "The uninstaller leaves the mark.");

         string names = CodeBody(extension, "PendingNewInstallation");
         string dbsetup = ReadRepositoryFile(@"hmailserver\source\Tools\DBSetup\NewInstallation.cs");
         foreach (string value in new[] { "NewInstallationPending", "NewInstallationProfile" })
         {
            StringAssert.Contains("'" + value + "'", extension, "The installer does not write " + value + ".");
            StringAssert.Contains("\"" + value + "\"", dbsetup, "DBSetup does not read " + value + ", which the installer writes.");
         }
         StringAssert.Contains("HKLM32", names, "The installer's mark is not in the 32-bit view, which DBSetup reads.");
         StringAssert.Contains("RegistryView.Registry32", dbsetup, "DBSetup does not read the mark in the 32-bit view the installer writes.");
      }

      [Test]
      [Description("The mark of an unfinished new installation is not taken by an update hMailServer.Updater runs, which upgrades a server in service as it is; and a database the settings file names before the step is held to its version as an upgrade's is, whatever the mark says, so that a step which changed it ends in 11 and is not put back. Found by the refuter of the 6.3.6 installer fixes (3 October 2026): with the mark, an unattended update would have moved the service by default and ended in 10 after changing the database. Setup run again takes the account and the profile the mark holds as its defaults, as DBSetup does.")]
      public void TheUpdaterLeavesAnUnfinishedInstallationsMarkAndItsDatabaseStepIsAnUpgrades()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         string pending = CodeBody(extension, "PendingNewInstallation");
         Assert.IsTrue(Regex.IsMatch(pending, @"if\s+g_bPendingBefore\s+and\s+\(g_szUpgradeToken\s*<>\s*''\)\s+then\s+begin\s+g_bPendingBefore\s*:=\s*false"),
            "An update hMailServer.Updater runs takes the mark of an unfinished new installation, and with it the account move, the new installation's defaults and a database step that is not held to its version.");
         Assert.IsTrue(Regex.IsMatch(pending, @"g_szPendingAccount\s*:="), "The mark's account is not read.");
         Assert.IsTrue(Regex.IsMatch(pending, @"PENDING_PROFILE_VALUE"), "The mark's profile is not read.");

         string tasks = CodeBody(extension, "RunPostInstallTasks");
         Assert.IsTrue(Regex.IsMatch(tasks, @"bDatabaseStepOnExisting\s*:=\s*\(GetCurrentDatabaseType\(\)\s*<>\s*''\)"),
            "Whether the database step meets an existing database is not what the settings file names: a database set up for an unfinished new installation is not held to its version, and a step that changed it ends in 10.");
         StringAssert.DoesNotContain("bDatabaseStepOnExisting := (IsNewDatabase() = false)", tasks);
         Assert.IsTrue(Regex.IsMatch(tasks, @"if\s+bDatabaseStepOnExisting\s+and\s+\(bServiceRegistered\s*=\s*true\)\s+then\s+StartServiceForDatabaseUpgrade\(\)"),
            "An existing database is not met by the start for a database upgrade, so an older one is reported as HM5011.");

         string profile = CodeBody(extension, "GetChosenProfile");
         Assert.IsTrue(Regex.IsMatch(profile, @"if\s+\(szProfile\s*=\s*''\)\s+and\s+PendingNewInstallation\(\)\s+then\s+szProfile\s*:=\s*g_szPendingProfile"),
            "A silent setup run again does not take the profile the unfinished installation chose, which DBSetup, finishing it instead, applies.");

         string pages = CodeBody(extension, "CreateWizardPages");
         Assert.IsTrue(Regex.IsMatch(pages, @"if\s+IsNewService\(\)\s+and\s+\(PendingAccountDefault\(\)\s*=\s*'localsystem'\)\s+then\s+g_pageServiceAccount\.SelectedValueIndex\s*:=\s*1"),
            "The page does not offer again the LocalSystem an unfinished installation chose.");
         string choice = CodeBody(extension, "GetServiceAccountChoice");
         Assert.IsTrue(Regex.IsMatch(choice, @"if\s+\(g_pageServiceAccount\.SelectedValueIndex\s*=\s*1\)\s+then\s+Result\s*:=\s*'localsystem'\s+else\s+Result\s*:=\s*'virtual'"),
            "A new installation's page answer is not the choice whatever it preselected.");
      }

      [Test]
      [Description("The service stop the installer performs before replacing files is bounded, and a failure to stop blocks the install.")]
      public void TestServiceStopIsBoundedAndBlocking()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         // The original was: while (IsServiceStopped('hMailServer') = false) do
         // Sleep(250); - no limit, no check of StopService's result, and no effect on
         // whether the install proceeded. A service that will not stop then means Inno
         // cannot replace hMailServer.exe; if the user answers Ignore, the old binary
         // stays while DBUpdater takes the schema forward, and the server will not run
         // against either version afterwards.
         Assert.IsFalse(Regex.IsMatch(extension,
               @"while\s*\(\s*IsServiceStopped\s*\(\s*'hMailServer'\s*\)\s*=\s*false\s*\)\s*do\s*begin\s*Sleep\s*\(\s*\d+\s*\)\s*;\s*end",
               RegexOptions.IgnoreCase | RegexOptions.Singleline),
            "The installer waits for the hMailServer service to stop in a loop with no upper bound. A service " +
            "stuck in STOP_PENDING freezes the wizard with no message and no way to cancel.");

         Assert.IsTrue(Regex.IsMatch(extension, @"function\s+StopHMailServerService\s*\(\s*\)\s*:\s*Boolean",
                                     RegexOptions.IgnoreCase),
            "StopHMailServerService is gone. Stopping the service has to be a function that can fail, because " +
            "NextButtonClick must be able to refuse to continue when the service is still running.");

         Assert.IsTrue(Regex.IsMatch(extension, @"StopHMailServerService\s*\(\s*\)\s*=\s*false\s*\)\s*then\s*Result\s*:=\s*false",
                                     RegexOptions.IgnoreCase | RegexOptions.Singleline),
            "NextButtonClick does not refuse to advance when the service could not be stopped. Continuing " +
            "leads to Inno's Retry/Ignore prompt on a locked hMailServer.exe, and Ignore leaves the old " +
            "server binary installed against an upgraded database schema.");
      }

      [Test]
      [Description("Guards a currently-correct invariant: the version stamped into the installer matches the server being installed.")]
      public void TestInstallerVersionMatchesTheServerVersion()
      {
         Match version = Regex.Match(ReadRepositoryFile(@"hmailserver\source\Server\Common\Application\Version.h"),
                                     @"#define\s+HMAILSERVER_VERSION\s+""([0-9.]+)""");
         Assert.IsTrue(version.Success, "Could not read HMAILSERVER_VERSION from Version.h.");
         string serverVersion = version.Groups[1].Value;

         string setup = ReadInstallerScript("section_setup_64.iss");

         // RELEASE.md step 6 stamps Version.h and section_setup_64.iss by hand. When
         // they drift, the installed file's version and the product's version disagree
         // and no support conversation about "which build is this" can be trusted.
         foreach (string key in new[] { "AppVersion", "VersionInfoVersion", "AppVerName", "OutputBaseFilename" })
         {
            Match line = Regex.Match(setup, @"^\s*" + key + @"\s*=\s*(.+?)\s*$", RegexOptions.Multiline);

            Assert.IsTrue(line.Success, key + " is missing from section_setup_64.iss.");
            Assert.IsTrue(line.Groups[1].Value.Contains(serverVersion),
               "section_setup_64.iss " + key + " is \"" + line.Groups[1].Value + "\" but Version.h says " +
               serverVersion + ".");
         }
      }

      [Test]
      [Description("The seven shipped .csproj <Version> values track Version.h. A prerelease label is allowed on them and not on Version.h, so this asserts starts-with rather than equality.")]
      public void TestToolVersionsTrackTheServerVersion()
      {
         Match version = Regex.Match(ReadRepositoryFile(@"hmailserver\source\Server\Common\Application\Version.h"),
                                     @"#define\s+HMAILSERVER_VERSION\s+""([0-9.]+)""");
         Assert.IsTrue(version.Success, "Could not read HMAILSERVER_VERSION from Version.h.");
         string serverVersion = version.Groups[1].Value;

         // Every shipped tool, and only the shipped tools: ControlPanel.Core and
         // ControlPanel.Tests carry no <Version> because neither is installed.
         string[] projects =
         {
            @"ControlPanel\ControlPanel.csproj",
            @"DBSetup\DBSetup.csproj",
            @"DBSetupQuick\DBSetupQuick.csproj",
            @"DBUpdater\DBUpdater.csproj",
            @"DataDirectorySynchronizer\DataDirectorySynchronizer.csproj",
            @"ImportTool\ImportTool.csproj",
            @"Shared\Shared.csproj",
         };

         foreach (string project in projects)
         {
            string text = File.ReadAllText(Paths.Combine(SourceRoot, @"Tools", project));
            Match element = Regex.Match(text, @"<Version>(.+?)</Version>");

            Assert.IsTrue(element.Success, project + " has no <Version> element.");

            // Starts-with, not equals. The label lives on the tools (where the .NET
            // SDK strips it back off for AssemblyVersion/FileVersion and keeps it in
            // InformationalVersion) and must NOT live in Version.h, where the regex
            // above - a digits-and-dots class - would stop matching and take this
            // whole fixture down with it.
            StringAssert.StartsWith(serverVersion, element.Groups[1].Value,
               project + " is stamped " + element.Groups[1].Value + " but Version.h says " + serverVersion + ".");
         }
      }

      [Test]
      [Description("No MSVC #import output is tracked. The generated .tlh includes its .tli by ABSOLUTE path, so committing one ships a developer machine layout and breaks the build everywhere else.")]
      public void TestNoImportGeneratedHeadersAreTracked()
      {
         // Walk the source tree rather than asking git: this fixture has no git
         // dependency anywhere else, and an untracked-but-present file is exactly
         // the normal state here - these are build output, produced on every build.
         // What matters is that none of them is COMMITTED, and the ignore rule is
         // what guarantees that, so the ignore rule is what this asserts.
         string ignore = ReadRepositoryFile(".gitignore");

         Assert.IsTrue(Regex.IsMatch(ignore, @"^\s*\*\.tlh\s*$", RegexOptions.Multiline),
            ".gitignore does not ignore *.tlh. MSVC #import writes these on every build and the " +
            "generated text embeds the absolute path of the machine that produced it.");

         Assert.IsTrue(Regex.IsMatch(ignore, @"^\s*\*\.tli\s*$", RegexOptions.Multiline),
            ".gitignore does not ignore *.tli.");

         // No content sweep over the files themselves, deliberately. Untracked .tlh
         // files are the NORMAL state of a built tree, and every one of them contains
         // an absolute path, because that is simply what MSVC generates - so a sweep
         // would fail on any machine that has ever built the server. The defect was
         // never that the files exist; it was that three of them were committed, and
         // the ignore rules above are the mechanism that prevents that recurring.
      }

      [Test]
      [Description("An unattended upgrade must never reach a modal password dialog: DBSetupQuick has to forward the password to DBUpdater, and the shared Authenticator has to fail rather than prompt under /silent.")]
      public void TestSilentUpgradeCannotReachAPasswordDialog()
      {
         // The two halves are separate files and either one alone leaves the hang in
         // place, so both are asserted here rather than in two fixtures that could
         // pass independently while the product still waits forever.
         // UpgradeDatabase takes the report file's path since 6.3.6 (report:PATH), so
         // its parameters are matched as whatever they are.
         string quick = File.ReadAllText(Paths.Combine(SourceRoot, @"Tools\DBSetupQuick\Program.cs"));
         Match upgrade = Regex.Match(quick, @"private static int UpgradeDatabase\([^)]*\)(.*?)\n      \}",
                                     RegexOptions.Singleline);

         Assert.IsTrue(upgrade.Success, "Could not find UpgradeDatabase in DBSetupQuick.");
         StringAssert.Contains("password:", upgrade.Groups[1].Value,
            "DBSetupQuick.UpgradeDatabase does not forward the administrator password to DBUpdater. " +
            "DBUpdater authenticates before it can move the schema, so without it an unattended " +
            "upgrade falls through to a modal dialog nobody is there to answer.");

         string authenticator = File.ReadAllText(Paths.Combine(SourceRoot, @"Tools\Shared\Miscellaneous\Authenticator.cs"));
         int silent = authenticator.IndexOf("IsSilent()", StringComparison.Ordinal);
         int dialog = authenticator.IndexOf("formEnterPassword", StringComparison.Ordinal);

         Assert.Greater(silent, -1, "Authenticator does not consult CommandLineParser.IsSilent().");
         Assert.Greater(dialog, -1, "Authenticator no longer references formEnterPassword - update this test.");
         Assert.Less(silent, dialog,
            "Authenticator reaches formEnterPassword before it checks IsSilent(). Under /silent that " +
            "dialog is not a prompt, it is a hang - and it hangs the installer waiting on the process.");
      }   }
}
