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
         return ScriptText(ReadRepositoryFile(Paths.Combine(@"hmailserver\installation", fileName)));
      }

      /// <summary>
      ///    An installer script's text as ISCC and build/check-installer-scripts.py read it:
      ///    a carriage return, alone or before a line feed, ends the line (a UTF-8 BOM is
      ///    dropped by File.ReadAllText). Read with the line feed alone, what followed a lone
      ///    carriage return was part of a // comment or of the line before to these tests,
      ///    where ISCC compiles it as code, a preprocessor line or a section's entry (check 7
      ///    of R1 to R4, 28 October 2026: three one-line changes that switched a rule off
      ///    passed these tests that way).
      /// </summary>
      private static string ScriptText(string text)
      {
         return text.Replace("\r\n", "\n").Replace('\r', '\n');
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
            string text = ScriptText(File.ReadAllText(script.FullName));

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
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "PrepareToInstallRefusal"), @"Result\s*:=\s*PrepareProgramFolders\(PreviousInstallationFolder\(\)\);\s*end;\s*$"),
            "PrepareToInstall does not make or harden the program folder, Bin and ControlPanel as its last step, before any file is copied.");

         // The chosen folder is checked and said - in a box that /SUPPRESSMSGBOXES
         // suppresses, and in the setup log - and never refused for its permissions.
         StringAssert.Contains("CheckProgramFolderPermissions(WizardDirValue(), true)", CodeBody(extension, "NextButtonClick"),
            "The folder page does not say when the chosen folder lets every user change what is in it.");
         StringAssert.Contains("CheckProgramFolderPermissions(szApp, false)", CodeBody(extension, "CheckProgramFolders"),
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
         StringAssert.Contains("Result := IsTrustedSid(pOwner);", CodeBody(extension, "HeldOwnerIsTrusted"),
            "HeldOwnerIsTrusted does not take SYSTEM, the Administrators and TrustedInstaller for a trusted owner.");
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
      [Description("Work item 78, R1: setup makes a new program folder itself in PrepareToInstall, before Inno Setup makes or copies anything, with permissions of its own from the moment it is made - NtCreateFile with a security descriptor that gives SYSTEM and the Administrators full control, Users Read & execute where every user could read a folder made there, inherits nothing and makes the Administrators the owner - and the folders above it that were not there with it, exactly the names below the deepest folder its walk found and held, each by its one name relative to the handle of the folder above it, so that no link put on the way since the walk is followed (check 5 of R1 to R4, 27 October 2026: CreateDirectoryW by the path made the folder where such a link led), and a name somebody else took since refused (STATUS_OBJECT_NAME_COLLISION); and makes Bin and ControlPanel the same way when the program folder is there and they are not. The uninstaller is given each to remove when empty (section_dirs.iss), and those still empty are removed when the installation does not reach its files. In 6.3.7 and earlier Inno Setup made the folder with what the folder above handed down, and icacls replaced that about 45 ms later (the installer smoke VM, 6 October 2026); a Bin or ControlPanel Inno Setup made in a program folder that was there took what that folder handed down (25 October 2026).")]
      public void ANewProgramFolderIsMadeWithPermissionsOfItsOwnFromTheStart()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         // NtCreateFile with an OBJECT_ATTRIBUTES - each folder by its one name relative to the
         // handle of the folder above it, with its descriptor - and the descriptor by SID; no
         // folder made by its path, which a link put on the way since the walk would lead
         // elsewhere (check 5 of R1 to R4, 27 October 2026).
         Assert.IsTrue(Regex.IsMatch(extension, @"function\s+MakeFileRelative\(var\s+FileHandle:\s*Integer;\s*DesiredAccess:\s*cardinal;\s*var\s+ObjectAttributes:\s*HM_OBJECT_ATTRIBUTES;[^;]*;[^)]*\):\s*cardinal;\s*external\s+'NtCreateFile@ntdll\.dll stdcall';"),
            "The installer does not declare NtCreateFile with an OBJECT_ATTRIBUTES, the way a folder is made with permissions of its own from its first moment, relative to the folder above it.");
         Assert.IsFalse(Regex.IsMatch(extension, @"external\s+'CreateDirectory\w*@"), "A folder of the program is made by its path again (CreateDirectoryW).");
         string makeIn = CodeBody(extension, "MakeFolderIn");
         foreach (string word in new[] { "attributes.RootDirectory := Parent;", "attributes.SecurityDescriptor := pDescriptor;", "HM_SHARE_ALL_BUT_DELETE, HM_FILE_CREATE, HM_MAKE_FOLDER_OPTIONS", "g_aHeldFolders" })
            StringAssert.Contains(word, makeIn, "MakeFolderIn does not use " + word + ".");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_MAKE_FOLDER_OPTIONS\s*=\s*\$00200021;") && Regex.IsMatch(extension, @"HM_FILE_CREATE\s*=\s*2;"),
            "A folder is not made as a new folder (FILE_CREATE, FILE_DIRECTORY_FILE), or is made through a link (FILE_OPEN_REPARSE_POINT).");
         string make = CodeBody(extension, "MakeFolders");
         foreach (string word in new[] { "'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)'", "'(A;OICI;0x1200a9;;;BU)'", "EveryUserMayReadMadeIn(HeldHandle)", "SecurityDescriptorOfText(szDescriptor, 1, pDescriptor, 0)",
                                         "g_aFoldersMade", "g_szBinMade := missing[index]", "g_szControlPanelMade := missing[index]",
                                         "MakeFolderIn(parent, ExtractFileName(missing[index]), pDescriptor, status)", "g_bProgramFolderNew := Result;", "HeldIsLink(HeldHandle, bKnown)",
                                         "MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere)" })
            StringAssert.Contains(word, make, "MakeFolders does not use " + word + ".");
         Assert.IsFalse(make.Contains("DirExists("), "MakeFolders decides what to make by looking at the path again, where somebody else may have made a folder or a link since the walk.");
         // The folder held above what is made read again, and each folder made read where
         // it is, through the handles (the review of 26 October 2026: an empty folder held
         // above a new program folder made a junction through a second open while held).
         Assert.IsTrue(Regex.IsMatch(extension, @"function\s+ReadHeldInformation\(hFile:\s*Integer;\s*var\s+lpFileInformation:\s*HM_FILE_INFORMATION\):\s*Boolean;\s*external\s+'GetFileInformationByHandle@kernel32\.dll stdcall';") &&
                       Regex.IsMatch(extension, @"function\s+FinalPathOfHandle\(hFile:\s*Integer;\s*lpszFilePath:\s*String;\s*cchFilePath:\s*cardinal;\s*dwFlags:\s*cardinal\):\s*cardinal;\s*external\s+'GetFinalPathNameByHandleW@kernel32\.dll stdcall';") &&
                       Regex.IsMatch(extension, @"HM_VOLUME_NAME_GUID\s*=\s*1;"),
            "What a held folder is now, and where, is not read through its handle (GetFileInformationByHandle, GetFinalPathNameByHandleW by the volume's name).");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_STATUS_NAME_COLLISION\s*=\s*\$C0000035;"), "STATUS_OBJECT_NAME_COLLISION is not $C0000035: a folder somebody else made since setup looked would be taken.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "EveryUserMayReadMadeIn"), @"HM_CONTAINER_INHERIT\)\s*<>\s*0") &&
                       CodeBody(extension, "EveryUserMayReadMadeIn").Contains("ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION"),
            "Whether every user could read a folder made there is not read, through the handle that holds it, from what it hands down to folders.");
         Assert.IsFalse(Regex.IsMatch(extension, @"^(procedure|function)\s+(HardenNewProgramFolder|MakeProgramFolder)\s*\(", RegexOptions.Multiline),
            "HardenNewProgramFolder or MakeProgramFolder is back.");
         Assert.IsFalse(Regex.IsMatch(CodeBody(extension, "CurStepChanged"), @"g_bProgramFolderNew\s*:="),
            "Whether this installation made the program folder is decided at ssInstall again, after PrepareToInstall made it.");

         // [Dirs]: the folders setup made, for the uninstaller - the eight above the
         // program folder, shallowest first, then the program folder, Bin and
         // ControlPanel - each recorded although it was there when [Dirs] ran, and only
         // when this setup made it.
         string dirs = ReadInstallerScript("section_dirs.iss");
         List<Match> entries = Regex.Matches(dirs, @"^Name:\s*""([^""]+)""([^\r\n]*)", RegexOptions.Multiline).Cast<Match>().ToList();
         Assert.GreaterOrEqual(entries.Count, 11, "section_dirs.iss has too few entries.");
         for (int index = 0; index < 8; index++)
         {
            Assert.AreEqual("{code:FolderMadeBySetup|" + index + "}", entries[index].Groups[1].Value, "Entry " + index + " of [Dirs] is not the folder above the program folder setup made, " + index + " from the top.");
            StringAssert.Contains("Flags: uninsalwaysuninstall", entries[index].Groups[2].Value, "A folder setup made above the program folder is not recorded for the uninstaller.");
            StringAssert.Contains("Check: SetupMadeFolder(" + index + ")", entries[index].Groups[2].Value, "A folder above the program folder that setup did not make would be removed by the uninstaller.");
         }
         string[] names = { "{app}", "{app}\\Bin", "{app}\\ControlPanel" };
         string[] checks = { "SetupMadeProgramFolder", "SetupMadeBin", "SetupMadeControlPanel" };
         for (int index = 0; index < 3; index++)
         {
            Assert.AreEqual(names[index], entries[8 + index].Groups[1].Value, "[Dirs] does not give the uninstaller " + names[index] + " after the folders above it.");
            StringAssert.Contains("Flags: uninsalwaysuninstall", entries[8 + index].Groups[2].Value, names[index] + ", made by setup, is not recorded for the uninstaller.");
            StringAssert.Contains("Check: " + checks[index], entries[8 + index].Groups[2].Value, names[index] + " would be recorded for the uninstaller although setup did not make it.");
         }
         Assert.IsFalse(dirs.Contains("AfterInstall:"), "[Dirs] gives a folder its permissions after Inno Setup has made it.");
         StringAssert.Contains("Result := ProgramFolderMadeAsNamed();", CodeBody(extension, "SetupMadeProgramFolder"));
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "SetupMadeBin"), @"Result\s*:=\s*g_szBinMade\s*<>\s*'';") &&
                       Regex.IsMatch(CodeBody(extension, "SetupMadeControlPanel"), @"Result\s*:=\s*g_szControlPanelMade\s*<>\s*'';"),
            "Bin or ControlPanel is given to the uninstaller whatever made it.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "ProgramFolderMadeAsNamed"), @"CompareText\(g_szProgramFolderReal,\s*RemoveBackslashUnlessRoot\(ExpandConstant\('\{app\}'\)\)\)\s*=\s*0"),
            "A program folder made where a link leads is given to the uninstaller as {app}.");

         // Not reaching the files - a cancel, a failure, a stop before them - removes
         // what setup made that is still empty, deepest first; and going back to the
         // folder page and on again removes what the earlier pass made.
         string deinitialize = CodeBody(extension, "DeinitializeSetup");
         Assert.IsTrue(Regex.IsMatch(deinitialize, @"if\s+not\s+g_bFilesInstalled\s+then\s+RemoveFoldersMadeHere\(\);\s*ReleaseHeldFolders\(\);"),
            "DeinitializeSetup does not remove the empty folders setup made when the files were not installed, while the folders above them are still held, and then let the held folders go.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "CurStepChanged"), @"CurStep\s*=\s*ssPostInstall\s+then\s+begin\s+(//[^\n]*\n\s*)*g_bFilesInstalled\s*:=\s*true;"),
            "The installation does not mark its files installed at ssPostInstall, so DeinitializeSetup would remove a folder it made, or leave one empty.");
         string remove = CodeBody(extension, "RemoveFoldersMadeHere");
         string removeOne = CodeBody(extension, "RemoveMadeFolder");
         Assert.IsTrue(Regex.IsMatch(remove, @"for\s+index\s*:=\s*GetArrayLength\(g_aFoldersMade\)\s*-\s*1\s+downto\s+0\s+do\s+RemoveMadeFolder\(") && removeOne.Contains("RemoveDir(Folder)") &&
                       !remove.Contains("DelTree(") && !removeOne.Contains("DelTree("),
            "RemoveFoldersMadeHere does not remove the folders setup made deepest first, and only when empty (RemoveDir).");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "CheckProgramFolders"), @"^\s*RemoveFoldersMadeHere\(\);\s*ReleaseHeldFolders\(\);", RegexOptions.Multiline),
            "A second pass, after the folder page was gone back to, keeps what the first held and made, or removes it by a path nothing holds any more.");

         // A folder made is recorded for its removal only once it is found where it was
         // named, with where that is and the handle that holds it, and removed only while
         // it is still that folder, read through a handle opened on its path as itself
         // (check 3 of R1 to R4, 27 October 2026: one made through a link put on the way
         // was recorded first, and a cancel removed whatever its path led to by then).
         int named = make.IndexOf("MadeWhereNamed(", StringComparison.Ordinal);
         foreach (string word in new[] { "g_aFoldersMade[", "g_szBinMade :=", "g_szControlPanelMade :=", "g_aFoldersMadeAt[", "g_szBinMadeAt :=", "g_szControlPanelMadeAt :=" })
            Assert.IsTrue(named >= 0 && make.IndexOf(word, StringComparison.Ordinal) > named, "MakeFolders records a folder it made (" + word + ") before it has found it where it was named.");
         foreach (string word in new[] { "LetGoOfHeld(Handle);", "HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0)",
                                         "(not HeldIsLink(check, bKnown))", "(CompareText(FinalPathOfHeld(check), At) = 0)" })
            StringAssert.Contains(word, removeOne, "RemoveMadeFolder does not use " + word + ".");

         AssertNoPlainMessageBox(extension, "MakeFolders", "RemoveFoldersMadeHere", "RemoveMadeFolder", "DeinitializeSetup");
      }

      [Test]
      [Description("Work item 78, R2: a program folder, Bin and ControlPanel that are there before the installation - a server installed into a folder made beforehand, an upgrade, a repair, the Control Panel alone - are hardened in PrepareToInstall, before any file is copied and before setup runs any program of the installation, and setup does not install into one it could not harden: the program folder whatever is in it, but for the earlier installation's whose Data, Logs, Temp, Database or Events inherit from it; an entry of its own that lets a group of every user write taken away; one another account owns taken only when it is the earlier installation's - given to the Administrators with takeown.exe /A, the entries of its own that give no right to write kept, the service's Read & execute on Bin among them, as ServiceAccount.cpp's RebuiltAcl_ keeps them - and refused otherwise; each read through the handle that holds it and read again once hardened. In 6.3.7 and earlier an upgrade copied Bin's files and ran hMailServer.exe /Register from it before /ProtectFolders hardened it (the installer smoke VM, 6 October 2026), and the first R2 of 6.3.8 left a folder that held a Data, and one it could not harden, as they were (the review of 26 October 2026).")]
      public void FoldersThatAreThereAreHardenedBeforeTheFiles()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string prepare = CodeBody(extension, "PrepareProgramFolders");

         // Every installation: no condition on the components, the server's included.
         Assert.IsFalse(prepare.Contains("IsComponentSelected(") || CodeBody(extension, "CheckProgramFolders").Contains("IsComponentSelected("),
            "The program folders are prepared for some components only; a server installation into a folder that is there would not be hardened before its files.");

         // The order: the program folder, then Bin and ControlPanel - hardened or made, each
         // refused when it is a link now - then the three read again for a link, then what
         // is in them.
         int[] order = new[] { "HardenHeldFolder(g_szProgramFolderReal", "HeldIsLink(g_hBin,", "HardenHeldFolder(g_szBinReal", "MakeFolders(g_szBinHeld", "HeldIsLink(g_hControlPanel,",
                               "HardenHeldFolder(g_szControlPanelReal", "MakeFolders(g_szControlPanelHeld", "HeldIsLink(g_hProgramFolderPrepared,", "HeldIsLink(g_hBinPrepared,",
                               "HeldIsLink(g_hControlPanelPrepared,", "CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, true" }.Select(word => prepare.IndexOf(word, StringComparison.Ordinal)).ToArray();
         Assert.IsTrue(order.All(position => position >= 0) && order.SequenceEqual(order.OrderBy(position => position)),
            "PrepareProgramFolders does not harden the program folder, then make or harden Bin and ControlPanel - each refused when it is a link now - then read the three again for a link, then look at what is in them.");

         // Whether an empty Bin or ControlPanel is made again is decided by CheckProgramFolders,
         // before anything changes, for every one that held nothing (check 3 of R1 to R4,
         // 27 October 2026: asked after the program folder's hardening had handed its
         // permissions down, who could write it read as nobody, and none was made again).
         string check = CodeBody(extension, "CheckProgramFolders");
         Assert.AreEqual(Regex.Matches(extension, @"g_bRemake(Bin|ControlPanel)\s*:=").Count, Regex.Matches(check, @"g_bRemake(Bin|ControlPanel)\s*:=").Count,
            "Whether Bin or ControlPanel is made again is decided outside CheckProgramFolders, after something may have changed.");

         // The last page runs nothing of an installation whose folders were not as setup
         // left them: its Control Panel entry is held to FoldersWereAsPrepared.
         string run = ReadInstallerScript("section_run.iss");
         List<string> lastPage = Regex.Matches(run, @"^Filename:[^\r\n]*\bpostinstall\b[^\r\n]*", RegexOptions.Multiline).Cast<Match>().Select(match => match.Value).ToList();
         Assert.IsTrue(lastPage.Count > 0 && lastPage.All(line => line.Contains("Check: FoldersWereAsPrepared")),
            "An entry of the last page runs whether or not the folders setup prepared were as it left them.");
         CollectionAssert.AreEqual(new[] { "true" }, Regex.Matches(extension, @"g_bFoldersNotAsPrepared\s*:=\s*(\w+)").Cast<Match>().Select(match => match.Groups[1].Value).ToArray(),
            "Whether the folders were found not as setup left them is set other than once, to true.");

         // The program folder of the earlier installation alone is left for
         // /ProtectFolders when its data folders inherit from it.
         StringAssert.Contains("g_bEarlierInstallation := CompareText(RemoveBackslashUnlessRoot(Trim(Earlier)), szApp) = 0;", CodeBody(extension, "CheckProgramFolders"),
            "The earlier installation's folder is not the one the registry records.");
         string inherits = CodeBody(extension, "DataFolderThatInherits");
         foreach (string folder in new[] { "'Data'", "'Logs'", "'Temp'", "'Database'", "'Events'" })
            StringAssert.Contains(folder, inherits, "DataFolderThatInherits does not look at " + folder + ".");
         StringAssert.Contains("ControlOfDescriptor(pDescriptor, control, revision)", CodeBody(extension, "InheritsNothing"));
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_DACL_PROTECTED\s*=\s*\$1000;"), "SE_DACL_PROTECTED is not $1000.");

         // In PrepareToInstall, before any file: nothing of it is left at ssInstall.
         Assert.IsFalse(Regex.IsMatch(CodeBody(extension, "CurStepChanged"), @"Harden\w*\("), "A program folder that is there is hardened at ssInstall again rather than in PrepareToInstall.");

         // After the files, before setup runs anything of the installation: the folders
         // it prepared read again through the handles that hold them, and what is in
         // them looked at for a link or an empty folder setup did not leave (the review
         // of 26 October 2026: a handle opened before the hardening keeps its rights).
         string after = CodeBody(extension, "CurStepChanged");
         int looked = after.IndexOf("FoldersStillAsPrepared(", StringComparison.Ordinal);
         Assert.IsTrue(looked >= 0 && new[] { "RunPostInstallTasks()", "RegisterTypeLib()", "DeleteOldFiles()" }.All(word => after.IndexOf(word, StringComparison.Ordinal) < 0 || after.IndexOf(word, StringComparison.Ordinal) > looked),
            "The folders setup prepared are not read again before setup runs a program of the installation.");
         StringAssert.Contains("'config hMailServer start= disabled'", CodeBody(extension, "KeepServiceFromStarting"), "The service is not kept from starting when the folders are not as setup left them.");
         StringAssert.Contains("restarts Windows", CodeBody(extension, "LinkRemedy"), "The remedy of a link does not say to restart Windows, which takes away what a program opened it for.");
         foreach (string word in new[] { "RemakeEmptyFolder(g_szBinReal, g_hBin, 1, hMade, szWhy)", "RemakeEmptyFolder(g_szControlPanelReal, g_hControlPanel, 2, hMade, szWhy)" })
            StringAssert.Contains(word, prepare, "PrepareProgramFolders does not use " + word + ".");

         // Hardened through the handle that holds it, every failure a refusal, and read
         // again after.
         string held = CodeBody(extension, "HardenHeldFolder");
         Assert.IsFalse(Regex.IsMatch(held, @"ReadFolder(Permissions|Owner)\("), "HardenHeldFolder reads the folder's owner or permissions by its path rather than through the handle that holds it.");
         Assert.IsFalse(held.Contains("O:BAD:PAI"), "HardenHeldFolder gives a folder another account owns a list of SYSTEM, the Administrators and Users alone, dropping the service's entries.");
         Assert.Less(held.IndexOf("/remove:g", StringComparison.Ordinal), held.IndexOf("/inheritance:r", StringComparison.Ordinal),
            "The entries of the folder's own that let every user write are not taken away before it stops inheriting.");
         foreach (string word in new[] { "/inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F", "*S-1-5-32-545:(OI)(CI)RX", "EveryUserMayRead(pDacl)" })
            StringAssert.Contains(word, held, "HardenHeldFolder does not use " + word + ".");
         string rebuild = CodeBody(extension, "RebuildFolderOfUntrustedOwner");
         foreach (string word in new[] { "HM_FOLDER_WRITE_RIGHTS", "HM_PROTECTED_DACL_INFORMATION", "HM_RIGHTS_READ_EXECUTE", "HM_FILE_ALL_ACCESS", "takeown.exe" })
            StringAssert.Contains(word, rebuild, "RebuildFolderOfUntrustedOwner does not use " + word + ".");
         Assert.IsFalse(Regex.IsMatch(rebuild, @"ReadFolder(Permissions|Owner)\("), "RebuildFolderOfUntrustedOwner reads the folder's permissions by its path.");
         StringAssert.Contains("HM_INHERITED_FLAG_BIT", CodeBody(extension, "AddAsOwnEntry"), "An entry kept is not made the folder's own.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_INHERITED_FLAG_BIT\s*=\s*\$1000;") && Regex.IsMatch(extension, @"HM_ACCESS_DENIED\s*=\s*1;") && Regex.IsMatch(extension, @"HM_FILE_ALL_ACCESS\s*=\s*\$1F01FF;") &&
                       Regex.IsMatch(extension, @"HM_RIGHTS_READ_EXECUTE\s*=\s*\$1200A9;") && Regex.IsMatch(extension, @"HM_PROTECTED_DACL_INFORMATION\s*=\s*\$80000004;"),
            "A constant of the rebuilt list is not the Windows value.");

         // The service's SID is the one the server computes for NT SERVICE\hMailServer.
         string account = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         Match serviceSid = Regex.Match(account, @"ServiceAccount::ServiceSid\(_T\(""hMailServer""\)\)\s*!=\s*_T\(""(S-1-5-80-[0-9-]+)""\)");
         Assert.IsTrue(serviceSid.Success, "ServiceAccountTester no longer holds NT SERVICE\\hMailServer's SID to a value.");
         Assert.IsTrue(extension.Contains("HM_SERVICE_SID              = '" + serviceSid.Groups[1].Value + "';"),
            "HM_SERVICE_SID is not NT SERVICE\\hMailServer's SID, " + serviceSid.Groups[1].Value + ".");

         // The root of a volume: C:\ and \\?\Volume{...}\ - where a mount point, or a link to
         // a volume's root, leads - with its backslash, which names the root folder and not
         // the volume's device (the review of 26 October 2026: RemoveBackslashUnlessRoot took
         // it off, and icacls was then refused the device).
         string root = CodeBody(extension, "IsVolumeRoot");
         Assert.IsTrue(root.Contains("Result := Length(Path) <= 3;") && root.Contains("CompareText(Copy(Path, 1, 11), '\\\\?\\Volume{') = 0") && root.Contains("Copy(Path, Length(Path) - 1, 2) = '}\\'"),
            "IsVolumeRoot does not take C:\\ and \\\\?\\Volume{...}\\ alone for the root of a volume.");
         Assert.IsFalse(Regex.IsMatch(prepare + CodeBody(extension, "CheckProgramFolders"), @"Length\(g_szProgramFolderReal\)\s*(<=|>)\s*3"),
            "The root of a volume is told by the length of its path, which a volume reached by its name is longer than.");
         Assert.AreEqual(2, Regex.Matches(CodeBody(extension, "HoldFolderChain"), @"if\s+CompareText\(szCurrent,\s*szRoot\)\s*=\s*0\s+then").Count,
            "HoldFolderChain takes the last backslash off the root of a volume it ends at, or holds a missing name below.");
         AssertNoPlainMessageBox(extension, "PrepareProgramFolders", "CheckProgramFolders", "HardenHeldFolder", "RebuildFolderOfUntrustedOwner", "HeldFolderIsHardened", "DataFolderThatInherits", "InheritsNothing", "OwnerIsAllowed");
      }

      [Test]
      [Description("Work item 78, R3: setup holds the program folder, Bin and ControlPanel from the root of their volume down, as itself and not shared for deleting, from the Ready page until DeinitializeSetup, and refuses - before anything changes and before the service is stopped, exit code 7 when silent - a junction, symbolic link or mount point on the way that anybody but SYSTEM, the Administrators and TrustedInstaller owns or may change or remove (ProtectedFile.cpp's OnlyAdministratorsControl, read through the handle that holds it), saying how to make it the administrators' alone; one they alone control is gone through and the path walked again from where it leads. A name the walk found missing is not held: what is there is decided by the walk alone, and the three walks are held to agree. In 6.3.7 and earlier setup installed through any link (the installer smoke VM, 6 October 2026: through a junction a standard account owned).")]
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
                                         "HoldFolderChain(szTarget, Depth + 1, Real, Held, HeldHandle, Whole, Why)", "Depth >= 8", "LinkRemedy(szNext)", "KindOfDrive(szRoot) = HM_DRIVE_REMOTE", "parent := handle;" })
            // A word, not a part of one: "Depth >= 8" is not in "Depth >= 80" (check 3 of
            // R1 to R4, 27 October 2026).
            Assert.IsTrue(Regex.IsMatch(chain, @"(?<!\w)" + Regex.Escape(word) + @"(?!\w)"), "HoldFolderChain does not use " + word + ".");
         Assert.IsTrue(chain.IndexOf("HoldForSetup(szNext)", StringComparison.Ordinal) < chain.IndexOf("LinkIsAdministratorsAlone(", StringComparison.Ordinal),
            "A link is looked at before it is held.");
         Assert.AreEqual(2, Regex.Matches(chain, @"attributes\s*:=\s*FolderAttributes\(szNext\);").Count, "A folder's attributes are not read before and after it is held.");
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

         // A folder on the way that is not a link - the root of its volume included - is
         // held to the live update's rule of a folder above the program folder
         // (AdminUpdateChecks.cpp's CheckAncestors_ and ReplaceRights_): owned by SYSTEM,
         // the Administrators or TrustedInstaller, and nobody else may rename it, remove
         // what is in it or change who may (the review of 26 October 2026).
         Match replace = Regex.Match(extension, @"HM_REPLACE_RIGHTS\s*=\s*\$([0-9A-Fa-f]+);");
         Assert.IsTrue(replace.Success, "HM_REPLACE_RIGHTS is not there.");
         uint replaceMask = Convert.ToUInt32(replace.Groups[1].Value, 16);
         string updateChecks = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\AdminUpdateChecks.cpp");
         Match above = Regex.Match(updateChecks, @"const DWORD ReplaceRights_\s*=\s*([A-Z_ |]+);");
         Assert.IsTrue(above.Success, "AdminUpdateChecks.cpp no longer names the rights it counts on a folder above the program folder.");
         foreach (string name in above.Groups[1].Value.Split('|').Select(part => part.Trim()))
         {
            Assert.IsTrue(rights.ContainsKey(name), "ReplaceRights_ counts " + name + ", which this test does not know.");
            Assert.AreNotEqual(0u, replaceMask & rights[name], "Setup does not count " + name + " on a folder above the program folder, which the live update does.");
         }
         string aboveRule = CodeBody(extension, "FolderAboveIsAdministratorsAlone");
         StringAssert.Contains("ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION", aboveRule, "A folder above the program folder is not read through the handle that holds it.");
         Assert.IsFalse(Regex.IsMatch(aboveRule, @"ReadFolder(Permissions|Owner)\("), "A folder above the program folder is read by its path.");
         foreach (string word in new[] { "FolderAboveIsAdministratorsAlone(parent, szRoot, szWhy)", "FolderAboveIsAdministratorsAlone(handle, szNext, szWhy)" })
            StringAssert.Contains(word, chain, "HoldFolderChain does not use " + word + ".");

         // Where a link leads, read from the link (FSCTL_GET_REPARSE_POINT).
         string target = CodeBody(extension, "LinkTarget");
         StringAssert.Contains("ReadReparseData(hLink, HM_GET_REPARSE_POINT", target);
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_GET_REPARSE_POINT\s*=\s*\$000900A8;"), "FSCTL_GET_REPARSE_POINT is not $000900A8.");
         Assert.IsTrue(target.Contains("(tagHigh = $A000) and (tagLow = $0003)") && target.Contains("(tagHigh = $A000) and (tagLow = $000C)"),
            "LinkTarget does not know a mount point's and a symbolic link's tags, and nothing else.");
         StringAssert.Contains("if not (OwnerTrusted and SidIs(pAce + 8, 'S-1-3-4')) then", CodeBody(extension, "AclLetsOthers"));

         // The check refuses - PrepareToInstall's answer, exit 7 when silent - before it
         // changes anything, and before the service is stopped.
         string check = CodeBody(extension, "CheckProgramFolders");
         Assert.IsFalse(Regex.IsMatch(check, @"\b(MakeFolders|HardenHeldFolder|RebuildFolderOfUntrustedOwner)\("), "The check, run before the service is stopped, changes a folder.");
         Assert.IsFalse(Regex.IsMatch(check, @"ReleaseFolder\(") || Regex.IsMatch(CodeBody(extension, "PrepareProgramFolders"), @"ReleaseFolder\("),
            "A folder is let go before the installation ends.");
         Assert.GreaterOrEqual(Regex.Matches(check, @"Nothing has been changed\.'").Count, 8, "A refusal of the check does not say that nothing has been changed.");
         string ready = CodeBody(extension, "NextButtonClick");
         int checkedAt = ready.IndexOf("CheckFoldersBeforeTheStop(PreviousInstallationFolder())", StringComparison.Ordinal);
         Assert.IsTrue(checkedAt >= 0 && checkedAt < ready.IndexOf("StopHMailServerService()", StringComparison.Ordinal),
            "The folders are not checked before the service is stopped, so a refusal would leave a running server stopped.");

         AssertNoPlainMessageBox(extension, "HoldFolderChain", "LinkIsAdministratorsAlone", "LinkTarget", "HoldForSetup", "WalksAgree");
      }

      [Test]
      [Description("Work item 78: every mask setup's [Code] tests an entry's rights against is a named HM_ constant that counts a generic right as the rights it means for a folder - it carries exactly the generic rights whose mapping (FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS) meets one of its own - as the server maps an entry's generic rights before it compares (ServiceAccount.cpp's FileRights_); and what reads is FILE_LIST_DIRECTORY with GENERIC_READ and GENERIC_ALL ($90000001), as EveryUserReads_ asks the mapped rights for FILE_READ_DATA. An entry handed down is often written with generic rights: Program Files hands Users (OI)(CI)(IO)(GR,GE) down, and with GENERIC_READ left out of the read mask setup made C:\\Program Files\\hMailServer with no entry for Users (the installer smoke VM, 23 October 2026).")]
      public void EveryMaskSetupTestsAnEntryAgainstCountsGenericRightsAsWhatTheyMean()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         var constants = new Dictionary<string, uint>();
         foreach (Match constant in Regex.Matches(extension, @"^\s*(HM_\w+)\s*=\s*\$([0-9A-Fa-f]+)\s*;", RegexOptions.Multiline))
            constants[constant.Groups[1].Value] = Convert.ToUInt32(constant.Groups[2].Value, 16);

         // The masks an entry's rights are tested against: head.Mask and ..., and the
         // rights AclLetsOthers and OwnEntriesLetOthers are asked about (their own tests
         // are of their parameter).
         // Whatever the expression: a mask that is not a named constant alone fails below.
         var tested = new HashSet<string>(Regex.Matches(extension, @"\.Mask\s+and\s+([^)]*)\)").Cast<Match>().Select(match => match.Groups[1].Value.Trim()));
         tested.UnionWith(Regex.Matches(extension, @"\b(?:AclLetsOthers|OwnEntriesLetOthers)\(([^,()]*),([^,]*),").Cast<Match>().Select(match => match.Groups[2].Value.Trim()));
         tested.Remove("Rights");
         foreach (string name in new[] { "HM_FOLDER_WRITE_RIGHTS", "HM_FOLDER_READ_RIGHTS", "HM_REMOVE_FROM_FOLDER_RIGHTS" })
            Assert.IsTrue(tested.Contains(name), "No entry's rights are tested against " + name + " any more; update this test.");

         // Each generic right, its bit, and what it means for a file or a folder.
         var generic = new[] { new { Name = "GENERIC_READ", Bit = 0x80000000u, Means = 0x120089u }, new { Name = "GENERIC_WRITE", Bit = 0x40000000u, Means = 0x120116u },
                               new { Name = "GENERIC_EXECUTE", Bit = 0x20000000u, Means = 0x1200A0u }, new { Name = "GENERIC_ALL", Bit = 0x10000000u, Means = 0x1F01FFu } };
         foreach (string name in tested)
         {
            Assert.IsTrue(constants.ContainsKey(name), "An entry's rights are tested against " + name + ", which is not a named HM_ mask.");
            uint value = constants[name];
            uint own = value & 0x0FFFFFFFu;
            foreach (var right in generic)
               Assert.AreEqual((right.Means & own) != 0, (value & right.Bit) != 0,
                  name + " ($" + value.ToString("X8") + ") does not count " + right.Name + " as what it means for a folder.");
         }

         Assert.AreEqual(0x90000001u, constants["HM_FOLDER_READ_RIGHTS"], "What reads is not FILE_LIST_DIRECTORY with GENERIC_READ and GENERIC_ALL.");
         foreach (string routine in new[] { "EveryUserMayRead", "EveryUserMayReadMadeIn" })
            StringAssert.Contains("head.Mask and HM_FOLDER_READ_RIGHTS", CodeBody(extension, routine), routine + " does not test an entry's rights against HM_FOLDER_READ_RIGHTS.");
         string serviceAccount = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         StringAssert.Contains("FileRights_(allow->Mask) & FILE_READ_DATA", CppFunctionCode(serviceAccount, "bool EveryUserReads_(", "ServiceAccount.cpp"),
            "The server no longer asks the mapped rights for FILE_READ_DATA before it gives Users Read & execute; hold the installer's read mask to what it asks now.");
      }

      /// <summary>
      ///    Work item 78's R1 to R4: each decision and the statement it is held to - as
      ///    build/check-installer-scripts.py's PROGRAM_FOLDER_STATEMENTS holds it - as
      ///    (routine, what a change of it would do, its statement): Pascal as the script
      ///    has it, where a space stands for any run of white space, ~ for anything up
      ///    to the next semicolon, @@ for comment lines, and a ^ that starts it or a $
      ///    that ends it for the start or the end of the routine.
      /// </summary>
      private static readonly string[][] ProgramFolderStatements =
      {
         new[] { "HoldFolderChain", "a name that cannot be read is taken for one that is not there", "if (lastError = 2) or (lastError = 3) then begin" },
         new[] { "HoldFolderChain", "a folder is not read again once it is held", "handle := HoldForSetup(szNext); if handle = -1 then begin Why := ~; bDone := true; end else begin @@attributes := FolderAttributes(szNext); if (attributes = HM_INVALID_ATTRIBUTES) or ((attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) then" },
         new[] { "HoldFolderChain", "a link is gone through without the rule", "else if (attributes and HM_REPARSE_POINT) <> 0 then begin if not LinkIsAdministratorsAlone(handle, parent, szWhy) then Why := " },
         new[] { "HoldFolderChain", "what a missing name is made below is not the deepest folder held", "if CompareText(szCurrent, szRoot) = 0 then Held := szRoot else Held := RemoveBackslashUnlessRoot(szCurrent); HeldHandle := parent; Whole := false; Result := true;" },
         new[] { "HoldFolderChain", "the root of a volume reached by its name is taken for its device, its last backslash taken off", "if CompareText(szCurrent, szRoot) = 0 then Real := szRoot else Real := RemoveBackslashUnlessRoot(szCurrent); Held := Real;" },
         new[] { "IsVolumeRoot", "more than the root of a volume is taken for one, or the root of a volume reached by its name is not", "^function IsVolumeRoot(Path: String): Boolean; begin Result := Length(Path) <= 3; if not Result then if CompareText(Copy(Path, 1, 11), '\\\\?\\Volume{') = 0 then Result := Copy(Path, Length(Path) - 1, 2) = '}\\'; end;$" },
         new[] { "HoldFolderChain", "the last folder found is not marked as held", "Real := szNext; Held := szNext; HeldHandle := handle; Whole := true; Result := true;" },
         new[] { "LinkIsAdministratorsAlone", "a link's own owner is not held to the three", "if not IsTrustedSid(pOwner) then Why := 'it is owned by '" },
         new[] { "LinkIsAdministratorsAlone", "what may change a link is not asked", "else if AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho) then Why := szWho + ' may change it'" },
         new[] { "LinkIsAdministratorsAlone", "what may remove a link from its folder is not asked", "else if AclLetsOthers(pDacl, HM_REMOVE_FROM_FOLDER_RIGHTS, true, szWho) then Why := " },
         new[] { "AclLetsOthers", "the rights on a link itself are not weighed", "if (aceFlags and HM_INHERIT_ONLY) = 0 then begin if aceType = HM_ACCESS_ALLOWED then" },
         new[] { "IsTrustedSid", "an account other than the three is trusted", "^function IsTrustedSid(pSid: cardinal): Boolean; begin Result := SidIs(pSid, 'S-1-5-18') or SidIs(pSid, 'S-1-5-32-544') or SidIs(pSid, HM_TRUSTED_INSTALLER_SID); end;$" },
         new[] { "HoldFolderChain", "the root of the volume is not held to the rule of a folder above the program folder", "if szRest <> '' then if not IsAtOrAboveChecked(szRoot) then if not FolderAboveIsAdministratorsAlone(parent, szRoot, szWhy) then begin Why := szWhy; Exit; end;" },
         new[] { "HoldFolderChain", "a folder above the program folder that another account owns, or may rename or empty, is gone through, or the walk goes on from a folder it did not take", "if szRest <> '' then if not IsAtOrAboveChecked(szNext) then if not FolderAboveIsAdministratorsAlone(handle, szNext, szWhy) then begin Why := szWhy; bDone := true; end; if not bDone then begin parent := handle; szCurrent := szNext;" },
         new[] { "FolderAboveIsAdministratorsAlone", "a folder above the program folder is not read through the handle that holds it, or one that cannot be read is taken", "readResult := ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin Why := ~; Exit; end;" },
         new[] { "FolderAboveIsAdministratorsAlone", "a folder above the program folder that another account owns is gone through", "if not IsTrustedSid(pOwner) then Why := " },
         new[] { "FolderAboveIsAdministratorsAlone", "what may rename or empty a folder above the program folder, or change who may, is not asked", "else if AclLetsOthers(pDacl, HM_REPLACE_RIGHTS, true, szWho) then begin" },
         new[] { "IsAtOrAboveChecked", "Bin's and ControlPanel's walks pass folders the program folder's walk did not hold to the rule", "^function IsAtOrAboveChecked(Folder: String): Boolean; var szFolder: String; begin Result := false; if g_szFolderAboveChecked = '' then Exit; szFolder := AddBackslash(Folder); Result := CompareText(Copy(AddBackslash(g_szFolderAboveChecked), 1, Length(szFolder)), szFolder) = 0; end;$" },
         new[] { "CheckProgramFolders", "the program folder's walk passes a folder above it unasked", "g_szFolderAboveChecked := ''; g_hProgramFolderPrepared := -1; g_hBinPrepared := -1; g_hControlPanelPrepared := -1; g_bProgramFolderHardened := false; @@szWhy := RemoveBackslashUnlessRoot(ExpandFileName(szApp)); if CompareText(szWhy, szApp) <> 0 then begin Result := ~; Exit; end; CheckProgramFolderPermissions(szApp, false); @@if RestartNeededFor(szApp, szWhy) then begin Result := ~; Exit; end; if not HoldFolderChain(szApp, 0," },
         new[] { "CheckProgramFolders", "Bin's and ControlPanel's walks are not held to the rule past what the program folder's walk held", "g_szFolderAboveChecked := g_szProgramFolderReal; if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0," },
         new[] { "SidIs", "a SID is taken for another, so that every owner is trusted or every group of every user is not one (the review of 26 October 2026)", "^function SidIs(pSid: cardinal; szSid: String): Boolean; var pOther: cardinal; begin Result := false; if pSid = 0 then Exit; pOther := 0; if SidOfText(szSid, pOther) then begin Result := SidsAreEqual(pSid, pOther); FreeWindowsMemory(pOther); end; end;$" },
         new[] { "IsBroadSid", "a group of every user - a domain's Domain Users and Domain Guests among them, as ServiceAccount.cpp's IsBroad_ counts them (check 6 of R1 to R4, 28 October 2026) - is not taken for one, so that an entry that lets every user write is kept", "^function IsBroadSid(pSid: cardinal): Boolean; var szSid: String; szName: String; group: Integer; begin Result := false; group := 0; szSid := BroadGroupSid(group, szName); while (not Result) and (szSid <> '') do begin if SidIs(pSid, szSid) then Result := true; group := group + 1; szSid := BroadGroupSid(group, szName); end; if not Result then begin szSid := TextOfSidAt(pSid); Result := (Length(szSid) > 13) and (CompareText(Copy(szSid, 1, 9), 'S-1-5-21-') = 0) and ((Copy(szSid, Length(szSid) - 3, 4) = '-513') or (Copy(szSid, Length(szSid) - 3, 4) = '-514')); end; end;$" },
         new[] { "BroadGroupSid", "a group of every user is left out, or its SID mistyped - each of ServiceAccount.cpp's BroadSids_, eight of which were missing before check 5 of R1 to R4 (27 October 2026), and four more before check 6 (28 October 2026)", "^function BroadGroupSid(Index: Integer; var Name: String): String; begin Result := ''; Name := ''; case Index of 0: begin Result := 'S-1-1-0'; Name := 'Everyone'; end; 1: begin Result := 'S-1-5-11'; Name := 'Authenticated Users'; end; 2: begin Result := 'S-1-5-32-545'; Name := 'Users'; end; 3: begin Result := 'S-1-5-4'; Name := 'INTERACTIVE'; end; 4: begin Result := 'S-1-2-0'; Name := 'LOCAL'; end; 5: begin Result := 'S-1-5-32-546'; Name := 'Guests'; end; 6: begin Result := 'S-1-5-32-547'; Name := 'Power Users'; end; 7: begin Result := 'S-1-5-32-555'; Name := 'Remote Desktop Users'; end; 8: begin Result := 'S-1-5-14'; Name := 'REMOTE INTERACTIVE LOGON'; end; 9: begin Result := 'S-1-5-13'; Name := 'TERMINAL SERVER USER'; end; 10: begin Result := 'S-1-2-1'; Name := 'CONSOLE LOGON'; end; 11: begin Result := 'S-1-5-2'; Name := 'NETWORK'; end; 12: begin Result := 'S-1-5-3'; Name := 'BATCH'; end; 13: begin Result := 'S-1-5-6'; Name := 'SERVICE'; end; 14: begin Result := 'S-1-5-7'; Name := 'ANONYMOUS LOGON'; end; 15: begin Result := 'S-1-15-2-1'; Name := 'ALL APPLICATION PACKAGES'; end; 16: begin Result := 'S-1-15-2-2'; Name := 'ALL RESTRICTED APPLICATION PACKAGES'; end; 17: begin Result := 'S-1-5-15'; Name := 'This Organization'; end; 18: begin Result := 'S-1-5-113'; Name := 'Local account'; end; 19: begin Result := 'S-1-18-1'; Name := 'Authentication authority asserted identity'; end; 20: begin Result := 'S-1-5-80-0'; Name := 'ALL SERVICES'; end; end; end;$" },
         new[] { "AclLetsOthers", "an entry of a form setup does not read, or one it cannot read, is taken for one that lets nobody, or another right counted", "^function AclLetsOthers(pDacl: cardinal; Rights: cardinal; OwnerTrusted: Boolean; var Who: String): Boolean; var info: ACL_SIZE_INFO; head: ACE_START; pAce: cardinal; index: cardinal; aceType: cardinal; aceFlags: cardinal; begin Result := false; Who := ''; if pDacl = 0 then begin Result := true; Who := 'everyone (it has no list of permissions at all)'; Exit; end; if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then begin Result := true; Who := 'an account in a list of permissions that could not be read'; Exit; end; index := 0; while (not Result) and (index < info.AceCount) do begin pAce := 0; if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then begin CopyAceStart(head, pAce, 8); aceType := head.TypeFlagsSize and $FF; aceFlags := (head.TypeFlagsSize shr 8) and $FF; if (aceFlags and HM_INHERIT_ONLY) = 0 then begin if aceType = HM_ACCESS_ALLOWED then begin if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) then begin if not (OwnerTrusted and SidIs(pAce + 8, 'S-1-3-4')) then begin Result := true; Who := SidNameAt(pAce + 8); end; end; end else if (aceType = 4) or (aceType = 5) or (aceType = 9) or (aceType = 11) then begin Result := true; Who := 'an account in an entry of a form setup does not read'; end; end; end else begin Result := true; Who := 'an account in an entry that could not be read'; end; index := index + 1; end; end;$" },
         new[] { "HoldForSetup", "a folder is held shared for deleting, or through its link, or not kept until the end", "^function HoldForSetup(Folder: String): Integer; begin Result := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if Result <> -1 then begin SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1); g_aHeldFolders[GetArrayLength(g_aHeldFolders) - 1] := Result; end; end;$" },
         new[] { "HoldFolderChain", "a network drive is taken", "if KindOfDrive(szRoot) = HM_DRIVE_REMOTE then begin Why := " },
         new[] { "LinkTarget", "a reparse point other than a mount point or a symbolic link is gone through", "if (tagHigh = $A000) and (tagLow = $0003) then start := 9 else if (tagHigh = $A000) and (tagLow = $000C) then begin start := 11; relative := (Ord(buffer[9]) and 1) <> 0; end else begin Why := 'it is a reparse point of a kind setup does not go through (tag ' + IntToStr(tagHigh) + ':' + IntToStr(tagLow) + ')'; Exit; end;" },
         new[] { "CheckWhatIsInside", "an item of Bin or ControlPanel that cannot be opened is taken", "handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then szWho := 'its owner could not be read (Windows error '" },
         new[] { "CheckWhatElseIsInside", "an item of the program folder that cannot be opened is taken", "handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then szWho := 'its owner could not be read (Windows error '" },
         new[] { "HeldFolderIsHardened", "a rebuilt folder is taken although another account may still write it", "else if AnyAccount and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and (not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) then begin Result := false;" },
         new[] { "MakeFolderIn", "a folder is made without its descriptor, or relative to another handle than the folder above it", "attributes.nLength := HM_OBJECT_ATTRIBUTES_SIZE; attributes.RootDirectory := Parent; attributes.ObjectName := pName; attributes.Attributes := HM_OBJ_CASE_INSENSITIVE; attributes.SecurityDescriptor := pDescriptor; attributes.SecurityQualityOfService := 0;" },
         new[] { "LinkTarget", "a link to a share is gone through", "else if CompareText(Copy(szName, 1, 8), '\\??\\UNC\\') = 0 then begin Why := " },
         new[] { "VolumeKeepsPermissions", "a volume that keeps no permissions is taken", "else if (flags and HM_PERSISTENT_ACLS) = 0 then Why := " },
         new[] { "VolumeKeepsPermissions", "a volume that cannot be told is taken", "begin Result := false; Why := ''; serial := 0; longest := 0; flags := 0; if not VolumeOfHeldFolder(HeldHandle, " },
         new[] { "CheckProgramFolders", "the program folder is not held and checked", "if not HoldFolderChain(szApp, 0, g_szProgramFolderReal, g_szProgramFolderHeld, g_hProgramFolder, g_bProgramFolderThere, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "Bin is not held and checked", "if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0, g_szBinReal, g_szBinHeld, g_hBin, g_bBinThere, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "ControlPanel is not held and checked", "if not HoldFolderChain(AddBackslash(szApp) + 'ControlPanel', 0, g_szControlPanelReal, g_szControlPanelHeld, g_hControlPanel, g_bControlPanelThere, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "the program folder's volume is not asked", "if not VolumeKeepsPermissions(g_hProgramFolder, g_szProgramFolderHeld, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "Bin's volume is not asked", "if not VolumeKeepsPermissions(g_hBin, g_szBinHeld, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "ControlPanel's volume is not asked", "if not VolumeKeepsPermissions(g_hControlPanel, g_szControlPanelHeld, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "walks that disagree are not refused", "if (not WalksAgree(g_szBinHeld, g_bBinThere)) or (not WalksAgree(g_szControlPanelHeld, g_bControlPanelThere)) then begin Result := " },
         new[] { "CheckProgramFolders", "the program folder is taken for the earlier installation's whatever that installation's folder", "g_bEarlierInstallation := false; if Trim(Earlier) <> '' then g_bEarlierInstallation := CompareText(RemoveBackslashUnlessRoot(Trim(Earlier)), szApp) = 0;" },
         new[] { "CheckProgramFolders", "the program folder's owner is not held to the rule", "if g_bProgramFolderThere then if not OwnerIsAllowed(g_szProgramFolderReal, g_hProgramFolder, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "a program folder that is the root of a volume is installed into, the uninstaller and its record given what the volume hands down (check 3 of R1 to R4, 27 October 2026)", "if IsVolumeRoot(g_szProgramFolderReal) then begin Result := ~; Exit; end;" },
         new[] { "CheckProgramFolders", "a Bin or a ControlPanel that leads to the root of a volume is installed into", "if IsVolumeRoot(g_szBinReal) or IsVolumeRoot(g_szControlPanelReal) then begin Result := ~; Exit; end;" },
         new[] { "CheckProgramFolders", "whether Bin and ControlPanel are made again is not decided before anything changes, every empty one, but after the program folder's hardening has handed its permissions down to them (check 3 of R1 to R4, 27 October 2026)", "g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal); g_bRemakeControlPanel := g_bControlPanelThere and FolderHoldsNothing(g_szControlPanelReal);" },
         new[] { "CheckProgramFolders", "whether the program folder waits for the files is not decided once, before anything changes, with the decisions of Bin and ControlPanel (check 7 of R1 to R4, 28 October 2026)", "g_bRemakeControlPanel := g_bControlPanelThere and FolderHoldsNothing(g_szControlPanelReal); @@if g_bProgramFolderThere then g_szProgramFolderWaits := ProgramFolderWaitsForTheFiles(); @@if g_bProgramFolderThere then if not OwnerIsAllowed(g_szProgramFolderReal, g_hProgramFolder, szWhy) then" },
         new[] { "CheckProgramFolders", "what an earlier pass made is removed once its holds are let go, by a path nothing holds any more, or what it decided is kept", "RemoveFoldersMadeHere(); ReleaseHeldFolders(); g_szFolderFindings := ''; g_bRemakeBin := false; g_bRemakeControlPanel := false; g_szProgramFolderWaits := '';" },
         new[] { "CheckProgramFolders", "Bin's owner is not held to the rule", "if g_bBinThere then if not OwnerIsAllowed(g_szBinReal, g_hBin, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "ControlPanel's owner is not held to the rule", "if g_bControlPanelThere then if not OwnerIsAllowed(g_szControlPanelReal, g_hControlPanel, szWhy) then begin Result := " },
         new[] { "CheckProgramFolders", "what is in Bin and ControlPanel is not refused before the service is stopped", "if g_bBinThere then CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBin), 0, false, false, found, szWhy); if g_bControlPanelThere then CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanel), 0, false, false, found, szWhy); if g_bProgramFolderThere then CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, false, found, szWhy); if found > 0 then begin Result := " },
         new[] { "OwnerIsAllowed", "a folder another account owns is taken although it is not the earlier installation's", "else if g_bEarlierInstallation then Result := true else Why := " },
         new[] { "WalksAgree", "what one walk found is not held to the others", "if g_bProgramFolderThere then Result := Whole or (CompareText(Held, g_szProgramFolderReal) = 0) else Result := (not Whole) and (CompareText(Held, g_szProgramFolderHeld) = 0);" },
         new[] { "MakeFolders", "a folder is made by its path, through any link put on the way since the walk, or relative to another folder than the one made or held above it (check 5 of R1 to R4, 27 October 2026), or one somebody else made in the meantime is taken", "status := 0; handle := MakeFolderIn(parent, ExtractFileName(missing[index]), pDescriptor, status); if handle <> -1 then begin if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then" },
         new[] { "MakeFolders", "a name somebody else took is not refused, a folder above made a link is not taken for a folder changed while setup prepared it, or another failure is taken", "if status = HM_STATUS_NAME_COLLISION then begin g_bFoldersChanged := true; Why := ~; end else if HeldIsLink(parent, bKnown) then begin g_bFoldersChanged := true; Why := ~; end else Why := ~; Result := false;" },
         new[] { "MakeFolderIn", "a folder is made by a path, through a link, shared for deleting, or opened when it is there rather than made (check 5 of R1 to R4, 27 October 2026)", "Status := MakeFileRelative(handle, HM_MAKE_FOLDER_ACCESS, attributes, io, 0, 0, HM_SHARE_ALL_BUT_DELETE, HM_FILE_CREATE, HM_MAKE_FOLDER_OPTIONS, 0, 0); FreeWindowsMemory(pName); if Status <> 0 then Exit;" },
         new[] { "MakeFolderIn", "the name made is not the one name given, relative to the folder above it, with its length", "CopyTextTo(pName + 8, Name, bytes); nameRecord.Lengths := bytes or ((bytes + 2) shl 16); nameRecord.Buffer := pName + 8; WriteNameRecord(pName, nameRecord, 8);" },
         new[] { "MakeFolderIn", "a folder made is not held until setup ends", "Result := handle; SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1); g_aHeldFolders[GetArrayLength(g_aHeldFolders) - 1] := Result; end;$" },
         new[] { "MakeFolderIn", "a failure to make room for the name is taken for a folder made", "Result := -1; Status := HM_STATUS_NO_MEMORY; bytes := Length(Name) * 2; pName := AllocateWindowsMemory(HM_LPTR, 8 + bytes + 2); if pName = 0 then Exit;" },
         new[] { "CheckProgramFolders", "a program folder path Windows reads as another - a name ending in a dot or a space, . or .. - is held as given, so that setup prepares a folder no path reaches and Inno Setup installs into the one Windows reads, made with what the folder above hands down (check 9 of R1 to R4, 31 October 2026)", "szWhy := RemoveBackslashUnlessRoot(ExpandFileName(szApp)); if CompareText(szWhy, szApp) <> 0 then begin Result := ~; Exit; end; CheckProgramFolderPermissions(szApp, false);" },
         new[] { "MakeFolders", "a name that ends in a dot or a space, which Windows drops from a path, is made by its name relative to the folder above, where no path reaches it (check 9 of R1 to R4, 31 October 2026)", "if (szName <> '') and ((Copy(szName, Length(szName), 1) = '.') or (Copy(szName, Length(szName), 1) = ' ')) then begin Why := ~; Exit; end; if szName <> '' then begin szPath := AddBackslash(szPath) + szName;" },
         new[] { "MakeFolders", "what is made is not the names below the deepest folder held", "szBase := AddBackslash(Held); if (Length(Folder) <= Length(szBase)) or (CompareText(Copy(Folder, 1, Length(szBase)), szBase) <> 0) then" },
         new[] { "MakeFolders", "the folder the walk held is not read again before a folder is made in it, so a link put in its place since is gone through (the review of 26 October 2026)", "Result := true; if HeldIsLink(HeldHandle, bKnown) then begin g_bFoldersChanged := true; if bKnown then Why := ~ else Why := ~; Result := false; end; index := 0; parent := HeldHandle; while Result and (index < count) do" },
         new[] { "MakeFolders", "a folder made is not held and read where it is through its handle, so one made through a link put on the way is taken", "if handle <> -1 then begin if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then begin g_bFoldersChanged := true; Why := ~; Log(~); Result := false; end else begin @@szWhere := FinalPathOfHeld(handle); if Kind = 0 then" },
         new[] { "MakeFolders", "a folder made is recorded without where it was made and the handle that holds it, so that it is removed by a path that may lead elsewhere by then (check 3 of R1 to R4, 27 October 2026)", "SetArrayLength(g_aFoldersMade, GetArrayLength(g_aFoldersMade) + 1); g_aFoldersMade[GetArrayLength(g_aFoldersMade) - 1] := missing[index]; SetArrayLength(g_aFoldersMadeAt, GetArrayLength(g_aFoldersMade)); g_aFoldersMadeAt[GetArrayLength(g_aFoldersMade) - 1] := szWhere; SetArrayLength(g_aFoldersMadeHandle, GetArrayLength(g_aFoldersMade)); g_aFoldersMadeHandle[GetArrayLength(g_aFoldersMade) - 1] := handle; end else if Kind = 1 then begin g_szBinMade := missing[index]; g_szBinMadeAt := szWhere; g_hBinMade := handle; end else begin g_szControlPanelMade := missing[index]; g_szControlPanelMadeAt := szWhere; g_hControlPanelMade := handle; end; Log(~); parent := handle; MadeHandle := handle; end;" },
         new[] { "MadeWhereNamed", "a folder made is taken wherever it is, or where it is when that cannot be read", "if (szParent = '') or (szMade = '') then Where := ~ else if CompareText(szMade, AddBackslash(szParent) + Name) <> 0 then Where := ~ else Result := true;" },
         new[] { "MadeWhereNamed", "where a folder made is is not read through the handles", "Result := false; szParent := FinalPathOfHeld(Parent); szMade := FinalPathOfHeld(Made);" },
         new[] { "HeldIsLink", "a held folder that is a link now, or whose attributes cannot be read, is taken for a folder", "^function HeldIsLink(HeldHandle: Integer; var Known: Boolean): Boolean; var information: HM_FILE_INFORMATION; begin Result := true; Known := ReadHeldInformation(HeldHandle, information); if Known then Result := (information.dwFileAttributes and HM_REPARSE_POINT) <> 0; end;$" },
         new[] { "FinalPathOfHeld", "where a held folder is is not read through its handle, by its volume's name", "size := FinalPathOfHandle(HeldHandle, buffer, capacity, HM_VOLUME_NAME_GUID); if size >= capacity then begin" },
         new[] { "RemoveMadeFolder", "a folder setup made is removed while it is still held, with what is in it, or by its path whatever that leads to now (check 3 of R1 to R4, 27 October 2026)", "LetGoOfHeld(Handle); check := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if check = -1 then begin Log(~); Exit; end; bSame := (not HeldIsLink(check, bKnown)) and (At <> '') and (CompareText(FinalPathOfHeld(check), At) = 0); ReleaseFolder(check); if not bSame then Log(~) else if RemoveDir(Folder) then Log(~) else Log(~);" },
         new[] { "RemoveFoldersMadeHere", "a Bin or ControlPanel setup made is not removed where it was made, before the folders above it", "if g_szBinMade <> '' then RemoveMadeFolder(g_szBinMade, g_szBinMadeAt, g_hBinMade); if g_szControlPanelMade <> '' then RemoveMadeFolder(g_szControlPanelMade, g_szControlPanelMadeAt, g_hControlPanelMade);" },
         new[] { "RemoveFoldersMadeHere", "the folders setup made above and at the program folder are not removed deepest first, each where it was made", "for index := GetArrayLength(g_aFoldersMade) - 1 downto 0 do RemoveMadeFolder(g_aFoldersMade[index], g_aFoldersMadeAt[index], g_aFoldersMadeHandle[index]);" },
         new[] { "PrepareProgramFolders", "a program folder that is not there is decided by its path", "if not g_bProgramFolderThere then begin if not MakeFolders(g_szProgramFolderHeld, g_hProgramFolder, g_szProgramFolderReal, 0, hMade, szWhy) then begin Result := ~; Exit; end; g_hProgramFolderPrepared := hMade; g_bProgramFolderHardened := true;" },
         new[] { "PrepareProgramFolders", "the program folder is left as it is by a rule other than the one the restart rule asks (ProgramFolderWaitsForTheFiles), or what an entry of its own lets a named account do is not said then (check 6 of R1 to R4, 28 October 2026)", "szInherits := g_szProgramFolderWaits; if CompareText(ProgramFolderWaitsForTheFiles(), szInherits) <> 0 then begin g_bFoldersChanged := true; Result := ~; Exit; end; if szInherits <> '' then begin Log(~); SayWriteEntriesKept(g_szProgramFolderReal, g_hProgramFolder); end else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, false, szWhy) then" },
         new[] { "PrepareProgramFolders", "the program folder is left unhardened before the files by an answer asked after the service stop, not the one the restart rule went by, or one that differs from it is taken (check 7 of R1 to R4, 28 October 2026)", "szInherits := g_szProgramFolderWaits; if CompareText(ProgramFolderWaitsForTheFiles(), szInherits) <> 0 then begin g_bFoldersChanged := true; Result := 'Setup does not install into ' + szApp + ': a data folder of the earlier installation in ' + g_szProgramFolderReal + ~restarts Windows~; Exit; end;" },
         new[] { "ProgramFolderWaitsForTheFiles", "the program folder is left as it is for an inheriting data folder that is not the earlier installation's", "^function ProgramFolderWaitsForTheFiles(): String; begin Result := ''; if g_bEarlierInstallation then if DirExists(AddBackslash(g_szProgramFolderReal) + 'Data') or FileExists(AddBackslash(g_szBinReal) + 'hMailServer.INI') then Result := DataFolderThatInherits(g_szProgramFolderReal); end;$" },
         new[] { "PrepareProgramFolders", "a program folder that could not be hardened, or given the file setup keeps in it, is installed into, or one another account owns is taken", "else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, false, szWhy) then begin Result := ~; Exit; end else if not MakePlaceholder(g_szProgramFolderReal, g_hProgramFolder, szWhy) then begin Result := ~; Exit; end else g_bProgramFolderHardened := true; g_hProgramFolderPrepared := g_hProgramFolder;" },
         new[] { "PrepareProgramFolders", "a Bin that is there and held nothing is not made again, one made a link since the walk is hardened through its path, one that could not be hardened or given the file setup keeps in it is installed into, or one another account owns is taken (check 3 and check 4 of R1 to R4, 27 October 2026)", "if g_bBinThere then begin if g_bRemakeBin then begin if not RemakeEmptyFolder(g_szBinReal, g_hBin, 1, hMade, szWhy) then begin Result := ~; Exit; end; g_hBinPrepared := hMade; end else if HeldIsLink(g_hBin, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szBinReal); Exit; end else if not HardenHeldFolder(g_szBinReal, g_hBin, false, szWhy) then begin Result := ~; Exit; end else if not MakePlaceholder(g_szBinReal, g_hBin, szWhy) then begin Result := ~; Exit; end else g_hBinPrepared := g_hBin; end" },
         new[] { "PrepareProgramFolders", "a Bin that is not there is not made by setup", "end else if not MakeFolders(g_szBinHeld, g_hBin, g_szBinReal, 1, hMade, szWhy) then begin Result := ~; Exit; end else g_hBinPrepared := hMade;" },
         new[] { "PrepareProgramFolders", "a ControlPanel that is there and held nothing is not made again, one made a link since the walk is hardened through its path, one that could not be hardened or given the file setup keeps in it is installed into, or one another account owns is taken", "if g_bControlPanelThere then begin if g_bRemakeControlPanel then begin if not RemakeEmptyFolder(g_szControlPanelReal, g_hControlPanel, 2, hMade, szWhy) then begin Result := ~; Exit; end; g_hControlPanelPrepared := hMade; end else if HeldIsLink(g_hControlPanel, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szControlPanelReal); Exit; end else if not HardenHeldFolder(g_szControlPanelReal, g_hControlPanel, false, szWhy) then begin Result := ~; Exit; end else if not MakePlaceholder(g_szControlPanelReal, g_hControlPanel, szWhy) then begin Result := ~; Exit; end else g_hControlPanelPrepared := g_hControlPanel; end" },
         new[] { "PrepareProgramFolders", "the program folder, Bin or ControlPanel made a link before the last look, which goes by their paths, is installed into", "if g_hProgramFolderPrepared <> -1 then if HeldIsLink(g_hProgramFolderPrepared, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szProgramFolderReal); Exit; end; if g_hBinPrepared <> -1 then if HeldIsLink(g_hBinPrepared, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szBinReal); Exit; end; if g_hControlPanelPrepared <> -1 then if HeldIsLink(g_hControlPanelPrepared, bKnown) then begin Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szControlPanelReal); Exit; end;" },
         new[] { "PrepareProgramFolders", "a ControlPanel that is not there is not made by setup", "end else if ControlPanelIsInstalled() then begin if not MakeFolders(g_szControlPanelHeld, g_hControlPanel, g_szControlPanelReal, 2, hMade, szWhy) then begin Result := ~; Exit; end; g_hControlPanelPrepared := hMade; end;" },
         new[] { "PrepareProgramFolders", "what is in Bin and ControlPanel is not looked at again once they are hardened, from where their handles say they are, or what is refused then is not taken for a folder changed while setup prepared it", "if g_bBinThere then CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, true, false, found, szWhy); if g_bControlPanelThere then CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, true, false, found, szWhy); if g_bProgramFolderThere then CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, true, found, szWhy); if found > 0 then begin g_bFoldersChanged := true; Result := ~; Exit; end;" },
         new[] { "HardenHeldFolder", "a held folder's owner and permissions are not read through its handle", "readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);" },
         new[] { "HardenHeldFolder", "an entry of the folder's own that lets every user write is kept", "if ((((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0) and LetsEveryUserWrite(pAce) then begin szSid := TextOfSidAt(pAce + 8);" },
         new[] { "HardenHeldFolder", "an entry that lets every user write is not taken away, or a failure to take it is not a refusal", "'\" /remove:g ' + szSid, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin Why := ~; Exit; end; if ResultCode <> 0 then begin Why := ~; Exit; end;" },
         new[] { "HardenHeldFolder", "a trusted owner's folder does not stop inheriting, or icacls's failure is not a refusal", "'\"' + Folder + '\" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F' + szUsers, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin Why := ~; Exit; end; Log(~); if ResultCode <> 0 then begin Why := ~; Exit; end;" },
         new[] { "HardenHeldFolder", "a folder another account owns is taken although it is not the earlier installation's", "else if not Adopt then begin Why := ~; FreeWindowsMemory(pDescriptor); Exit; end else begin Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, bUsers, false, Why);" },
         new[] { "HardenHeldFolder", "a hardened folder is not read again", "if not HeldFolderIsHardened(HeldHandle, not bTrusted, false, Why) then begin Why := ~; Exit; end; if bTrusted then SayWriteEntriesKept(Folder, HeldHandle) else begin" },
         new[] { "HeldFolderIsHardened", "a list that still inherits is taken for hardened", "else if (control and HM_DACL_PROTECTED) = 0 then Why := " },
         new[] { "HeldFolderIsHardened", "a folder another account still owns is taken for hardened", "else if not IsTrustedSid(pOwner) then Why := " },
         new[] { "HeldFolderIsHardened", "an entry that lets every user write is taken for hardened", "if LetsEveryUserWrite(pAce) then begin Result := false;" },
         new[] { "LetsEveryUserWrite", "what lets every user write is not an allow entry with a right to write for a group of every user", "if (head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED then if (head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0 then Result := IsBroadSid(pAce + 8);" },
         new[] { "RebuildFolderOfUntrustedOwner", "a rebuilt folder keeps an entry of its own that lets another account write", "if aceType <> HM_ACCESS_ALLOWED then szDropped := szDropped + ', an entry of its own of another kind' else if writes and (not IsTrustedSid(pAce + 8)) and (not (Service and SidIs(pAce + 8, HM_SERVICE_SID))) then szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own' else ok := AddAsOwnEntry(pNew, pAce);" },
         new[] { "RebuildFolderOfUntrustedOwner", "a rebuilt folder keeps an inherited entry other than the service's Read", "if aceType <> HM_ACCESS_ALLOWED then szDropped := szDropped + ', an inherited entry of another kind' else if SidIs(pAce + 8, HM_SERVICE_SID) and (Service or (not writes)) then ok := AddAsOwnEntry(pNew, pAce) else if writes and (not SidIs(pAce + 8, 'S-1-5-18')) and (not SidIs(pAce + 8, 'S-1-5-32-544')) then szDropped := ~; end;" },
         new[] { "RebuildFolderOfUntrustedOwner", "the deny entries are not kept on a rebuilt folder", "if pass = 0 then begin if aceType = HM_ACCESS_DENIED then ok := AddAsOwnEntry(pNew, pAce); end" },
         new[] { "RebuildFolderOfUntrustedOwner", "a folder another account owns is not given to the Administrators with their privilege first", "if not Exec(ExpandConstant('{sys}\\takeown.exe'), '/F \"' + Folder + '\" /A', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then Why := ~ else if ResultCode <> 0 then Why := ~ else begin written := WriteFolderSecurity(Folder, HM_FILE_OBJECT, HM_PROTECTED_DACL_INFORMATION, 0, 0, pNew, 0);" },
         new[] { "CheckWhatIsInside", "a program, a library, a folder or a link another account controls is said and not refused", "if bLink or bCode or bFolder then begin Found := Found + 1;" },
         new[] { "CheckWhatIsInside", "a link, a program or a library inside Bin or ControlPanel is not known as one", "bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0; bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsSettingsFile(rec.Name) or (Scripts and IsScriptFile(rec.Name))); bKeyStore := (not bFolder) and (CompareText(rec.Name, 'hMailServerApiKeys.ini') = 0);" },
         new[] { "IsSettingsFile", "a file that tells the service, the tools or the uninstaller what to run, or where, is said, not refused, when another account controls it (check 3 of R1 to R4, 27 October 2026)", "^function IsSettingsFile(Name: String): Boolean; var szExtension: String; begin szExtension := Lowercase(ExtractFileExt(Name)); Result := (CompareText(Name, 'hMailServer.INI') = 0) or (CompareText(Name, 'hMailServerApiKeys.ini') = 0) or (szExtension = '.config') or (szExtension = '.json') or ((CompareText(Copy(Name, 1, 5), 'unins') = 0) and (szExtension = '.dat')); end;$" },
         new[] { "CheckWhatIsInside", "an owner other than the three is not refused", "else if (not IsTrustedSid(pOwner)) and (not (bKeyStore and SidIs(pOwner, HM_SERVICE_SID))) then begin if (not bCode) and (not bFolder) and SidIs(pOwner, HM_SERVICE_SID) then szWho := ''" },
         new[] { "CheckWhatIsInside", "a program or a folder an entry of whose own lets another account change it is not refused", "else if bCode or bFolder then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, bKeyStore, szText) then" },
         new[] { "CheckWhatIsInside", "what is below the first level is not looked at, or a folder there is walked without being the folder of its name in the one above it (check 4 of R1 to R4, 27 October 2026)", "for index := 0 to count - 1 do if not CheckWhatIsInside(folders[index], AddBackslash(szFinal) + ExtractFileName(folders[index]), Depth + 1, Final, Scripts, Found, Why) then Result := false; ReleaseFolder(folderHandle); end;$" },
         new[] { "CheckWhatIsInside", "the owner of what is in Bin and ControlPanel is not read through a handle on each", "readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);" },
         new[] { "IsProgramFile", "a program or a library is not what the live update holds to its rule", "Result := (szExtension = '.exe') or (szExtension = '.dll');" },
         new[] { "OwnEntriesLetOthers", "an inherited entry is weighed, or an entry of the item's own is not", "if (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0 then begin if aceType = HM_ACCESS_ALLOWED then begin if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) and (not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) then" },
         new[] { "IsScriptFile", "a script the service, DBSetup or the web server runs is not counted as a program", "Result := (szExtension = '.vbs') or (szExtension = '.js') or (szExtension = '.sql') or (szExtension = '.php') or (szExtension = '.ps1') or (szExtension = '.cmd') or (szExtension = '.bat');" },
         new[] { "IsDataFolderName", "a folder other than the data folders the service writes in is taken for one", "Result := (CompareText(Name, 'Data') = 0) or (CompareText(Name, 'Logs') = 0) or (CompareText(Name, 'Temp') = 0) or (CompareText(Name, 'Database') = 0);" },
         new[] { "CheckWhatElseIsInside", "which items of the program folder are links, programs, scripts or data folders is not read", "bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0; bData := bFolder and IsDataFolderName(rec.Name); bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsScriptFile(rec.Name) or IsSettingsFile(rec.Name));" },
         new[] { "CheckWhatElseIsInside", "a link in the program folder that another account controls is taken (the review of 26 October 2026), or one on the program folder's own level that is not a data folder's is taken whoever controls it (check 7 of R1 to R4, 28 October 2026)", "if bLink then begin @@if not bData then begin if not LinkIsAdministratorsAlone(handle, -1, szText) then szWho := ~ else if bFolder and IsRunFolderName(rec.Name) then szWho := ~ else szWho := ~; if bFolder then szRemedy := ~ else szRemedy := ~; end else if not LinkIsAdministratorsAlone(handle, -1, szText) then begin szWho := ~; szRemedy := LinkRemedy(szItem) + '.'; end else if not DataLinkLeadsWhereAllowed(szItem, Final, szText) then begin szWho := ~; szRemedy := ~; end; end" },
         new[] { "CheckWhatElseIsInside", "a link on the program folder's own level that only the administrators control is not refused, or not with a remedy that removes it, when it is not a data folder's (check 7 of R1 to R4, 28 October 2026)", "else szWho := 'is a junction, a symbolic link or a mount point, and on the program folder''s own level setup takes a link only for Data, Logs, Temp or ' + ~; if bFolder then szRemedy := 'An administrator removes the link - rmdir \"' + szItem + ~ else szRemedy := 'An administrator removes the link - del \"' + szItem + ~; end" },
         new[] { "IsRunFolderName", "a folder setup installs into that holds what the service or the administrators run is taken behind a link (check 3 of R1 to R4, 27 October 2026)", "^function IsRunFolderName(Name: String): Boolean; begin Result := (CompareText(Name, 'Addons') = 0) or (CompareText(Name, 'Events') = 0) or (CompareText(Name, 'DBScripts') = 0) or (CompareText(Name, 'WebAdmin') = 0) or (CompareText(Name, 'Languages') = 0); end;$" },
         new[] { "DataLinkLeadsWhereAllowed", "where a data folder's link leads is not held to the rules of the program folder's own way, or to what the grant after the files refuses (check 4 of R1 to R4, 27 October 2026), or one that is not there is taken", "hLink := HoldForSetup(Link); if hLink = -1 then begin Why := ~; Exit; end; if not HeldIsLink(hLink, bKnown) then begin Why := ~; Exit; end; if not LinkTarget(hLink, Link, szTarget, szWhy) then begin Why := szWhy; Exit; end; if not HoldFolderChain(szTarget, 1, szReal, szHeld, hHeld, bWhole, szWhy) then begin Why := ~; Exit; end; if not bWhole then begin Why := ~; Exit; end; if not VolumeKeepsPermissions(hHeld, szHeld, szWhy) then begin Why := ~; Exit; end; @@if not DataLinkTargetIsGrantable(hHeld, szReal, szWhy) then begin Why := szWhy; Exit; end; if CompareText(ExtractFileName(Link), 'Database') = 0 then if not DatabaseTargetIsTheAdministrators(hHeld, szReal, szWhy) then begin Why := szWhy; Exit; end; if not DataFolderIsAllowed(szReal, hHeld, bOwner, szWho, szRemedy) then begin" },
         new[] { "CheckWhatElseIsInside", "the owner of what is in the program folder is not read through a handle on each", "readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);" },
         new[] { "CheckWhatElseIsInside", "an owner other than the three is taken in the program folder", "else if not IsTrustedSid(pOwner) then szWho := 'is owned by ' + SidNameAt(pOwner) + " },
         new[] { "CheckWhatElseIsInside", "an entry of its own that lets another account change a program, a script or a folder of the program folder is taken", "else if bCode or bFolder then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, false, szText) then" },
         new[] { "CheckWhatElseIsInside", "a link, a program, a script or a folder - a data folder among them - another account controls in the program folder is said, not refused (check 5 of R1 to R4, 27 October 2026)", "if bLink or bCode or bFolder then begin Found := Found + 1;" },
         new[] { "CheckWhatElseIsInside", "a data folder is not held to its rule, the earlier installation's that another account owns is installed into without a restart or one of a new installation taken, or the Database's files are not looked at (check 5 of R1 to R4, 27 October 2026)", "else if bData then begin @@if not DataFolderIsAllowed(szItem, handle, bOwner, szWho, szRemedy) then begin if bOwner and g_bEarlierInstallation and (not Final) then begin AddRestartFolder(szItem, -1, true, false, szItem + ' was ' + Copy(szWho, 4, Length(szWho))); szWho := ''; szRemedy := ''; end; end else if CompareText(rec.Name, 'Database') = 0 then szWho := DatabaseFilesWhy(szItem, szRemedy); end" },
         new[] { "CheckWhatElseIsInside", "a program, a script or a settings file of the program folder that an entry it kept from where it was made lets another account change is not noted for the restart rule (check 5 of R1 to R4, 27 October 2026)", "else if bCode and (not Final) and StaleEntryLetsOthers(pDacl, pAppDescriptor, pAppDacl, false, false, szText) then NoteStaleItem(szItem, szText);" },
         new[] { "CheckWhatElseIsInside", "what the program folder hands down to the files in it is not read through its handle, or one that cannot be read is taken to hand everything down", "if ReadHeldSecurity(AppHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pAppOwner, 0, pAppDacl, 0, pAppDescriptor) <> 0 then pAppDescriptor := 0;" },
         new[] { "CheckWhatElseIsInside", "the folders setup installs into that hold what is run are not these five", "0: szFolder := 'Addons'; 1: szFolder := 'Events'; 2: szFolder := 'DBScripts'; 3: szFolder := 'WebAdmin'; 4: szFolder := 'Languages'; 5: szFolder := 'MySQL';" },
         new[] { "CheckWhatElseIsInside", "what is in those folders is not looked at at any depth, scripts counted as programs", "if not CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why) then Result := false;" },
         new[] { "LinkIsAdministratorsAlone", "the folder a link is in is not asked about unless the link is in the program folder itself", "FreeWindowsMemory(pDescriptor); if not Result then Exit; if hParent = -1 then Exit; Result := false;" },
         new[] { "RemakeEmptyFolder", "a Bin or ControlPanel that holds nothing is not let go, removed and made again, or a failure of either is taken (the review of 26 October 2026)", "parent := HoldForSetup(szParent); if parent = -1 then begin Why := ~; Exit; end; LetGoOfHeld(HeldHandle); if not RemoveDir(Folder) then begin g_bFoldersChanged := true; Why := ~; Exit; end; Log(~); Result := MakeFolders(szParent, parent, Folder, Kind, MadeHandle, Why);" },
         new[] { "LetGoOfHeld", "a folder let go is let go again at the end, or another is", "if g_aHeldFolders[index] = Handle then begin ReleaseFolder(Handle); last := GetArrayLength(g_aHeldFolders) - 1; g_aHeldFolders[index] := g_aHeldFolders[last]; SetArrayLength(g_aHeldFolders, last); end;" },
         new[] { "FolderHoldsNothing", "a folder that holds something is taken for empty, or an empty one for one that holds something", "Result := false; if FindFirst(AddBackslash(Folder) + '*', rec) then begin try Result := true; repeat if (rec.Name <> '.') and (rec.Name <> '..') then Result := false; until (not Result) or (not FindNext(rec));" },
         new[] { "CheckWhatIsInside", "an empty folder in Bin, ControlPanel or the folders that hold what is run is left, once they are hardened, for a handle opened before to make a junction, or removed by its path rather than through a handle read as that folder (check 4 of R1 to R4, 27 October 2026)", "if Final and FolderHoldsNothing(szItem) and (not IsAtOrBelow(szItem, AddBackslash(g_szProgramFolderReal) + 'MySQL')) then begin if RemoveHeldFolder(szItem, szFinal, szRemoved) then Log('Setup removed ' + szItem + ', an empty folder there before this installation" },
         new[] { "CheckWhatIsInside", "an empty folder that could not be removed is taken", "Log('The program folder: refused, ' + szText); Result := false; end; end else begin SetArrayLength(folders, count + 1);" },
         new[] { "CheckWhatElseIsInside", "an empty folder of the program folder's own level that setup makes again - one of the five, a data folder when the server is installed - is left, once it is hardened, for a handle opened before to make a junction, or removed by its path rather than through a handle read as that folder (check 4 of R1 to R4, 27 October 2026); or an administrator's empty folder setup neither installs into nor makes again is removed (check 5); or one removed is not made again at once (check 8); or the setup log says again, after RemoveAndMakeAgain has said which way it made the folder again, that it was made again with the permissions it had, which one that inherited was not (the fold of R1 to R4, 31 October 2026)", "else if Final and bFolder and (not bLink) and (not IsRunFolderName(rec.Name)) and (not (bData and IsComponentSelected('server'))) and FolderHoldsNothing(szItem) then @@Log('The program folder: ' + szItem + ' is an empty folder there before this installation, which setup neither installs into nor makes again, and is left ' + 'as it is.') else if Final and bFolder and (not bLink) and FolderHoldsNothing(szItem) then begin @@if not RemoveAndMakeAgain(szItem, AppHandle, szAppFinal, szRemoved) then begin szText := szItem + ' is an empty folder there before this installation, and could not be removed and made again - '" },
         new[] { "RemoveAndMakeAgain", "an empty folder of the program folder's own level is removed without its permissions read first through a handle on it as itself, or by a path rather than through a handle read as that folder (check 8 of R1 to R4, 28 October 2026)", "handle := HoldFolder(Item, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then begin Why := ~; Exit; end; pOwner := 0; pDacl := 0; pDescriptor := 0; readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); ReleaseFolder(handle); if readResult <> 0 then begin Why := ~; Exit; end; @@control := 0; revision := 0; bInherits := true; if ControlOfDescriptor(pDescriptor, control, revision) then bInherits := (control and HM_DACL_PROTECTED) = 0; if not RemoveHeldFolder(Item, AppFinal, Why) then begin FreeWindowsMemory(pDescriptor); Exit; end;" },
         new[] { "RemoveAndMakeAgain", "an empty folder removed in the last look is left for Inno Setup to make again with what the program folder hands down, or made by a path, or with other permissions than it had - the list it had when it inherited nothing; what the program folder hands down now and the entries of its own when it inherited, an entry it had marked inherited not carried over (check 9 of R1 to R4, 31 October 2026) - or one that could not be made again, or given its own entries, is taken (check 8 of R1 to R4); or the setup log does not say, of each folder made again, which way it was (the fold of R1 to R4, 31 October 2026)", "if bInherits then made := MakeFolderIn(AppHandle, ExtractFileName(Item), 0, status) else made := MakeFolderIn(AppHandle, ExtractFileName(Item), pDescriptor, status); if made = -1 then begin FreeWindowsMemory(pDescriptor); g_bFoldersChanged := true; Why := ~; Exit; end; if bInherits then if not WriteOwnEntriesOn(AppHandle, made, Item, ExtractFileName(Item), pDacl, Why) then begin FreeWindowsMemory(pDescriptor); g_bFoldersChanged := true; Why := ~; Exit; end; FreeWindowsMemory(pDescriptor); @@if bInherits then Log('Setup removed ' + Item + ', an empty folder there before this installation, and made it again with what the program folder hands down now and the entries ' + ~) else Log('Setup removed ' + Item + ', an empty folder there before this installation, and made it again with the permissions it had, which inherited nothing: a ' + ~); Result := true; end;$" },
         new[] { "WriteOwnEntriesOn", "a folder made again whose list inherited is given its own entries though it had none to give, or none though it had some (check 9 of R1 to R4, 31 October 2026)", "if (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0 then own := own + 1; index := index + 1; end; end; if own = 0 then begin Result := true; Exit; end;" },
         new[] { "WriteOwnEntriesOn", "an entry the folder had marked inherited - one it kept from where it was made - is carried over onto the folder made again, or an entry of its own is left out, or what it inherits now from the program folder is not kept (check 9 of R1 to R4, 31 October 2026)", "bOwn := (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0; bDeny := (head.TypeFlagsSize and $FF) = HM_ACCESS_DENIED; if ((pass = 0) and bOwn and bDeny) or ((pass = 1) and bOwn and (not bDeny)) or ((pass = 2) and (not bOwn)) then ok := AppendAce(pNew, HM_ACL_REVISION, HM_MAXDWORD, pAce, size);" },
         new[] { "WriteOwnEntriesOn", "the entries of its own are written through a link, or on another folder than the one setup made and holds, or with the list protected, so that what the program folder hands down is gone (check 9 of R1 to R4, 31 October 2026)", "handle := HoldFolder(Item, HM_WRITE_DAC_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then begin FreeWindowsMemory(pNew); Why := ~; Exit; end; if HeldIsLink(handle, bKnown) then Why := ~ else if not MadeWhereNamed(AppHandle, handle, Name, szWhere) then Why := ~ else if not (ReadHeldInformation(handle, openedInformation) and ReadHeldInformation(Made, heldInformation)) then Why := ~ else if (openedInformation.dwVolumeSerialNumber <> heldInformation.dwVolumeSerialNumber) or (openedInformation.nFileIndexHigh <> heldInformation.nFileIndexHigh) or (openedInformation.nFileIndexLow <> heldInformation.nFileIndexLow) then Why := ~ else begin written := WriteHeldSecurity(handle, HM_FILE_OBJECT, HM_UNPROTECTED_DACL_INFORMATION, 0, 0, pNew, 0); if written <> 0 then Why := ~; end; ReleaseFolder(handle); FreeWindowsMemory(pNew); if Why <> '' then Exit;" },
         new[] { "WriteOwnEntriesOn", "the folder made again is taken without its entries of its own read again through the handle setup holds (check 9 of R1 to R4, 31 October 2026)", "readResult := ReadHeldSecurity(Made, HM_FILE_OBJECT, HM_DACL_INFORMATION, pNowOwner, 0, pNowDacl, 0, pNowDescriptor); if readResult <> 0 then begin Why := ~; Exit; end; found := 0;" },
         new[] { "WriteOwnEntriesOn", "a folder made again whose entries of its own are not all there when read again is taken (check 9 of R1 to R4, 31 October 2026)", "FreeWindowsMemory(pNowDescriptor); if found <> own then begin Why := ~; Exit; end; Log(~); Result := true; end;$" },
         new[] { "ItemIsIn", "what the path reaches is taken for what is in the folder setup holds without being read through the two handles", "handle := HoldFolder(Item, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then Exit; szItem := FinalPathOfHeld(handle); ReleaseFolder(handle); szFolder := FinalPathOfHeld(FolderHandle); if (szItem <> '') and (szFolder <> '') then Result := CompareText(szItem, AddBackslash(szFolder) + ExtractFileName(Item)) = 0;" },
         new[] { "NothingTurnedIn", "a link or an empty folder setup did not leave is taken after the files", "if bLink or (bFolder and FolderHoldsNothing(szItem)) then begin if bLink then szWhat := ~ else szWhat := ~; Found := Found + 1; Why := ~; Log(~); Result := false; end" },
         new[] { "NothingTurnedIn", "what is below the first level is not looked at after the files, or a folder there is walked without being the folder of its name in the one above it (check 4 of R1 to R4, 27 October 2026)", "for index := 0 to count - 1 do if not NothingTurnedIn(folders[index], AddBackslash(szFinal) + ExtractFileName(folders[index]), Depth + 1, Found, Why) then Result := false; ReleaseFolder(folderHandle); end;$" },
         new[] { "PreparedFolderIsAsLeft", "a folder setup prepared is taken after the files as a link, reached elsewhere by its path, or with its permissions changed - one setup made with an entry for any account but the three", "Result := false; if HeldIsLink(Handle, bKnown) then Why := ~ else if (Item <> '') and (not ItemIsIn(Item, Handle)) then Why := ~ else if Hardened and (not HeldFolderIsHardened(Handle, AnyAccount, false, szWhy)) then Why := ~ else Result := true;" },
         new[] { "FoldersStillAsPrepared", "the program folder and Bin are not read again after the files, a folder setup made is not held to having no other account's entry, or Bin is walked when it was not found as setup left it, or from its path rather than its handle", "if g_hProgramFolderPrepared <> -1 then if PreparedFolderIsAsLeft(g_szProgramFolderReal, g_hProgramFolderPrepared, szApp + 'Bin', g_bProgramFolderHardened, g_bProgramFolderNew, Why) then szAppFinal := FinalPathOfHeld(g_hProgramFolderPrepared); if g_hBinPrepared <> -1 then if PreparedFolderIsAsLeft(g_szBinReal, g_hBinPrepared, szApp + 'Bin\\hMailServer.INI', true, SetupMadeBin(), Why) then NothingTurnedIn(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, found, Why);" },
         new[] { "FoldersStillAsPrepared", "ControlPanel is not read again after the files, or is walked when it was not found as setup left it", "if ControlPanelIsInstalled() then begin if PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, szApp + 'ControlPanel\\hMailCP.exe', true, SetupMadeControlPanel(), Why) then NothingTurnedIn(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, found, Why); end else if PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, '', true, SetupMadeControlPanel(), Why) then NothingTurnedIn(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, found, Why);" },
         new[] { "FoldersStillAsPrepared", "what is in the folders that hold what is run is not looked at after the files, or is walked when the program folder was not found as setup left it, or as other than the folders of their names in it", "if szAppFinal <> '' then for index := 0 to 4 do begin case index of 0: szFolder := 'Addons'; 1: szFolder := 'Events'; 2: szFolder := 'DBScripts'; 3: szFolder := 'WebAdmin'; 4: szFolder := 'Languages'; end; NothingTurnedIn(AddBackslash(g_szProgramFolderReal) + szFolder, AddBackslash(szAppFinal) + szFolder, 0, found, Why); end; Result := Why = '';" },
         new[] { "CurStepChanged", "the folders are not read again before setup runs a program of the installation, or what is not as left does not fail it, keep the service from starting, keep setup from installing again before a restart and say so for hMailServer.Updater", "g_bFilesInstalled := true; @@if not FoldersStillAsPrepared(szWhy) then begin g_bFoldersNotAsPrepared := true; g_bStepsIncomplete := true; KeepServiceFromStarting(); RecordRestartNeeded(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), ~); Log(FoldersChangedLine()); RaiseException(" },
         new[] { "FoldersWereAsPrepared", "the last page runs the Control Panel from the folders found not as setup left them (check 3 of R1 to R4, 27 October 2026)", "^function FoldersWereAsPrepared(): Boolean; begin Result := not g_bFoldersNotAsPrepared; end;$" },
         new[] { "KeepServiceFromStarting", "the service is not set not to start", "if Exec(ExpandConstant('{sys}\\sc.exe'), 'config hMailServer start= disabled', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then" },
         new[] { "LinkRemedy", "the remedy of a link does not say that a restart takes away what a program opened it for", "/L /inheritance:r /grant:r *S-1-5-18:F *S-1-5-32-544:F *S-1-5-32-545:RX - restarts Windows, since a program that opened the link while another account could ' + 'change it keeps that right until it closes it, and runs setup again" },
         new[] { "HardenHeldFolder", "the earlier installation's folder another account owned is not said, with the restart that takes its handles away", "if bTrusted then SayWriteEntriesKept(Folder, HeldHandle) else begin @@szText := ~; Log('The program folder: ' + szText); g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10; end; Result := true;" },
         new[] { "PrepareProgramFolders", "Bin, and ControlPanel when it is installed, are not made, and recorded, in a program folder setup made, so that a cancel during the copy leaves them, and every folder setup made above them, behind (check 4 of R1 to R4, 27 October 2026)", "if not MakeFolders(g_szProgramFolderReal, hMade, AddBackslash(g_szProgramFolderReal) + 'Bin', 1, hInside, szWhy) then begin Result := ~; Exit; end; g_hBinPrepared := hInside; if ControlPanelIsInstalled() then begin if not MakeFolders(g_szProgramFolderReal, hMade, AddBackslash(g_szProgramFolderReal) + 'ControlPanel', 2, hInside, szWhy) then begin Result := ~; Exit; end; g_hControlPanelPrepared := hInside; end; Exit; end;" },
         new[] { "SayWriteEntriesKept", "an entry kept that lets an account other than the three write is not said", "if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) then" },
         new[] { "NextButtonClick", "the folders are not checked before the service is stopped, or a refusal there stops it all the same", "g_szFolderRefusal := CheckFoldersBeforeTheStop(PreviousInstallationFolder()); if g_szFolderRefusal <> '' then Exit;" },
         new[] { "NextButtonClick", "whether setup stopped a running service is not known", "bWasRunning := IsServiceRunning('hMailServer'); if bWasRunning then g_bServiceStoppedForSetup := true; if (StopHMailServerService() = false) then Result := false;" },
         new[] { "PrepareToInstall", "a refusal after setup stopped the service does not say so, one that found a folder changed does not keep the service from starting and say so for hMailServer.Updater, or one whose remedy ends in a restart lets setup install again before it", "Result := PrepareToInstallRefusal(); if Result <> '' then begin @@if g_bFoldersChanged then begin KeepServiceFromStarting(); Log(FoldersChangedLine()); if g_bServiceStoppedForSetup then Result := Result + ' Setup stopped the hMailServer service~; end else if g_bServiceStoppedForSetup then Result := Result + ' Setup stopped the hMailServer service~; @@if Pos('restarts Windows', Result) > 0 then RecordRestartNeeded(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), ~); Log('Refused: ' + Result); end; end;$" },
         new[] { "PrepareToInstallRefusal", "the program folders are not prepared last, their refusal the answer", "Result := PrepareProgramFolders(PreviousInstallationFolder()); end;$" },
         new[] { "HoldFolderChain", "a walk follows links deeper than eight, or starts from what an earlier walk left (check 3 of R1 to R4, 27 October 2026)", "begin Result := false; Real := ''; Held := ''; HeldHandle := -1; Whole := false; Why := ''; if Depth >= 8 then begin Why := ~; Exit; end; if not SplitAtRoot(Path, szRoot, szRest, Why) then Exit;" },
         new[] { "HoldFolderChain", "the root of a volume is not held, or one that cannot be opened is walked", "parent := HoldForSetup(szRoot); if parent = -1 then begin Why := ~; Exit; end;" },
         new[] { "HoldFolderChain", "the next name is not cut from the path as it is written, so that a name is passed over or taken with the rest", "separator := Pos('\\', szRest); if separator = 0 then begin szName := szRest; szRest := ''; end else begin szName := Copy(szRest, 1, separator - 1); szRest := Copy(szRest, separator + 1, Length(szRest)); end;" },
         new[] { "HoldFolderChain", "a name is not looked at where the walk is, or whether it could be read is not asked", "szNext := AddBackslash(szCurrent) + szName; attributes := FolderAttributes(szNext); lastError := DLLGetLastError(); if attributes = HM_INVALID_ATTRIBUTES then begin" },
         new[] { "HoldFolderChain", "the folders below a missing name are not among those setup makes", "Real := szNext; if szRest <> '' then Real := AddBackslash(szNext) + szRest;" },
         new[] { "HoldFolderChain", "the rest of the path after a link is not walked from where the link leads, so that the walk ends there", "if szRest <> '' then szTarget := AddBackslash(szTarget) + szRest; Result := HoldFolderChain(szTarget, Depth + 1, Real, Held, HeldHandle, Whole, Why); end; bDone := true;" },
         new[] { "LinkTarget", "a link is taken for one setup has read before its data is read", "begin Result := false; Target := ''; Why := '';" },
         new[] { "LinkTarget", "a link whose data cannot be read, or is too short to hold a name, is taken", "if not ReadReparseData(hLink, HM_GET_REPARSE_POINT, 0, 0, buffer, 16384, returned, 0) then begin Why := ~; Exit; end; if returned < 20 then begin Why := ~; Exit; end;" },
         new[] { "LinkTarget", "where a link leads is read from other words of its data - the name shown rather than the substitute name Windows follows", "tagLow := Ord(buffer[1]); tagHigh := Ord(buffer[2]); offset := Ord(buffer[5]); size := Ord(buffer[6]); relative := false;" },
         new[] { "LinkTarget", "a name past the end of the link's data, or an empty one, is taken, or the name is read from elsewhere", "bytes := returned; if ((offset div 2) + (size div 2) + start - 1 > (bytes div 2)) or (size = 0) then begin Why := ~; Exit; end; szName := Copy(buffer, start + (offset div 2), size div 2);" },
         new[] { "LinkTarget", "a relative symbolic link is taken from setup's own folder rather than as Windows takes it (RelativeLinkTarget)", "if relative then begin if not RelativeLinkTarget(Link, szName, Target, Why) then Exit; end else if" },
         new[] { "RelativeLinkTarget", "a relative symbolic link is taken from a root other than its own volume's, or a link setup cannot name is taken (check 8 of R1 to R4, 28 October 2026)", "begin Result := false; Target := ''; if not SplitAtRoot(Link, szRoot, szRest, Why) then Exit;" },
         new[] { "RelativeLinkTarget", "a relative symbolic link whose name starts with a separator is taken from the folder the link is in, not from the root of its volume as Windows takes it, or one without from elsewhere than the folder the link is in (check 8 of R1 to R4)", "if Copy(Name, 1, 1) = '\\' then szPath := Name else szPath := szRest + '\\..\\' + Name; szPath := szPath + '\\'; count := 0;" },
         new[] { "RelativeLinkTarget", "a relative name that goes above the root of its volume is taken, or .. and . are read as folders (check 8 of R1 to R4)", "if szPart = '..' then begin if count = 0 then begin Why := ~; Exit; end; count := count - 1; end else if (szPart <> '') and (szPart <> '.') then begin" },
         new[] { "RelativeLinkTarget", "where a relative link leads is not made a full path from the root of its volume (check 8 of R1 to R4)", "Target := szRoot; for index := 0 to count - 1 do begin if index > 0 then Target := Target + '\\'; Target := Target + parts[index]; end; Result := true; end;$" },
         new[] { "LinkTarget", "what a link leads to is not made a full path as Windows names it, a volume's root kept with its backslash", "else if Copy(szName, 1, 4) = '\\??\\' then begin Target := Copy(szName, 5, Length(szName)); if CompareText(Copy(Target, 1, 7), 'Volume{') = 0 then Target := '\\\\?\\' + Target; end else begin Why := ~; Exit; end; Target := RemoveBackslashUnlessRoot(Target); if Target = '' then begin Why := ~; Exit; end; if Copy(Target, 1, 4) = '\\\\?\\' then if Target[Length(Target)] = '}' then Target := Target + '\\'; Result := true;" },
         new[] { "CheckWhatIsInside", "Bin and ControlPanel are looked at past 32 folders down, a folder whose listing cannot be read is taken, or one that is a link now, or not where it was listed, is walked (check 4 of R1 to R4, 27 October 2026)", "Result := true; count := 0; if Depth > 32 then begin Found := Found + 1; Why := ~; Result := false; Exit; end; folderHandle := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); szFinal := ''; if folderHandle <> -1 then if not HeldIsLink(folderHandle, bKnown) then szFinal := FinalPathOfHeld(folderHandle); if (szFinal = '') or ((Expected <> '') and (CompareText(szFinal, Expected) <> 0)) then begin if folderHandle <> -1 then ReleaseFolder(folderHandle); Found := Found + 1; szText := ~; Why := Why + szText + ' '; Log(~); Result := false; Exit; end; if not FindFirst(AddBackslash(Folder) + '*', rec) then begin ReleaseFolder(folderHandle); Found := Found + 1; Why := Why + 'what is in ' + Folder + ' could not be read (Windows error '" },
         new[] { "CheckWhatIsInside", "what is a folder in Bin and ControlPanel is not read, so that nothing below their first level is looked at and a folder another account owns is only said", "if (rec.Name <> '.') and (rec.Name <> '..') then begin szItem := AddBackslash(Folder) + rec.Name; bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;" },
         new[] { "CheckWhatElseIsInside", "the program folder's own level is not looked at, or one whose listing cannot be read is taken", "Result := true; szAppFinal := FinalPathOfHeld(AppHandle); if not IsVolumeRoot(App) then begin if not FindFirst(AddBackslash(App) + '*', rec) then begin Found := Found + 1;" },
         new[] { "CheckWhatElseIsInside", "an item of the program folder's own level other than Bin and ControlPanel is passed over, or what is a folder there is not read", "if (rec.Name <> '.') and (rec.Name <> '..') and (CompareText(rec.Name, 'Bin') <> 0) and (CompareText(rec.Name, 'ControlPanel') <> 0) then begin szItem := AddBackslash(App) + rec.Name; bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;" },
         new[] { "CheckWhatElseIsInside", "Addons, Events, DBScripts, WebAdmin or Languages, when it is there and no link, is not looked through, or one that is a link is", "for index := 0 to 5 do begin case index of 0: szFolder := 'Addons';" },
         new[] { "NoteFoldersOthersCouldChange", "Addons, Events, DBScripts, WebAdmin, Languages and MySQL are not the folders on the program folder's own level the restart rule asks (check 7 of R1 to R4, 28 October 2026)", "for index := 0 to 5 do begin case index of 0: szFolder := 'Addons'; 1: szFolder := 'Events'; 2: szFolder := 'DBScripts'; 3: szFolder := 'WebAdmin'; 4: szFolder := 'Languages'; 5: szFolder := 'MySQL'; end;" },
         new[] { "CheckWhatElseIsInside", "one of the five folders that is there is not looked through, or one that is a link is (check 3 of R1 to R4, 27 October 2026)", "szFolder := AddBackslash(App) + szFolder; @@if FolderAttributes(szFolder) <> HM_INVALID_ATTRIBUTES then if ((FolderAttributes(szFolder) and FILE_ATTRIBUTE_DIRECTORY) <> 0) and ((FolderAttributes(szFolder) and HM_REPARSE_POINT) = 0) then if not CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why) then Result := false;" },
         new[] { "EveryUserMayReadMadeIn", "the folder setup makes in is not read through its handle, or one whose permissions cannot be read is taken for one every user may read", "Result := false; pDescriptor := 0; pDacl := 0; pOwner := 0; if ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then Exit; if pDacl = 0 then Result := true else if ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then" },
         new[] { "EveryUserMayReadMadeIn", "Users is given Read on a folder made where not every user could read one: an entry that is not an allow entry handed down to folders, does not read, or is another group's, counted", "if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((((head.TypeFlagsSize shr 8) and $FF) and HM_CONTAINER_INHERIT) <> 0) and ((head.Mask and HM_FOLDER_READ_RIGHTS) <> 0) then begin group := 0; while (not Result) and (group < 3) do begin szSid := BroadGroupSid(group, szName); if SidIs(pAce + 8, szSid) then Result := true; group := group + 1; end; end;" },
         new[] { "ProgramFolderMadeAsNamed", "a program folder setup did not make, or made where a link leads, is given to the uninstaller as {app}", "^function ProgramFolderMadeAsNamed(): Boolean; begin Result := false; if g_bProgramFolderNew then Result := CompareText(g_szProgramFolderReal, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0; end;$" },
         new[] { "MadeParentCount", "the folders setup made above the program folder are counted with it, or without it where a link leads", "^function MadeParentCount(): Integer; begin Result := GetArrayLength(g_aFoldersMade); if ProgramFolderMadeAsNamed() then Result := Result - 1; if Result < 0 then Result := 0; end;$" },
         new[] { "MadeParentOffset", "other than the eight folders setup made nearest the program folder are given to the uninstaller", "^function MadeParentOffset(): Integer; begin Result := MadeParentCount() - 8; if Result < 0 then Result := 0; end;$" },
         new[] { "SetupMadeFolder", "a folder setup made above the program folder is not given to the uninstaller, or one it did not make is", "^function SetupMadeFolder(Index: Integer): Boolean; begin Result := g_bProgramFolderNew and (MadeParentOffset() + Index < MadeParentCount()); end;$" },
         new[] { "FolderMadeBySetup", "the uninstaller is given another folder than the one setup made for an entry", "^function FolderMadeBySetup(Param: String): String; var index: Integer; begin index := MadeParentOffset() + StrToIntDef(Param, 0); Result := ''; if (index >= 0) and (index < GetArrayLength(g_aFoldersMade)) then Result := g_aFoldersMade[index]; end;$" },
         new[] { "SetupMadeProgramFolder", "the program folder setup made is not given to the uninstaller, or one it did not make is", "^function SetupMadeProgramFolder(): Boolean; begin Result := ProgramFolderMadeAsNamed(); end;$" },
         new[] { "SetupMadeBin", "Bin is given to the uninstaller whatever made it", "^function SetupMadeBin(): Boolean; begin Result := g_szBinMade <> ''; end;$" },
         new[] { "SetupMadeControlPanel", "ControlPanel is given to the uninstaller whatever made it", "^function SetupMadeControlPanel(): Boolean; begin Result := g_szControlPanelMade <> ''; end;$" },
         new[] { "RemoveFoldersMadeHere", "a folder setup made is not removed after a cancel, removed shallowest first, or remembered after it is gone (check 3 of R1 to R4, 27 October 2026)", "^procedure RemoveFoldersMadeHere(); var index: Integer; begin @@if g_szBinMade <> '' then RemoveMadeFolder(g_szBinMade, g_szBinMadeAt, g_hBinMade); if g_szControlPanelMade <> '' then RemoveMadeFolder(g_szControlPanelMade, g_szControlPanelMadeAt, g_hControlPanelMade); g_szBinMade := ''; g_szBinMadeAt := ''; g_szControlPanelMade := ''; g_szControlPanelMadeAt := ''; for index := GetArrayLength(g_aFoldersMade) - 1 downto 0 do RemoveMadeFolder(g_aFoldersMade[index], g_aFoldersMadeAt[index], g_aFoldersMadeHandle[index]); SetArrayLength(g_aFoldersMade, 0); SetArrayLength(g_aFoldersMadeAt, 0); SetArrayLength(g_aFoldersMadeHandle, 0); g_bProgramFolderNew := false; end;$" },
         new[] { "DataLinkLeadsWhereAllowed", "a data folder's link is taken when it cannot be held, read or walked", "begin Result := false; Why := ''; hLink := HoldForSetup(Link);" },
         new[] { "DataLinkLeadsWhereAllowed", "where a data folder's link leads, owned by another account or with an entry of its own that lets one write it, is taken, or the earlier installation's is installed into without a restart (check 5 of R1 to R4, 27 October 2026)", "if not DataFolderIsAllowed(szReal, hHeld, bOwner, szWho, szRemedy) then begin if bOwner and g_bEarlierInstallation and (not Final) then AddRestartFolder(szReal, hHeld, true, false, Link + ' led to ' + szReal + ', which was ' + Copy(szWho, 4, Length(szWho))) else begin Why := ~; Exit; end; end; Result := true; end;$" },
         new[] { "DataFolderIsAllowed", "a data folder whose owner and permissions cannot be read, or whose owner is another account than the three or the service, is taken (check 5 of R1 to R4, 27 October 2026)", "readResult := ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin Who := ~; Exit; end; if pOwner = 0 then Who := ~ else if (not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID)) then begin Owner := true;" },
         new[] { "DataFolderIsAllowed", "a data folder with an entry of its own that lets another account but the service write it is taken (check 5 of R1 to R4, 27 October 2026)", "else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then begin Who := " },
         new[] { "DataFolderIsAllowed", "a data folder is taken but when its owner and its entries were read and found as they may be", "^function DataFolderIsAllowed(Folder: String; Handle: Integer; var Owner: Boolean; var Who: String; var Remedy: String): Boolean; var pOwner: cardinal; pDacl: cardinal; pDescriptor: cardinal; readResult: cardinal; szText: String; szSid: String; begin Result := false;" },
         new[] { "DatabaseFilesWhy", "a file of the Database another account owns, or whose owner cannot be read, is taken (check 5 of R1 to R4, 27 October 2026)", "if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then begin Result := 'holds ' + szItem" },
         new[] { "DatabaseFilesWhy", "a file of the Database with an entry of its own that lets another account but the service write it is taken", "else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then begin Result := 'holds ' + szItem" },
         new[] { "DatabaseFilesWhy", "a link among the Database's files, or a Database whose files cannot be listed, is taken", "if (rec.Attributes and HM_REPARSE_POINT) <> 0 then begin Result := 'holds ' + szItem + ', which is a link';" },
         new[] { "DatabaseFilesWhy", "a Database whose files cannot be listed is taken", "Result := ''; Remedy := ''; if not FindFirst(AddBackslash(Folder) + '*', rec) then begin Result := " },
         new[] { "HardenDataFolder", "a data folder another account owned is rebuilt as a program folder (the service's entries dropped, Users given Read), not read again, or its owner named after the list is freed", "szOwner := SidNameAt(pOwner); Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, false, true, Why); FreeWindowsMemory(pDescriptor); if not Result then Exit; Result := HeldFolderIsHardened(HeldHandle, true, true, Why); if not Result then begin Why := ~; Exit; end;" },
         new[] { "AddRestartFolder", "a folder is noted for the restart rule twice, or without whether it is a data folder, or whether another account could change its permissions or its owner (check 6 of R1 to R4, 28 October 2026)", "for index := 0 to GetArrayLength(g_aRestartFolders) - 1 do if CompareText(g_aRestartFolders[index], Folder) = 0 then Exit; count := GetArrayLength(g_aRestartFolders) + 1; SetArrayLength(g_aRestartFolders, count); SetArrayLength(g_aRestartHandles, count); SetArrayLength(g_aRestartData, count); SetArrayLength(g_aRestartDistrust, count); g_aRestartFolders[count - 1] := Folder; g_aRestartHandles[count - 1] := Handle; g_aRestartData[count - 1] := Data; g_aRestartDistrust[count - 1] := Distrust;" },
         new[] { "StaleEntryLetsOthers", "an entry a file or a folder kept from where it was made is not found, or one the folder it is in hands down is taken for it (check 5 of R1 to R4, 27 October 2026; a folder, check 7, 28 October 2026)", "if ((aceFlags and HM_INHERITED_ACE) <> 0) and (Folder or ((aceFlags and HM_INHERIT_ONLY) = 0)) and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-4')) and (not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) and (not HandsDownWriteTo(pFolderDescriptor, pFolderDacl, pAce + 8, inherit)) then begin Result := true;" },
         new[] { "StaleEntryLetsOthers", "a folder's entry that applies to it is held to what the folder it is in hands down to files, not to folders, or a file's to what it hands down to folders (check 7 of R1 to R4, 28 October 2026)", "inherit := HM_OBJECT_INHERIT; if Folder then begin if (aceFlags and HM_INHERIT_ONLY) = 0 then inherit := HM_CONTAINER_INHERIT else inherit := HM_OBJECT_INHERIT or HM_CONTAINER_INHERIT; end;" },
         new[] { "HandsDownWriteTo", "a folder whose list could not be read is taken to hand a right down, or an entry not handed down to files, not giving a right to write, or of another account is taken for one", "Result := false; if pFolderDescriptor = 0 then Exit; if pFolderDacl = 0 then begin Result := true; Exit; end; @@if not ReadAclSize(pFolderDacl, info, 12, HM_ACL_SIZE_INFORMATION) then Exit;" },
         new[] { "HandsDownWriteTo", "an entry not handed down to files, not giving a right to write, or of another account is taken for one the folder hands down", "if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((((head.TypeFlagsSize shr 8) and $FF) and Inherit) <> 0) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and SidsAreEqual(pAce + 8, pSid) then Result := true;" },
         new[] { "HandsDownWriteTo", "the inheritance asked for is not the one the caller names (check 7 of R1 to R4, 28 October 2026)", "^function HandsDownWriteTo(pFolderDescriptor: cardinal; pFolderDacl: cardinal; pSid: cardinal; Inherit: cardinal): Boolean;" },
         new[] { "NoteStaleItem", "a stale entry is not kept for the restart rule with what it let", "SetArrayLength(g_aStaleItems, GetArrayLength(g_aStaleItems) + 1); g_aStaleItems[GetArrayLength(g_aStaleItems) - 1] := Item; SetArrayLength(g_aStaleWhat, GetArrayLength(g_aStaleItems)); g_aStaleWhat[GetArrayLength(g_aStaleItems) - 1] := Item + ' let ' + Who" },
         new[] { "StaleItemIn", "what is in Bin, ControlPanel or a folder that holds what is run is taken for the program folder's, or the other way about", "bDirect := CompareText(Folder, g_szProgramFolderReal) = 0; for index := 0 to GetArrayLength(g_aStaleItems) - 1 do begin if bDirect then bIn := CompareText(ExtractFileDir(g_aStaleItems[index]), Folder) = 0 else bIn := CompareText(Copy(g_aStaleItems[index], 1, Length(AddBackslash(Folder))), AddBackslash(Folder)) = 0; if bIn and (Result = '') then Result := g_aStaleWhat[index]; end;" },
         new[] { "TakenEntriesLetOthers", "an entry setup's hardening keeps - a named account's own - is counted for a restart, so that every run of setup is refused, or one it takes away - inherited, or a group of every user's own - is not (check 5 of R1 to R4, 27 October 2026)", "if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-4')) and (((aceFlags and HM_INHERITED_ACE) <> 0) or IsBroadSid(pAce + 8)) then begin Result := true;" },
         new[] { "TakenEntriesLetOthers", "no list, a list that cannot be read, or an entry of a form setup does not read is not counted, or an inherit-only entry is", "if pDacl = 0 then begin Result := true; Who := ~; Exit; end; if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then begin Result := true; Who := ~; Exit; end;" },
         new[] { "TakenEntriesLetOthers", "an inherit-only entry, or one of a form setup does not read, is weighed otherwise", "if (aceFlags and HM_INHERIT_ONLY) = 0 then begin if aceType = HM_ACCESS_ALLOWED then begin" },
         new[] { "CheckWhatIsInside", "a program, a library, a script, a settings file or a folder that an entry it kept from where it was made lets another account change is not noted for the restart rule (check 5 of R1 to R4, 27 October 2026; a folder, check 7, 28 October 2026)", "else if bCode or bFolder then begin if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, bKeyStore, szText) then begin szWho := ~; szRemedy := ~; end else if (not Final) and StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, bFolder, szText) then NoteStaleItem(szItem, szText); end;" },
         new[] { "CheckWhatIsInside", "what a folder hands down to the files in it is not read through its handle, or one that cannot be read is taken to hand everything down", "if ReadHeldSecurity(folderHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pFolderOwner, 0, pFolderDacl, 0, pFolderDescriptor) <> 0 then pFolderDescriptor := 0;" },
         new[] { "PreparedFolderIsLink", "the refusal of a prepared folder made a link does not name it, or does not take it for a folder changed while setup prepared it", "^function PreparedFolderIsLink(Folder: String): String; begin g_bFoldersChanged := true; Result := Folder + ' is a junction, a symbolic link or a mount point now" },
         new[] { "ReleaseHeldFolders", "a folder setup holds is let go before the end, or not at all", "^procedure ReleaseHeldFolders(); var index: Integer; begin for index := GetArrayLength(g_aHeldFolders) - 1 downto 0 do ReleaseFolder(g_aHeldFolders[index]); SetArrayLength(g_aHeldFolders, 0); end;$" },
         new[] { "ControlPanelIsInstalled", "ControlPanel is made, or not, whatever is installed", "^function ControlPanelIsInstalled(): Boolean; begin Result := IsComponentSelected('controlpanel'); end;$" },
         new[] { "CheckProgramFolders", "setup installs again before Windows has restarted, once it has hardened a folder another account could change or refused for what a restart must follow (check 4 of R1 to R4, 27 October 2026)", "if RestartNeededFor(szApp, szWhy) then begin Result := ~; Exit; end; if not HoldFolderChain(szApp, 0," },
         new[] { "CheckProgramFolders", "what another account could change before this installation is not looked at, or looked at before every refusal of the check", "if found > 0 then begin Result := ~; Exit; end; @@NoteFoldersOthersCouldChange(); end;$" },
         new[] { "CheckProgramFolders", "what an earlier pass found another account could change, or found changed, is kept when the folder page is gone back to", "g_bFoldersChanged := false; SetArrayLength(g_aRestartFolders, 0); SetArrayLength(g_aRestartHandles, 0); SetArrayLength(g_aRestartData, 0); SetArrayLength(g_aRestartDistrust, 0); SetArrayLength(g_aStaleItems, 0); SetArrayLength(g_aStaleWhat, 0); g_szRestartWhy := ''; @@g_hProgramFolder := -1; g_hBin := -1; g_hControlPanel := -1;" },
         new[] { "NoteFoldersOthersCouldChange", "the program folder, a Bin or a ControlPanel setup hardens in place is not asked whether another account could change it", "if not g_bProgramFolderThere then Exit; NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, g_szProgramFolderWaits <> ''); if g_bBinThere and (not g_bRemakeBin) then NoteFolderOthersCouldChange(g_szBinReal, g_hBin, true, false); if g_bControlPanelThere and (not g_bRemakeControlPanel) then NoteFolderOthersCouldChange(g_szControlPanelReal, g_hControlPanel, true, false);" },
         new[] { "NoteFoldersOthersCouldChange", "Addons, Events, DBScripts, WebAdmin and Languages are not asked whether another account could change them, or are asked through a link", "handle := HoldFolder(szFolder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle <> -1 then begin if not HeldIsLink(handle, bKnown) then NoteFolderOthersCouldChange(szFolder, handle, false, false); ReleaseFolder(handle); end;" },
         new[] { "NoteFolderOthersCouldChange", "a folder that holds something but empty folders, or the program folder setup leaves unhardened before the files, is not asked who could write it, or a folder whose permissions another account could change is not noted so (check 6 of R1 to R4, 28 October 2026)", "if not OthersCouldChange(Folder, HeldHandle, Unhardened or (not HoldsOnlyEmptyFolders(Folder)), bDistrust, szWhat) then begin szWhat := StaleItemIn(Folder); if szWhat = '' then Exit; end; if Keep then AddRestartFolder(Folder, HeldHandle, false, bDistrust, szWhat) else AddRestartFolder(Folder, -1, false, bDistrust, szWhat);" },
         new[] { "OthersCouldChange", "an owner other than the three, an entry that lets another account change the permissions or the owner, or one that lets it write a folder that holds something, is not found, or a folder whose permissions cannot be read is taken", "Result := true; Distrust := false; What := ''; pOwner := 0; pDacl := 0; pDescriptor := 0; readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin What := ~; Exit; end; if pOwner = 0 then What := ~ else if not IsTrustedSid(pOwner) then What := ~ else if TakenEntriesLetOthers(pDacl, HM_CHANGE_PERMISSIONS_RIGHTS, szWho) then begin What := ~; Distrust := true; end else if Contents and TakenEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, szWho) then What := ~ else Result := false;" },
         new[] { "HoldsOnlyEmptyFolders", "a file, a link or a folder that holds something is taken for an empty folder, or a folder whose listing cannot be read for one that holds only those", "Result := false; if FindFirst(AddBackslash(Folder) + '*', rec) then begin try Result := true; repeat if (rec.Name <> '.') and (rec.Name <> '..') then begin if ((rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) or ((rec.Attributes and HM_REPARSE_POINT) <> 0) then Result := false else if not FolderHoldsNothing(AddBackslash(Folder) + rec.Name) then Result := false; end; until (not Result) or (not FindNext(rec));" },
         new[] { "HardenForRestart", "what another account could change is installed into without a restart, or the restart is not recorded before anything is changed", "if GetArrayLength(g_aRestartFolders) = 0 then Exit; szApp := RemoveBackslashUnlessRoot(ExpandConstant('{app}')); RecordRestartNeeded(szApp, ~); szDone := '';" },
         new[] { "HardenForRestart", "a folder another account could change is not hardened, or one another account owned is taken for a folder other than the earlier installation's", "handle := g_aRestartHandles[index]; if handle = -1 then handle := HoldForSetup(g_aRestartFolders[index]); if handle = -1 then szWhy := ~ else if g_aRestartData[index] then begin if not HardenDataFolder(g_aRestartFolders[index], handle, szWhy) then begin if szWhy = '' then szWhy := ~; end else szWhy := ''; end else if g_aRestartDistrust[index] then begin if not HardenDistrustedFolder(g_aRestartFolders[index], handle, szWhy) then begin if szWhy = '' then szWhy := ~; end else szWhy := ''; end else if not HardenHeldFolder(g_aRestartFolders[index], handle, g_bEarlierInstallation, szWhy) then" },
         new[] { "HardenDistrustedFolder", "a folder whose permissions another account could change keeps an entry of its own that lets another account write - one that account could have written - or is not read through its handle, or one another account owns is taken although it is not the earlier installation's (check 6 of R1 to R4, 28 October 2026)", "readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor); if readResult <> 0 then begin Why := ~; Exit; end; if pOwner = 0 then Why := ~ else if (not IsTrustedSid(pOwner)) and (not g_bEarlierInstallation) then Why := ~ else Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, EveryUserMayRead(pDacl), false, Why); FreeWindowsMemory(pDescriptor); if not Result then Exit; Result := HeldFolderIsHardened(HeldHandle, true, false, Why);" },
         new[] { "HardenForRestart", "the installation goes on once what another account could change is hardened, rather than being refused until Windows has restarted", "szDone := szDone + g_aRestartFolders[index] + ', '; end; Result := 'Setup does not install into ' + szApp + ' until Windows has restarted: " },
         new[] { "CheckFoldersBeforeTheStop", "what another account could change is not hardened and refused before the service is stopped, or is after a refusal of the check", "^function CheckFoldersBeforeTheStop(Earlier: String): String; begin Result := CheckProgramFolders(Earlier); if Result = '' then Result := HardenForRestart(); end;$" },
         new[] { "RestartNeededFor", "what setup recorded until Windows restarts is read elsewhere, or not by the program folder's name", "^function RestartNeededFor(App: String; var What: String): Boolean; begin What := ''; Result := RegQueryStringValue(HKLM64, HM_RESTART_KEY, App, What); end;$" },
         new[] { "RecordRestartNeeded", "what is recorded until Windows restarts is written where a restart does not take it away, or by another name than the program folder's, or not also by where the walks of the program folder, Bin and ControlPanel ended (check 5 of R1 to R4, 27 October 2026)", "written := MakeVolatileKey(HM_HKEY_LOCAL_MACHINE, HM_RESTART_KEY, 0, 0, HM_REG_OPTION_VOLATILE, HM_KEY_SET_VALUE_64, 0, key, disposition); if written = 0 then begin written := WriteKeyText(key, App, 0, HM_REG_SZ, szText, (Length(szText) + 1) * 2); for which := 0 to 2 do begin szFinal := WalkedFinal(which); if (written = 0) and (szFinal <> '') and (not IsVolumeRoot(szFinal)) then written := WriteKeyText(key, szFinal, 0, HM_REG_SZ, szText, (Length(szText) + 1) * 2); end; CloseKey(key); end;" },
         new[] { "RestartRecordedForWalked", "a record by where a walk ended is not looked up, or one of another folder, or of none, is taken for it (check 5 of R1 to R4, 27 October 2026)", "if not RegGetValueNames(HKLM64, HM_RESTART_KEY, names) then Exit; for index := 0 to GetArrayLength(names) - 1 do if (not Result) and (CompareText(Copy(names[index], 1, 4), '\\\\?\\') = 0) then for which := 0 to 2 do begin szWalked := WalkedFinal(which); if (not Result) and IsAtOrBelow(szWalked, names[index]) then begin Result := true;" },
         new[] { "WalkedFinal", "where a walk ended is named by a path rather than by its held handle, or a folder not there yet by the one above it alone (check 5 of R1 to R4, 27 October 2026)", "if (handle = -1) or (szHeld = '') or (szReal = '') then Exit; szFinal := FinalPathOfHeld(handle); if szFinal = '' then Exit; if CompareText(szHeld, szReal) = 0 then Result := szFinal else if CompareText(Copy(szReal, 1, Length(AddBackslash(szHeld))), AddBackslash(szHeld)) = 0 then Result := AddBackslash(szFinal) + Copy(szReal, Length(AddBackslash(szHeld)) + 1, Length(szReal));" },
         new[] { "WalkedFinal", "the walks are named by another walk's handle", "0: begin handle := g_hProgramFolder; szHeld := g_szProgramFolderHeld; szReal := g_szProgramFolderReal; end; 1: begin handle := g_hBin; szHeld := g_szBinHeld; szReal := g_szBinReal; end; else begin handle := g_hControlPanel; szHeld := g_szControlPanelHeld; szReal := g_szControlPanelReal; end;" },
         new[] { "CheckProgramFolders", "the record of a restart is not looked up by where the walks ended, so that another name for the same folder installs before the restart (check 5 of R1 to R4, 27 October 2026)", "@@if RestartRecordedForWalked(szWhy) then begin Result := 'Setup does not install into ' + szApp + ' until Windows has restarted: " },
         new[] { "MakePlaceholder", "the file setup keeps in a folder it hardened in place is shared for deleting, kept after setup, or not made new", "handle := HoldFolder(AddBackslash(Folder) + HM_PLACEHOLDER_NAME, HM_PLACEHOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_CREATE_NEW, HM_PLACEHOLDER_FLAGS, 0);" },
         new[] { "MakePlaceholder", "the file setup keeps is taken where a link put the folder elsewhere", "if not MadeWhereNamed(HeldHandle, handle, HM_PLACEHOLDER_NAME, szWhere) then begin g_bFoldersChanged := true; Why := ~; Exit; end;" },
         new[] { "ReleasePlaceholders", "the files setup keeps are not let go, and so stay", "^procedure ReleasePlaceholders(); var index: Integer; begin for index := GetArrayLength(g_aPlaceholders) - 1 downto 0 do ReleaseFolder(g_aPlaceholders[index]); SetArrayLength(g_aPlaceholders, 0); end;$" },
         new[] { "CurStepChanged", "the files setup keeps go before the folders are read again after the files, or a service an earlier run set not to start stays so", "RaiseException(~); end; ReleasePlaceholders(); RestoreServiceStart();" },
         new[] { "DeinitializeSetup", "the files setup keeps are not let go when setup ends before the files, or the folders setup made are removed after a successful installation too", "^procedure DeinitializeSetup(); begin ReleasePlaceholders(); if not g_bFilesInstalled then RemoveFoldersMadeHere(); ReleaseHeldFolders(); end;$" },
         new[] { "IsServiceInstalled", "whether the service is installed is not asked of the service manager, so that a service found not as prepared is not kept from starting", "^function IsServiceInstalled(Name: String) : Boolean; var manager: SC_HANDLE; service: SC_HANDLE; begin service := OpenNamedService(Name, SERVICE_QUERY_CONFIG, manager); Result := (service <> 0); CloseServiceAndManager(service, manager); end;$" },
         // IsServiceInstalled asks through the two routines below, which the service routines
         // written from Microsoft's documentation share (master's 4c57c52f5f, met when R1 to R4
         // moved onto master on 28 October 2026): the three hold what the one statement held.
         new[] { "OpenNamedService", "the service is not opened by its name through the service manager with the access asked, so that IsServiceInstalled does not answer whether it is installed", "^function OpenNamedService(Name: String; Access: cardinal; var Manager: SC_HANDLE) : SC_HANDLE; begin Result := 0; Manager := OpenServiceManager(); if (Manager = 0) then Exit; Result := OpenServiceW(Manager, Name, Access); if (Result = 0) then begin CloseServiceHandle(Manager); Manager := 0; end; end;$" },
         new[] { "CloseServiceAndManager", "the service or the connection to the service manager IsServiceInstalled opened is not let go", "^procedure CloseServiceAndManager(Service: SC_HANDLE; Manager: SC_HANDLE); begin if (Service <> 0) then CloseServiceHandle(Service); if (Manager <> 0) then CloseServiceHandle(Manager); end;$" },
         // The rest of that chain, and what the Ready page's stop rests on (check 9 of R1 to
         // R4, 31 October 2026); their functions, record and constants are among
         // ProgramFolderDeclarations.
         new[] { "OpenServiceManager", "the service manager is not asked, or a connection is taken where Windows refused one, so that IsServiceInstalled says no service is there and none found not as prepared is kept from starting", "^function OpenServiceManager() : SC_HANDLE; var lastError: Integer; who: String; begin Result := OpenSCManagerW(0, 0, SC_MANAGER_CONNECT); if (Result = 0) then begin lastError := DLLGetLastError(); if IsUninstaller() then who := 'The uninstaller' else who := 'Setup'; Log(~); SuppressibleMsgBox(~); end; end;$" },
         new[] { "GetServiceState", "the state the service is in is not the one Windows reports, so that the Ready page does not stop a running service, or does not say it stopped it", "^function GetServiceState(Name: String) : cardinal; var manager: SC_HANDLE; service: SC_HANDLE; status : SERVICE_STATUS_PROCESS; needed : cardinal; begin Result := 0; service := OpenNamedService(Name, SERVICE_QUERY_STATUS, manager); if (service = 0) then Exit; if QueryServiceStatusProcess(service, SERVICE_STATUS_PROCESS_INFO, status, SERVICE_STATUS_PROCESS_SIZE, needed) then Result := status.dwCurrentState; CloseServiceAndManager(service, manager); end;$" },
         new[] { "IsServiceRunning", "a running service is not taken for running, so that the Ready page neither stops it nor says, in a refusal, that it stopped it", "^function IsServiceRunning(Name: String) : Boolean; begin Result := (GetServiceState(Name) = SERVICE_RUNNING); end;$" },
         new[] { "IsServiceStopped", "a service that is not stopped is taken for stopped, so that setup does not ask it to stop before it prepares the folders and copies the files", "^function IsServiceStopped(Name: String) : Boolean; begin Result := (GetServiceState(Name) = SERVICE_STOPPED); end;$" },
         new[] { "RemoveHeldFolder", "an empty folder is removed through a link, or by a handle not read as that folder", "handle := HoldFolder(Folder, HM_REMOVE_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); if handle = -1 then begin Why := ~; Exit; end; szFinal := FinalPathOfHeld(handle); if HeldIsLink(handle, bKnown) then Why := ~ else if (szFinal = '') or (ParentFinal = '') or (CompareText(szFinal, AddBackslash(ParentFinal) + ExtractFileName(Folder)) <> 0) then Why := ~ else begin info := HM_DISPOSITION_DELETE_POSIX; Result := SetHeldInformation(handle, HM_FILE_DISPOSITION_INFO_EX, info, 4);" },
         new[] { "NothingTurnedIn", "a folder is walked after the files without being opened as itself, as no link, and as the folder its path names", "folderHandle := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0); szFinal := ''; if folderHandle <> -1 then if not HeldIsLink(folderHandle, bKnown) then szFinal := FinalPathOfHeld(folderHandle); if (szFinal = '') or (CompareText(szFinal, Expected) <> 0) then begin" },
         new[] { "GetCustomSetupExitCode", "folders found not as setup left them after the files end in 10, which hMailServer.Updater rolls back, rather than 12", "if g_bFoldersNotAsPrepared then Result := 12 else if g_bStepsIncomplete and g_bDatabaseChanged then Result := 11 else if g_bStepsIncomplete then Result := 10 else Result := 0;" },
         new[] { "FoldersChangedLine", "the line hMailServer.Updater reads to put nothing back over changed folders is changed", "^function FoldersChangedLine(): String; begin Result := 'Setup found the folders it prepares changed (work item 78): the hMailServer service is set not to start, and no earlier version is to be put back over them.'; end;$" },
         new[] { "KeepServiceFromStarting", "a service that is not installed is configured, or the mark that lets a later run set it to start again is not left", "if not IsServiceInstalled('hMailServer') then Exit; if Exec(ExpandConstant('{sys}\\sc.exe'), 'config hMailServer start= disabled', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then begin RegWriteDWordValue(HKLM, 'SOFTWARE\\hMailServer', 'SetupKeptServiceFromStarting', 1);" },
         new[] { "RestoreServiceStart", "a service setup did not set not to start is set to start, or one it did is left so for good", "if not RegQueryDWordValue(HKLM, 'SOFTWARE\\hMailServer', 'SetupKeptServiceFromStarting', mark) then Exit; if not IsServiceInstalled('hMailServer') then Exit; if Exec(ExpandConstant('{sys}\\sc.exe'), 'config hMailServer start= auto', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then begin RegDeleteValue(HKLM, 'SOFTWARE\\hMailServer', 'SetupKeptServiceFromStarting');" },
         new[] { "DataLinkTargetIsGrantable", "a data folder's link to a drive's root is taken", "if IsVolumeRoot(szFinal) then begin SetLength(szNames, 2048); returned := 0; if not VolumePathNames(szFinal, szNames, 2048, returned) then begin Why := ~; Exit; end;" },
         new[] { "DataLinkTargetIsGrantable", "a data folder's link into, or over, the Windows folder, Program Files, the profiles or ProgramData\\Microsoft is taken", "0: szName := ExpandConstant('{win}'); 1: szName := ExpandConstant('{commonpf64}'); 2: szName := ExpandConstant('{commonpf32}'); 3: szName := ProfilesFolder(); 4: szName := AddBackslash(ExpandConstant('{commonappdata}')) + 'Microsoft'; end; szOther := ''; if szName <> '' then szOther := FinalPathOf(szName); if IsAtOrBelow(szFinal, szOther) or IsAtOrBelow(szOther, szFinal) then begin Why := ~; Exit; end;" },
         new[] { "DataLinkTargetIsGrantable", "a data folder's link to what holds ProgramData, or to what is, holds or is in the program folder outside its data folders, is taken", "szOther := FinalPathOf(ExpandConstant('{commonappdata}')); if IsAtOrBelow(szOther, szFinal) then begin Why := ~; Exit; end; szApp := FinalPathOfHeld(g_hProgramFolder); if IsAtOrBelow(szApp, szFinal) then begin Why := ~; Exit; end; if IsAtOrBelow(szFinal, szApp) then if not (IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Data') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Logs') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Temp') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Database')) then" },
         new[] { "DatabaseTargetIsTheAdministrators", "the folder a Database link leads to is taken whoever owns it or may change it", "if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then Why := ~ else if AclLetsEveryUserWrite(pDacl) then Why := ~;" },
         new[] { "DatabaseTargetIsTheAdministrators", "a file in the folder a Database link leads to is taken whoever owns it or may change it, or when it is a link", "if (rec.Attributes and HM_REPARSE_POINT) <> 0 then Why := ~ else begin handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);" },
         new[] { "AclLetsEveryUserWrite", "an entry that lets a group of every user write is missed, or one handed down to what is made in it alone counted", "if ((((head.TypeFlagsSize shr 8) and $FF) and HM_INHERIT_ONLY) = 0) and LetsEveryUserWrite(pAce) then Result := true; end else Result := true;" },
         new[] { "IsAtOrBelow", "a folder is taken for one inside another by a part of a name", "^function IsAtOrBelow(Path: String; Folder: String): Boolean; begin Result := false; if (Path = '') or (Folder = '') then Exit; Result := (CompareText(AddBackslash(Path), AddBackslash(Folder)) = 0) or (CompareText(Copy(Path, 1, Length(AddBackslash(Folder))), AddBackslash(Folder)) = 0); end;$" },
      };

      /// <summary>A ProgramFolderStatements statement as a regular expression.</summary>
      private static string StatementPattern(string statement)
      {
         var pattern = new StringBuilder();
         if (statement.StartsWith("^", StringComparison.Ordinal))
         {
            pattern.Append(@"\A");
            statement = statement.Substring(1);
         }
         bool end = statement.EndsWith("$", StringComparison.Ordinal);
         if (end)
            statement = statement.Substring(0, statement.Length - 1);
         for (int index = 0; index < statement.Length; index++)
         {
            if (string.CompareOrdinal(statement, index, "@@", 0, 2) == 0)
            {
               pattern.Append(@"(?://[^\n]*\n\s*)*");
               index++;
            }
            else if (statement[index] == '~')
               pattern.Append(@"[^;]*");
            else if (statement[index] == ' ')
               pattern.Append(@"\s*");
            else
               pattern.Append(Regex.Escape(statement[index].ToString()));
         }
         if (end)
            pattern.Append(@"\s*\z");
         return pattern.ToString();
      }

      [Test]
      [Description("Work item 78, R1 to R4: every decision of the program folder's preparation is the statement it is held to - as build/check-installer-scripts.py's PROGRAM_FOLDER_STATEMENTS holds it - so that a one-line change of one fails: the drive's root and the walk, each folder's hardening and its refusal, the rule of a link, a name that cannot be read, a link to a share, a folder made in the meantime, a list rebuilt, what is in Bin and ControlPanel, the volumes and the service. The review of 26 October 2026 changed nineteen such lines one at a time, and the tests that looked for words caught six.")]
      public void EachDecisionOfTheProgramFoldersIsTheStatementItIsHeldTo()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         var failed = new List<string>();
         foreach (string[] statement in ProgramFolderStatements)
         {
            if (!Regex.IsMatch(CodeBody(extension, statement[0]), StatementPattern(statement[2])))
               failed.Add(statement[0] + ": " + statement[1]);
         }
         Assert.IsEmpty(failed, "The statement work item 78 holds each to is not there: " + string.Join("; ", failed) + ".");

         // The checker holds the same, statement for statement.
         string checker = ReadRepositoryFile(@"build\check-installer-scripts.py");
         foreach (string[] statement in ProgramFolderStatements)
            StringAssert.Contains("('" + statement[0] + "', ", checker, "build/check-installer-scripts.py does not hold " + statement[0] + " to a statement.");
         Assert.AreEqual(ProgramFolderStatements.Length, Regex.Matches(CheckerTable(checker, "PROGRAM_FOLDER_STATEMENTS"), @"^    \('\w+', '", RegexOptions.Multiline).Count,
            "build/check-installer-scripts.py's PROGRAM_FOLDER_STATEMENTS and this test's ProgramFolderStatements hold different numbers of statements.");

         // A folder is made with its descriptor, set once (the review of 26 October 2026:
         // one set to nothing after it passed every check).
         Assert.AreEqual(1, Regex.Matches(extension, @"attributes\.SecurityDescriptor\s*:=").Count, "The descriptor a folder is made with is set other than once, as setup made it.");

         // Whether setup stopped a running service is set once, to true, and never
         // cleared: a later click on the Ready page finds the service it stopped stopped.
         CollectionAssert.AreEqual(new[] { "true" }, Regex.Matches(extension, @"g_bServiceStoppedForSetup\s*:=\s*(\w+)").Cast<Match>().Select(match => match.Groups[1].Value).ToArray(),
            "Whether setup stopped a running service is set other than once, to true, so a later pass may clear it and a refusal would not say the service was stopped.");

         // PrepareToInstallRefusal, which the rest of setup shares, leaves before the program
         // folders are prepared only with a refusal (check 3 of R1 to R4, 27 October 2026).
         string refusal = CodeToken.Replace(CodeBody(extension, "PrepareToInstallRefusal"),
                                            match => match.Value.StartsWith("'", StringComparison.Ordinal) ? "'" + new string('x', match.Length - 2) + "'" : new string(' ', match.Length));
         foreach (Match exit in Regex.Matches(refusal, @"\bExit\s*;"))
            Assert.IsTrue(Regex.IsMatch(refusal.Substring(0, exit.Index), @"\bResult\s*:=\s*'x+'[^;]*;\s*$"),
               "PrepareToInstallRefusal leaves before the program folders are prepared without a refusal, so that setup would install into folders it has not prepared.");
      }

      /// <summary>
      ///    Work item 78's R1 to R4 whole, as build/check-installer-scripts.py holds them
      ///    (PROGRAM_FOLDER_CONSTANTS, PROGRAM_FOLDER_DECLARATIONS, PROGRAM_FOLDER_ENTRY_POINTS,
      ///    PROGRAM_FOLDER_SHARED and PROGRAM_FOLDER_CODE), entry for entry: every HM_ constant
      ///    (its name, its value as the script writes it, what Windows calls it); the
      ///    Windows functions and records R1 to R4 use, each as it is declared (a statement
      ///    as ProgramFolderStatements' are); the routines R1 to R4 start from; those they
      ///    reach that the rest of setup shares, and why; and every other routine they
      ///    reach with the digest of its code (CodeDigest).
      /// </summary>
      private static readonly string[][] ProgramFolderConstants =
      {
         new[] { "HM_FILE_OBJECT", "1", "SE_FILE_OBJECT" },
         new[] { "HM_DACL_INFORMATION", "4", "DACL_SECURITY_INFORMATION" },
         new[] { "HM_ACL_SIZE_INFORMATION", "2", "AclSizeInformation" },
         new[] { "HM_ACCESS_ALLOWED", "0", "ACCESS_ALLOWED_ACE_TYPE" },
         new[] { "HM_OBJECT_INHERIT", "$01", "OBJECT_INHERIT_ACE" },
         new[] { "HM_CONTAINER_INHERIT", "$02", "CONTAINER_INHERIT_ACE" },
         new[] { "HM_NO_PROPAGATE_INHERIT", "$04", "NO_PROPAGATE_INHERIT_ACE" },
         new[] { "HM_INHERIT_ONLY", "$08", "INHERIT_ONLY_ACE" },
         new[] { "HM_FOLDER_WRITE_RIGHTS", "$500D0156", "FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL" },
         new[] { "HM_FOLDER_READ_RIGHTS", "$90000001", "FILE_LIST_DIRECTORY | GENERIC_READ | GENERIC_ALL" },
         new[] { "HM_HOLD_FOLDER_ACCESS", "$200A1", "FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL" },
         new[] { "HM_READ_CONTROL", "$20000", "READ_CONTROL" },
         new[] { "HM_SHARE_ALL_BUT_DELETE", "$3", "FILE_SHARE_READ | FILE_SHARE_WRITE" },
         new[] { "HM_SHARE_EVERYTHING", "$7", "FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE" },
         new[] { "HM_OPEN_EXISTING", "3", "OPEN_EXISTING" },
         new[] { "HM_HOLD_FOLDER_FLAGS", "$02200000", "FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT" },
         new[] { "HM_REPARSE_POINT", "$400", "FILE_ATTRIBUTE_REPARSE_POINT" },
         new[] { "HM_INVALID_ATTRIBUTES", "$FFFFFFFF", "INVALID_FILE_ATTRIBUTES" },
         new[] { "HM_OWNER_INFORMATION", "1", "OWNER_SECURITY_INFORMATION" },
         new[] { "HM_OWNER_AND_DACL_INFORMATION", "5", "OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION" },
         new[] { "HM_PROTECTED_DACL_INFORMATION", "$80000004", "PROTECTED_DACL_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION" },
         new[] { "HM_REMOVE_FROM_FOLDER_RIGHTS", "$100C0040", "FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL" },
         new[] { "HM_REPLACE_RIGHTS", "$100D0040", "FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL" },
         new[] { "HM_GET_REPARSE_POINT", "$000900A8", "FSCTL_GET_REPARSE_POINT" },
         new[] { "HM_DRIVE_REMOTE", "4", "DRIVE_REMOTE" },
         new[] { "HM_TRUSTED_INSTALLER_SID", "'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'", "NT SERVICE\\TrustedInstaller, AdminUpdateChecks.cpp's TrustedInstallerSid" },
         new[] { "HM_SERVICE_SID", "'S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809'", "NT SERVICE\\hMailServer, as ServiceAccountTester holds it" },
         new[] { "HM_STATUS_NAME_COLLISION", "$C0000035", "STATUS_OBJECT_NAME_COLLISION" },
         new[] { "HM_STATUS_NO_MEMORY", "$C0000017", "STATUS_NO_MEMORY" },
         new[] { "HM_MAKE_FOLDER_ACCESS", "$1200A1", "FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE" },
         new[] { "HM_FILE_CREATE", "2", "FILE_CREATE" },
         new[] { "HM_MAKE_FOLDER_OPTIONS", "$00200021", "FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT" },
         new[] { "HM_OBJ_CASE_INSENSITIVE", "$40", "OBJ_CASE_INSENSITIVE" },
         new[] { "HM_OBJECT_ATTRIBUTES_SIZE", "24", "sizeof(OBJECT_ATTRIBUTES) in a 32-bit process" },
         new[] { "HM_ACL_REVISION", "2", "ACL_REVISION" },
         new[] { "HM_LPTR", "$40", "LPTR" },
         new[] { "HM_MAXDWORD", "$FFFFFFFF", "MAXDWORD" },
         new[] { "HM_ACCESS_DENIED", "1", "ACCESS_DENIED_ACE_TYPE" },
         new[] { "HM_INHERITED_ACE", "$10", "INHERITED_ACE" },
         new[] { "HM_INHERITED_FLAG_BIT", "$1000", "INHERITED_ACE where it sits in ACE_START's TypeFlagsSize" },
         new[] { "HM_FILE_ALL_ACCESS", "$1F01FF", "FILE_ALL_ACCESS" },
         new[] { "HM_RIGHTS_READ_EXECUTE", "$1200A9", "FILE_GENERIC_READ | FILE_EXECUTE, ServiceAccount::RightsReadExecute" },
         new[] { "HM_DACL_PROTECTED", "$1000", "SE_DACL_PROTECTED" },
         new[] { "HM_PERSISTENT_ACLS", "$8", "FILE_PERSISTENT_ACLS" },
         new[] { "HM_VOLUME_NAME_GUID", "1", "VOLUME_NAME_GUID" },
         new[] { "HM_CHANGE_PERMISSIONS_RIGHTS", "$100C0000", "WRITE_DAC | WRITE_OWNER | GENERIC_ALL" },
         new[] { "HM_HKEY_LOCAL_MACHINE", "$80000002", "HKEY_LOCAL_MACHINE" },
         new[] { "HM_REG_OPTION_VOLATILE", "1", "REG_OPTION_VOLATILE" },
         new[] { "HM_KEY_SET_VALUE_64", "$0102", "KEY_SET_VALUE | KEY_WOW64_64KEY" },
         new[] { "HM_REG_SZ", "1", "REG_SZ" },
         new[] { "HM_RESTART_KEY", "'SOFTWARE\\hMailServerSetupRestart'", "the volatile key that keeps setup from installing again before a restart" },
         new[] { "HM_PLACEHOLDER_ACCESS", "$80010000", "GENERIC_READ | DELETE" },
         new[] { "HM_CREATE_NEW", "1", "CREATE_NEW" },
         new[] { "HM_PLACEHOLDER_FLAGS", "$04000102", "FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN" },
         new[] { "HM_PLACEHOLDER_NAME", "'hMailServer-setup-in-progress.tmp'", "the file setup keeps in a folder it hardened in place" },
         new[] { "HM_REMOVE_FOLDER_ACCESS", "$00010080", "DELETE | FILE_READ_ATTRIBUTES" },
         new[] { "HM_FILE_DISPOSITION_INFO", "4", "FileDispositionInfo" },
         new[] { "HM_FILE_DISPOSITION_INFO_EX", "21", "FileDispositionInfoEx" },
         new[] { "HM_DISPOSITION_DELETE_POSIX", "3", "FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS" },
         new[] { "HM_OPEN_FOLLOWING_FLAGS", "$02000000", "FILE_FLAG_BACKUP_SEMANTICS" },
         new[] { "HM_WRITE_DAC_ACCESS", "$00060080", "READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES" },
         new[] { "HM_UNPROTECTED_DACL_INFORMATION", "$20000004", "UNPROTECTED_DACL_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION" },
      };

      private static readonly string[] ProgramFolderDeclarations =
      {
         "ACL_SIZE_INFO = record AceCount : cardinal; AclBytesInUse : cardinal; AclBytesFree : cardinal; end;",
         "ACE_START = record TypeFlagsSize : cardinal; Mask : cardinal; end;",
         "HM_OBJECT_ATTRIBUTES = record nLength : cardinal; RootDirectory : Integer; ObjectName : cardinal; Attributes : cardinal; SecurityDescriptor : cardinal; SecurityQualityOfService : cardinal; end;",
         "HM_UNICODE_STRING = record Lengths : cardinal; Buffer : cardinal; end;",
         "HM_IO_STATUS_BLOCK = record Status : cardinal; Information : cardinal; end;",
         "HM_FILE_INFORMATION = record dwFileAttributes : cardinal; ftCreationTimeLow : cardinal; ftCreationTimeHigh : cardinal; ftLastAccessTimeLow : cardinal; ftLastAccessTimeHigh : cardinal; ftLastWriteTimeLow : cardinal; ftLastWriteTimeHigh : cardinal; dwVolumeSerialNumber : cardinal; nFileSizeHigh : cardinal; nFileSizeLow : cardinal; nNumberOfLinks : cardinal; nFileIndexHigh : cardinal; nFileIndexLow : cardinal; end;",
         "function ReadFolderPermissions(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; ppsidOwner: cardinal; ppsidGroup: cardinal; var ppDacl: cardinal; ppSacl: cardinal; var ppSecurityDescriptor: cardinal): cardinal; external 'GetNamedSecurityInfoW@advapi32.dll stdcall';",
         "function ReadAclSize(pAcl: cardinal; var AclInformation: ACL_SIZE_INFO; nAclInformationLength: cardinal; dwAclInformationClass: cardinal): Boolean; external 'GetAclInformation@advapi32.dll stdcall';",
         "function ReadAclEntry(pAcl: cardinal; dwAceIndex: cardinal; var pAce: cardinal): Boolean; external 'GetAce@advapi32.dll stdcall';",
         "procedure CopyAceStart(var Destination: ACE_START; Source: cardinal; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
         "function SidOfText(StringSid: String; var Sid: cardinal): Boolean; external 'ConvertStringSidToSidW@advapi32.dll stdcall';",
         "function SidsAreEqual(Sid1: cardinal; Sid2: cardinal): Boolean; external 'EqualSid@advapi32.dll stdcall';",
         "function FreeWindowsMemory(hMem: cardinal): cardinal; external 'LocalFree@kernel32.dll stdcall';",
         "function HoldFolder(lpFileName: String; dwDesiredAccess: cardinal; dwShareMode: cardinal; lpSecurityAttributes: cardinal; dwCreationDisposition: cardinal; dwFlagsAndAttributes: cardinal; hTemplateFile: cardinal): Integer; external 'CreateFileW@kernel32.dll stdcall';",
         "function ReleaseFolder(hObject: Integer): Boolean; external 'CloseHandle@kernel32.dll stdcall';",
         "function FolderAttributes(lpFileName: String): cardinal; external 'GetFileAttributesW@kernel32.dll stdcall';",
         "function SecurityDescriptorOfText(StringSecurityDescriptor: String; StringSDRevision: cardinal; var SecurityDescriptor: cardinal; SecurityDescriptorSize: cardinal): Boolean; external 'ConvertStringSecurityDescriptorToSecurityDescriptorW@advapi32.dll stdcall';",
         "function WriteFolderSecurity(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; psidOwner: cardinal; psidGroup: cardinal; pDacl: cardinal; pSacl: cardinal): cardinal; external 'SetNamedSecurityInfoW@advapi32.dll stdcall';",
         "function ReadHeldSecurity(Handle: Integer; ObjectType: cardinal; SecurityInfo: cardinal; var ppsidOwner: cardinal; ppsidGroup: cardinal; var ppDacl: cardinal; ppSacl: cardinal; var ppSecurityDescriptor: cardinal): cardinal; external 'GetSecurityInfo@advapi32.dll stdcall';",
         "function ControlOfDescriptor(pSecurityDescriptor: cardinal; var pControl: Word; var lpdwRevision: cardinal): Boolean; external 'GetSecurityDescriptorControl@advapi32.dll stdcall';",
         "function SidToText(Sid: cardinal; var StringSid: cardinal): Boolean; external 'ConvertSidToStringSidW@advapi32.dll stdcall';",
         "function WideTextLength(lpString: cardinal): Integer; external 'lstrlenW@kernel32.dll stdcall';",
         "procedure CopyWideText(Destination: String; Source: cardinal; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
         "procedure CopyBytes(Destination: cardinal; Source: cardinal; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
         "procedure WriteAceStart(Destination: cardinal; var Source: ACE_START; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
         "function AllocateWindowsMemory(uFlags: cardinal; uBytes: cardinal): cardinal; external 'LocalAlloc@kernel32.dll stdcall';",
         "function StartAcl(pAcl: cardinal; nAclLength: cardinal; dwAclRevision: cardinal): Boolean; external 'InitializeAcl@advapi32.dll stdcall';",
         "function AppendAce(pAcl: cardinal; dwAceRevision: cardinal; dwStartingAceIndex: cardinal; pAceList: cardinal; nAceListLength: cardinal): Boolean; external 'AddAce@advapi32.dll stdcall';",
         "function AppendAllowAce(pAcl: cardinal; dwAceRevision: cardinal; AceFlags: cardinal; AccessMask: cardinal; pSid: cardinal): Boolean; external 'AddAccessAllowedAceEx@advapi32.dll stdcall';",
         "function ReadReparseData(hDevice: Integer; dwIoControlCode: cardinal; lpInBuffer: cardinal; nInBufferSize: cardinal; lpOutBuffer: String; nOutBufferSize: cardinal; var lpBytesReturned: cardinal; lpOverlapped: cardinal): Boolean; external 'DeviceIoControl@kernel32.dll stdcall';",
         "function MakeFileRelative(var FileHandle: Integer; DesiredAccess: cardinal; var ObjectAttributes: HM_OBJECT_ATTRIBUTES; var IoStatusBlock: HM_IO_STATUS_BLOCK; AllocationSize: cardinal; FileAttributes: cardinal; ShareAccess: cardinal; CreateDisposition: cardinal; CreateOptions: cardinal; EaBuffer: cardinal; EaLength: cardinal): cardinal; external 'NtCreateFile@ntdll.dll stdcall';",
         "procedure CopyTextTo(Destination: cardinal; Source: String; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
         "procedure WriteNameRecord(Destination: cardinal; var Source: HM_UNICODE_STRING; Length: cardinal); external 'RtlMoveMemory@kernel32.dll stdcall';",
         "function VolumeOfHeldFolder(hFile: Integer; lpVolumeNameBuffer: cardinal; nVolumeNameSize: cardinal; var lpVolumeSerialNumber: cardinal; var lpMaximumComponentLength: cardinal; var lpFileSystemFlags: cardinal; lpFileSystemNameBuffer: cardinal; nFileSystemNameSize: cardinal): Boolean; external 'GetVolumeInformationByHandleW@kernel32.dll stdcall';",
         "function KindOfDrive(lpRootPathName: String): cardinal; external 'GetDriveTypeW@kernel32.dll stdcall';",
         "function LookupNameOfSid(lpSystemName: cardinal; Sid: cardinal; Name: String; var cchName: cardinal; ReferencedDomainName: String; var cchReferencedDomainName: cardinal; var peUse: cardinal): Boolean; external 'LookupAccountSidW@advapi32.dll stdcall';",
         "function ReadHeldInformation(hFile: Integer; var lpFileInformation: HM_FILE_INFORMATION): Boolean; external 'GetFileInformationByHandle@kernel32.dll stdcall';",
         "function FinalPathOfHandle(hFile: Integer; lpszFilePath: String; cchFilePath: cardinal; dwFlags: cardinal): cardinal; external 'GetFinalPathNameByHandleW@kernel32.dll stdcall';",
         "function MakeVolatileKey(hKey: cardinal; lpSubKey: String; Reserved: cardinal; lpClass: cardinal; dwOptions: cardinal; samDesired: cardinal; lpSecurityAttributes: cardinal; var phkResult: cardinal; var lpdwDisposition: cardinal): Integer; external 'RegCreateKeyExW@advapi32.dll stdcall';",
         "function WriteKeyText(hKey: cardinal; lpValueName: String; Reserved: cardinal; dwType: cardinal; lpData: String; cbData: cardinal): Integer; external 'RegSetValueExW@advapi32.dll stdcall';",
         "function CloseKey(hKey: cardinal): Integer; external 'RegCloseKey@advapi32.dll stdcall';",
         "function SetHeldInformation(hFile: Integer; FileInformationClass: cardinal; var lpFileInformation: cardinal; dwBufferSize: cardinal): Boolean; external 'SetFileInformationByHandle@kernel32.dll stdcall';",
         "function VolumePathNames(lpszVolumeName: String; lpszVolumePathNames: String; cchBufferLength: cardinal; var lpcchReturnLength: cardinal): Boolean; external 'GetVolumePathNamesForVolumeNameW@kernel32.dll stdcall';",
         "function WriteHeldSecurity(Handle: Integer; ObjectType: cardinal; SecurityInfo: cardinal; psidOwner: cardinal; psidGroup: cardinal; pDacl: cardinal; pSacl: cardinal): cardinal; external 'SetSecurityInfo@advapi32.dll stdcall';",
         // The Service Control Manager's, which IsServiceInstalled asks through and
         // GetServiceState uses (check 9 of R1 to R4, 31 October 2026); the constants are
         // Windows' names, not HM_, and so held here, each once.
         "function OpenSCManagerW(lpMachineName: cardinal; lpDatabaseName: cardinal; dwDesiredAccess: cardinal): SC_HANDLE; external 'OpenSCManagerW@advapi32.dll stdcall';",
         "function OpenServiceW(hSCManager: SC_HANDLE; lpServiceName: String; dwDesiredAccess: cardinal): SC_HANDLE; external 'OpenServiceW@advapi32.dll stdcall';",
         "function CloseServiceHandle(hSCObject: SC_HANDLE): Boolean; external 'CloseServiceHandle@advapi32.dll stdcall';",
         "function QueryServiceStatusProcess(hService: HANDLE; InfoLevel: cardinal; var Buffer: SERVICE_STATUS_PROCESS; cbBufSize: cardinal; var pcbBytesNeeded: cardinal): boolean; external 'QueryServiceStatusEx@advapi32.dll stdcall';",
         "SERVICE_STATUS_PROCESS = record dwServiceType : cardinal; dwCurrentState : cardinal; dwControlsAccepted : cardinal; dwWin32ExitCode : cardinal; dwServiceSpecificExitCode : cardinal; dwCheckPoint : cardinal; dwWaitHint : cardinal; dwProcessId : cardinal; dwServiceFlags : cardinal; end;",
         "SC_MANAGER_CONNECT = $0001;",
         "SERVICE_QUERY_CONFIG = $0001;",
         "SERVICE_QUERY_STATUS = $0004;",
         "SERVICE_STOPPED = $00000001;",
         "SERVICE_RUNNING = $00000004;",
         "SERVICE_STATUS_PROCESS_INFO = 0;",
         "SERVICE_STATUS_PROCESS_SIZE = 36;",
      };

      private static readonly string[] ProgramFolderEntryPoints =
      {
         "CheckProgramFolders",
         "PrepareProgramFolders",
         "FoldersStillAsPrepared",
         "RemoveFoldersMadeHere",
         "KeepServiceFromStarting",
         "FoldersWereAsPrepared",
         "FolderMadeBySetup",
         "SetupMadeFolder",
         "SetupMadeProgramFolder",
         "SetupMadeBin",
         "SetupMadeControlPanel",
         "PreviousInstallationFolder",
         "ReleasePlaceholders",
         "RestoreServiceStart",
         "FoldersChangedLine",
      };

      private static readonly string[][] ProgramFolderShared =
      {
         new[] { "CheckProgramFolderPermissions", "the folder page's note of 6.3.7 on a program folder another account may write, which decides nothing" },
         new[] { "IsServiceInstalled", "whether the service is installed, which the rest of setup asks too" },
      };

      private static readonly string[][] ProgramFolderCode =
      {
         new[] { "AccountNameOfSid", "d7f15a73b2a52847" },
         new[] { "AclLetsEveryUserWrite", "7fd37319681c8b53" },
         new[] { "AclLetsOthers", "6b00430bd84cb1ab" },
         new[] { "AddAsOwnEntry", "f0c0620c1378f54d" },
         new[] { "AddRestartFolder", "49938388fed2521d" },
         new[] { "BroadGroupSid", "8b255776e8013335" },
         new[] { "CheckFoldersBeforeTheStop", "a2e38e9db48a13e5" },
         new[] { "CheckProgramFolders", "7e0c3340809c7fec" },
         new[] { "CheckWhatElseIsInside", "3aeeb1fb6a3227c4" },
         new[] { "CheckWhatIsInside", "d53f9875ea5c5d75" },
         new[] { "ControlPanelIsInstalled", "3ab47178b35c2ef7" },
         new[] { "DatabaseFilesWhy", "385a1b53ff151af2" },
         new[] { "DatabaseTargetIsTheAdministrators", "5aafe7086f1634b2" },
         new[] { "DataFolderIsAllowed", "b1088c905684623f" },
         new[] { "DataFolderThatInherits", "66c2cd8de8d2f08b" },
         new[] { "DataLinkLeadsWhereAllowed", "0b4799996608f6cd" },
         new[] { "DataLinkTargetIsGrantable", "49b5e0fd9d0bf941" },
         new[] { "EveryUserMayRead", "311a6e77de030208" },
         new[] { "EveryUserMayReadMadeIn", "b9e6f38e684da9c5" },
         new[] { "FinalPathOf", "9de48f21c9043b14" },
         new[] { "FinalPathOfHeld", "2c5642212af4ef31" },
         new[] { "FolderAboveIsAdministratorsAlone", "932c4bfbf248554a" },
         new[] { "FolderHoldsNothing", "7335ca15ee314789" },
         new[] { "FolderMadeBySetup", "3ed9e1d5025dba38" },
         new[] { "FoldersChangedLine", "1c1a75a70ac8df34" },
         new[] { "FoldersStillAsPrepared", "4a9a3f6b11bec5ed" },
         new[] { "FoldersWereAsPrepared", "7b86c82e306d4585" },
         new[] { "HandsDownWriteTo", "43f307a120cfd36a" },
         new[] { "HardenDataFolder", "fb9e890fb43679ae" },
         new[] { "HardenDistrustedFolder", "3463104dcc3133d8" },
         new[] { "HardenForRestart", "13ee441b644792ec" },
         new[] { "HardenHeldFolder", "8e38a444233c3c88" },
         new[] { "HeldFolderIsHardened", "3e667a4213e1312e" },
         new[] { "HeldIsLink", "dc9e0afb79a54681" },
         new[] { "HeldOwnerIsTrusted", "6c22996dbd870f3e" },
         new[] { "HoldFolderChain", "3488e3586d2e893d" },
         new[] { "HoldForSetup", "950dbbf8ac5695d2" },
         new[] { "HoldsOnlyEmptyFolders", "b5d86f4cfb2c4d10" },
         new[] { "InheritsNothing", "ba9a5f635445f6e7" },
         new[] { "IsAtOrAboveChecked", "65db442d814071a2" },
         new[] { "IsAtOrBelow", "ddb47adbbef155e5" },
         new[] { "IsBroadSid", "38c2f0d7a83df514" },
         new[] { "IsDataFolderName", "ce6c435a26838422" },
         new[] { "IsProgramFile", "219188cbc96d13c5" },
         new[] { "IsRunFolderName", "618eab586bb72741" },
         new[] { "IsScriptFile", "da5441e710f716d7" },
         new[] { "IsSettingsFile", "a4a7b5a10ddbcbd9" },
         new[] { "IsTrustedSid", "b1e4972ae9631bbd" },
         new[] { "IsVolumeRoot", "1d8d1a5e9fee3a54" },
         new[] { "ItemIsIn", "3f4a4540977f6580" },
         new[] { "KeepServiceFromStarting", "cdbbbdc7f29148f1" },
         new[] { "LetGoOfHeld", "83a63b6a0c82fba3" },
         new[] { "LetsEveryUserWrite", "f8c242f2bf668c73" },
         new[] { "LinkIsAdministratorsAlone", "c2c9823696671e05" },
         new[] { "LinkRemedy", "421eb698557d16a3" },
         new[] { "LinkTarget", "78cb462e54e3b55c" },
         new[] { "MadeParentCount", "98517a4e3b3fbbfe" },
         new[] { "MadeParentOffset", "3298776306b79064" },
         new[] { "MadeWhereNamed", "98d8f43dacc9751a" },
         new[] { "MakeFolderIn", "34f935fa5de4000f" },
         new[] { "MakeFolders", "5d5589bec77ff4ad" },
         new[] { "MakePlaceholder", "0b825c9ab1d96d0e" },
         new[] { "NoteFolderOthersCouldChange", "f6ed4de1f7a769ea" },
         new[] { "NoteFoldersOthersCouldChange", "0649cbf4a983effb" },
         new[] { "NoteStaleItem", "8faf68da80c3e0e9" },
         new[] { "NothingTurnedIn", "f4dba71743a003e6" },
         new[] { "OthersCouldChange", "59cd57cd4f2192fb" },
         new[] { "OwnEntriesLetOthers", "432884347c2a0dfa" },
         new[] { "OwnerIsAllowed", "01366659a7062c32" },
         new[] { "PreparedFolderIsAsLeft", "b6fb831f8e861123" },
         new[] { "PreparedFolderIsLink", "ba199f0e24f474c1" },
         new[] { "PrepareProgramFolders", "737bfa20ab72e9a9" },
         new[] { "PreviousInstallationFolder", "e5316bef61ccd228" },
         new[] { "ProfilesFolder", "510631cc6e496304" },
         new[] { "ProgramFolderMadeAsNamed", "4126d0c1e6214bee" },
         new[] { "ProgramFolderWaitsForTheFiles", "a89df9530e50e3d1" },
         new[] { "RebuildFolderOfUntrustedOwner", "dedaf61436fb3d03" },
         new[] { "RecordRestartNeeded", "235f797c1cefa764" },
         new[] { "RelativeLinkTarget", "e71b5f87fff8e5ca" },
         new[] { "ReleaseHeldFolders", "f8c80f82d876588b" },
         new[] { "ReleasePlaceholders", "676d729ff0128f12" },
         new[] { "RemakeEmptyFolder", "f84455156da5337c" },
         new[] { "RemoveAndMakeAgain", "b7bb404494fe2f8c" },
         new[] { "RemoveFoldersMadeHere", "5d8963b8738f5484" },
         new[] { "RemoveHeldFolder", "149850770478f53c" },
         new[] { "RemoveMadeFolder", "b58144954352dd5e" },
         new[] { "RestartNeededFor", "4ef98799db2b5bf9" },
         new[] { "RestartRecordedForWalked", "070d8af3cd79ed43" },
         new[] { "RestoreServiceStart", "1ea970d4b76972e3" },
         new[] { "SayWriteEntriesKept", "d8f9b5d24a6298b6" },
         new[] { "SetupMadeBin", "1dbd327f1d6d18a6" },
         new[] { "SetupMadeControlPanel", "1ac337150384c97d" },
         new[] { "SetupMadeFolder", "96f12e0fabeb6aee" },
         new[] { "SetupMadeProgramFolder", "7f70cdaa4f086ceb" },
         new[] { "SidInName", "2a5e78371e8a3930" },
         new[] { "SidIs", "389072b7476600fc" },
         new[] { "SidNameAt", "c8f922d592759b87" },
         new[] { "SplitAtRoot", "a9a5495b516a0a1a" },
         new[] { "StaleEntryLetsOthers", "ce957ad5f77e1596" },
         new[] { "StaleItemIn", "31bbbc5f8143d368" },
         new[] { "TakenEntriesLetOthers", "eab4b339385484f1" },
         new[] { "TextOfSidAt", "f3c4acf66f69003e" },
         new[] { "VolumeKeepsPermissions", "7816988ef8c6a23f" },
         new[] { "WalkedFinal", "471561a0e7f5164a" },
         new[] { "WalksAgree", "b3961e81811d817c" },
         new[] { "WriteOwnEntriesOn", "1b7260df1968d584" },
      };

      // A [Code] comment (// to the end of the line, { }, (* *)), a string, or a run of
      // white space; an external function's declaration; a routine's header; a record's. A
      // brace that opens {# is no comment but the Inno Setup preprocessor's inline
      // directive, which stays in a routine's code (check 6 of R1 to R4, 28 October 2026).
      private static readonly Regex CodeToken = new Regex(@"//[^\n]*|\{(?!#)[^}]*\}|\(\*[\s\S]*?\*\)|'(?:[^'\n]|'')*'|\s+");
      private static readonly Regex ExternalDeclaration = new Regex(@"^(?:function|procedure)\s+(\w+)\s*(?:\([^()]*\))?\s*(?::\s*\w+\s*)?;\s*external\s+'[^']*'\s*;",
                                                                   RegexOptions.Multiline | RegexOptions.IgnoreCase);
      private static readonly Regex RoutineHeader = new Regex(@"^(?:function|procedure)\s+(\w+)\s*[(:;]", RegexOptions.Multiline | RegexOptions.IgnoreCase);
      private static readonly Regex RecordHeader = new Regex(@"^\s*(\w+)\s*=\s*record\b", RegexOptions.Multiline | RegexOptions.IgnoreCase);

      /// <summary>
      ///    A routine's code as the digest reads it: its comments dropped, each run of white
      ///    space, a comment's place among it, one space, and every string as it is written
      ///    (build/check-installer-scripts.py's routine_code).
      /// </summary>
      private static string RoutineCode(string body)
      {
         var code = new StringBuilder();
         int at = 0;
         foreach (Match token in CodeToken.Matches(body))
         {
            code.Append(body, at, token.Index - at);
            if (token.Value.StartsWith("'", StringComparison.Ordinal))
               code.Append(token.Value);
            else if (code.Length > 0 && code[code.Length - 1] != ' ')
               code.Append(' ');
            at = token.Index + token.Length;
         }
         code.Append(body, at, body.Length - at);
         return code.ToString().Trim();
      }

      /// <summary>The first 16 hex digits of the SHA-256 of a routine's code in UTF-8 (code_digest).</summary>
      private static string CodeDigest(string body)
      {
         using (var sha = System.Security.Cryptography.SHA256.Create())
            return string.Concat(sha.ComputeHash(Encoding.UTF8.GetBytes(RoutineCode(body))).Take(8).Select(part => part.ToString("x2")));
      }

      /// <summary>
      ///    Each routine of the extension, its external declarations left out, by its name in
      ///    lower case: its name, and its text from its header to the "end;" that closes it
      ///    (routine_bodies, CodeBody's text).
      /// </summary>
      private static Dictionary<string, string[]> RoutineBodies(string extension)
      {
         var externals = new HashSet<string>(ExternalDeclaration.Matches(extension).Cast<Match>().Select(match => match.Groups[1].Value.ToLowerInvariant()));
         var closing = new Regex(@"^end;", RegexOptions.Multiline);
         var bodies = new Dictionary<string, string[]>();
         foreach (Match header in RoutineHeader.Matches(extension))
         {
            string key = header.Groups[1].Value.ToLowerInvariant();
            if (externals.Contains(key) || bodies.ContainsKey(key))
               continue;
            Match end = closing.Match(extension, header.Index + header.Length);
            if (end.Success)
               bodies[key] = new[] { header.Groups[1].Value, extension.Substring(header.Index, end.Index + end.Length - header.Index) };
         }
         return bodies;
      }

      /// <summary>
      ///    The routines R1 to R4's entry points reach (ProgramFolderEntryPoints, through
      ///    every routine but ProgramFolderShared's), in the order found (program_folder_reach);
      ///    Used gets the external functions and records the reached routines but the shared
      ///    use, Defined how many times each routine name is defined.
      /// </summary>
      private static List<string> ProgramFolderReach(string extension, HashSet<string> used, Dictionary<string, int> defined)
      {
         var externals = new Dictionary<string, string>();
         foreach (Match match in ExternalDeclaration.Matches(extension))
            externals[match.Groups[1].Value.ToLowerInvariant()] = match.Groups[1].Value;
         foreach (Match match in RoutineHeader.Matches(extension))
         {
            string key = match.Groups[1].Value.ToLowerInvariant();
            if (!externals.ContainsKey(key))
               defined[key] = defined.TryGetValue(key, out int count) ? count + 1 : 1;
         }
         var bodies = RoutineBodies(extension);
         var records = new Dictionary<string, string>();
         foreach (Match match in RecordHeader.Matches(extension))
            records[match.Groups[1].Value.ToLowerInvariant()] = match.Groups[1].Value;
         var shared = new HashSet<string>(ProgramFolderShared.Select(entry => entry[0].ToLowerInvariant()));
         var reached = new List<string>();
         var todo = new Queue<string>(ProgramFolderEntryPoints.Select(name => name.ToLowerInvariant()));
         while (todo.Count > 0)
         {
            string name = todo.Dequeue();
            if (reached.Any(one => one.ToLowerInvariant() == name))
               continue;
            reached.Add(bodies.ContainsKey(name) ? bodies[name][0] : name);
            if (!bodies.ContainsKey(name) || shared.Contains(name))
               continue;
            string code = CodeToken.Replace(bodies[name][1], match => match.Value.StartsWith("'", StringComparison.Ordinal) ? "''" : " ");
            foreach (string word in new SortedSet<string>(Regex.Matches(code, @"\w+").Cast<Match>().Select(match => match.Value.ToLowerInvariant()), StringComparer.Ordinal))
            {
               if (word == name)
                  continue;
               if (externals.ContainsKey(word))
                  used.Add(externals[word]);
               else if (defined.ContainsKey(word))
                  todo.Enqueue(word);
               else if (records.ContainsKey(word))
                  used.Add(records[word]);
            }
         }
         return reached;
      }

      /// <summary>One table of build/check-installer-scripts.py: from "NAME = (" to the ")" that closes it.</summary>
      private static string CheckerTable(string checker, string name)
      {
         string text = checker.Replace("\r\n", "\n");
         int start = text.IndexOf("\n" + name + " = (\n", StringComparison.Ordinal);
         Assert.GreaterOrEqual(start, 0, "build/check-installer-scripts.py has no " + name + ".");
         int end = text.IndexOf("\n)\n", start + 1, StringComparison.Ordinal);
         Assert.Greater(end, start, "build/check-installer-scripts.py's " + name + " has no end.");
         return text.Substring(start, end - start + 1);
      }

      [Test]
      [Description("Work item 78, R1 to R4 whole: every HM_ constant of the installer's [Code] is held to its value and defined once, HM_TRUSTED_INSTALLER_SID to the live update's TrustedInstallerSid; every Windows function and record R1 to R4 use to its declaration; and every routine R1 to R4 reach, but those the rest of setup shares, to a digest of its code - comments dropped, white space one space, every string as it is written - so that no change of one passes until it has been reviewed and its digest written here and in build/check-installer-scripts.py, which holds the same, entry for entry. Check 3 of R1 to R4 (27 October 2026) changed twenty constants and helpers the decisions rely on by one line each - HM_REPARSE_POINT to $800, so that no link would be seen, among them - and the checker passed nineteen, these tests eleven; a sweep of every if, while, Result and Exit of those routines found 433 changes the statements alone let through.")]
      public void EveryConstantDeclarationAndRoutineOfTheProgramFoldersIsHeldWhole()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         var failed = new List<string>();

         // The constants.
         var defined = Regex.Matches(extension, @"^[ \t]*(HM_\w+)[ \t]*=[ \t]*([^;\n]*);", RegexOptions.Multiline).Cast<Match>()
                            .Select(match => new[] { match.Groups[1].Value, match.Groups[2].Value.Trim() }).ToList();
         var held = ProgramFolderConstants.ToDictionary(constant => constant[0], constant => constant[1]);
         foreach (string[] constant in ProgramFolderConstants)
         {
            var values = defined.Where(one => one[0] == constant[0]).Select(one => one[1]).ToList();
            if (values.Count != 1)
               failed.Add(constant[0] + " (" + constant[2] + ") is defined " + values.Count + " times, not once");
            else if (values[0] != constant[1])
               failed.Add(constant[0] + " (" + constant[2] + ") is " + values[0] + ", not " + constant[1]);
         }
         foreach (string[] one in defined.Where(one => !held.ContainsKey(one[0])))
            failed.Add(one[0] + " = " + one[1] + " is a constant this test does not hold");
         Match trusted = Regex.Match(ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\AdminUpdateChecks.cpp"), @"TrustedInstallerSid\s*=\s*L""(S-1-5-80-[0-9-]+)""");
         Assert.IsTrue(trusted.Success, "AdminUpdateChecks.cpp's TrustedInstallerSid is not there.");
         Assert.AreEqual("'" + trusted.Groups[1].Value + "'", held["HM_TRUSTED_INSTALLER_SID"], "HM_TRUSTED_INSTALLER_SID is not the SID the live update trusts.");
         // The groups of every user are ServiceAccount.cpp's, every one, Domain Users and
         // Domain Guests with them (check 5 of R1 to R4, 27 October 2026: eight were left
         // out; check 6, 28 October 2026: four more, and Domain Guests).
         string serviceAccount = ReadRepositoryFile(@"hmailserver\source\Server\Common\Util\ServiceAccount.cpp");
         Match broad = Regex.Match(serviceAccount, @"const wchar_t \*const BroadSids_\[\] = \{(.*?)\};", RegexOptions.Singleline);
         Assert.IsTrue(broad.Success, "ServiceAccount.cpp's BroadSids_ is not there.");
         CollectionAssert.AreEquivalent(Regex.Matches(broad.Groups[1].Value, @"L""(S-[0-9-]+)""").Cast<Match>().Select(match => match.Groups[1].Value).ToArray(),
            Regex.Matches(CodeBody(extension, "BroadGroupSid"), @"Result := '(S-[0-9-]+)';").Cast<Match>().Select(match => match.Groups[1].Value).ToArray(),
            "BroadGroupSid does not count the groups of every user ServiceAccount.cpp's BroadSids_ counts.");
         Match serverRids = Regex.Match(serviceAccount, @"return sidText\.Left\(9\)\.CompareNoCase\(_T\(""S-1-5-21-""\)\) == 0 && \((.*)\);");
         Assert.IsTrue(serverRids.Success, "ServiceAccount.cpp's IsBroad_ does not count a domain's groups as the test reads them.");
         string[] rids = Regex.Matches(serverRids.Groups[1].Value, @"sidText\.Right\(4\) == _T\(""(-\d{3})""\)").Cast<Match>().Select(match => match.Groups[1].Value).OrderBy(rid => rid).ToArray();
         CollectionAssert.AreEqual(new[] { "-513", "-514" }, rids, "ServiceAccount.cpp's IsBroad_ does not count a domain's Domain Users and Domain Guests, and only those.");
         Match installerRids = Regex.Match(CodeBody(extension, "IsBroadSid"),
            @"Result := \(Length\(szSid\) > 13\) and \(CompareText\(Copy\(szSid, 1, 9\), 'S-1-5-21-'\) = 0\) and\s*\((.*?)\);", RegexOptions.Singleline);
         Assert.IsTrue(installerRids.Success, "IsBroadSid does not count a domain's groups as the test reads them.");
         CollectionAssert.AreEqual(rids, Regex.Matches(installerRids.Groups[1].Value, @"\(Copy\(szSid, Length\(szSid\) - 3, 4\) = '(-\d{3})'\)").Cast<Match>()
            .Select(match => match.Groups[1].Value).OrderBy(rid => rid).ToArray(), "IsBroadSid does not count the domain groups ServiceAccount.cpp's IsBroad_ counts.");

         // The declarations.
         foreach (string declaration in ProgramFolderDeclarations)
         {
            int count = Regex.Matches(extension, StatementPattern(declaration)).Count;
            if (count != 1)
               failed.Add("the declaration \"" + declaration.Substring(0, Math.Min(90, declaration.Length)) + "\" is there " + count + " times, not once");
         }
         var declared = new HashSet<string>(ProgramFolderDeclarations.Select(declaration => Regex.Match(declaration, @"(?:function|procedure)?\s*(\w+)").Groups[1].Value.ToLowerInvariant()));

         // The routines.
         var used = new HashSet<string>();
         var counts = new Dictionary<string, int>();
         List<string> reached = ProgramFolderReach(extension, used, counts);
         foreach (string name in used.Where(name => !declared.Contains(name.ToLowerInvariant())))
            failed.Add("R1 to R4 use " + name + ", whose declaration this test does not hold");
         var digests = ProgramFolderCode.ToDictionary(entry => entry[0].ToLowerInvariant(), entry => entry[1]);
         var shared = new HashSet<string>(ProgramFolderShared.Select(entry => entry[0].ToLowerInvariant()));
         var bodies = RoutineBodies(extension);
         foreach (string name in reached.Where(name => !shared.Contains(name.ToLowerInvariant())))
         {
            string key = name.ToLowerInvariant();
            if (!bodies.ContainsKey(key))
               failed.Add(name + ", which R1 to R4 reach, is not there");
            else if (!digests.ContainsKey(key))
               failed.Add(name + ", which R1 to R4 reach, is neither held whole (its digest is " + CodeDigest(bodies[key][1]) + ") nor said to be shared");
            else if (counts[key] != 1)
               failed.Add(name + " is defined " + counts[key] + " times");
            else if (CodeDigest(bodies[key][1]) != digests[key])
               failed.Add(name + " is not the code the reviews of work item 78 held (its digest is " + CodeDigest(bodies[key][1]) + ", held " + digests[key] + ")");
         }
         foreach (string name in digests.Keys.Concat(shared).Where(name => !reached.Any(one => one.ToLowerInvariant() == name)))
            failed.Add(name + " is held as a routine of R1 to R4, which no longer reach it");
         Assert.IsEmpty(failed, "Work item 78's R1 to R4 are not as they are held whole: " + string.Join("; ", failed) + ".");

         // The checker holds the same, entry for entry.
         string checker = ReadRepositoryFile(@"build\check-installer-scripts.py");
         var checkerConstants = Regex.Matches(CheckerTable(checker, "PROGRAM_FOLDER_CONSTANTS"), @"^    \('(HM_\w+)', (?:'([^']*)'|""([^""]*)"")", RegexOptions.Multiline).Cast<Match>()
                                     .ToDictionary(match => match.Groups[1].Value, match => (match.Groups[2].Success ? match.Groups[2].Value : match.Groups[3].Value).Replace(@"\\", @"\"));
         CollectionAssert.AreEquivalent(held, checkerConstants, "build/check-installer-scripts.py's PROGRAM_FOLDER_CONSTANTS and this test's ProgramFolderConstants differ.");
         Assert.AreEqual(ProgramFolderDeclarations.Length, Regex.Matches(CheckerTable(checker, "PROGRAM_FOLDER_DECLARATIONS"), @""",$", RegexOptions.Multiline).Count,
            "build/check-installer-scripts.py's PROGRAM_FOLDER_DECLARATIONS and this test's ProgramFolderDeclarations hold different numbers of declarations.");
         Match entry = Regex.Match(checker, @"PROGRAM_FOLDER_ENTRY_POINTS = \(([^)]*)\)");
         Assert.IsTrue(entry.Success, "build/check-installer-scripts.py has no PROGRAM_FOLDER_ENTRY_POINTS.");
         CollectionAssert.AreEquivalent(ProgramFolderEntryPoints, Regex.Matches(entry.Groups[1].Value, @"'(\w+)'").Cast<Match>().Select(match => match.Groups[1].Value).ToArray(),
            "build/check-installer-scripts.py's PROGRAM_FOLDER_ENTRY_POINTS and this test's ProgramFolderEntryPoints differ.");
         CollectionAssert.AreEquivalent(ProgramFolderShared.Select(one => one[0]).ToArray(),
            Regex.Matches(CheckerTable(checker, "PROGRAM_FOLDER_SHARED"), @"^    \('(\w+)', ", RegexOptions.Multiline).Cast<Match>().Select(match => match.Groups[1].Value).ToArray(),
            "build/check-installer-scripts.py's PROGRAM_FOLDER_SHARED and this test's ProgramFolderShared differ.");
         CollectionAssert.AreEquivalent(ProgramFolderCode.ToDictionary(one => one[0], one => one[1]),
            Regex.Matches(CheckerTable(checker, "PROGRAM_FOLDER_CODE"), @"^    \('(\w+)', '([0-9a-f]{16})'\),", RegexOptions.Multiline).Cast<Match>().ToDictionary(match => match.Groups[1].Value, match => match.Groups[2].Value),
            "build/check-installer-scripts.py's PROGRAM_FOLDER_CODE and this test's ProgramFolderCode differ.");
      }

      [Test]
      [Description("Work item 78, the volumes setup does not install into: a program folder on a share of another computer or a network drive, whose permissions that computer sets, or a program folder, Bin or ControlPanel - each walk's own, since an administrators' link may lead to another volume - on a volume that keeps no permissions (FAT, exFAT), where every account may change every program, or one whose volume cannot be told: refused before anything changes, with what to do. In 6.3.7 and earlier setup installed there and could not harden anything; the first R1 to R4 of 6.3.8 asked the program folder's volume alone, and took one it could not tell.")]
      public void AProgramFolderNoPermissionsCanProtectIsRefused()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string check = CodeBody(extension, "CheckProgramFolders");
         foreach (string chain in new[] { "g_hProgramFolder, g_szProgramFolderHeld", "g_hBin, g_szBinHeld", "g_hControlPanel, g_szControlPanelHeld" })
            Assert.IsTrue(Regex.IsMatch(check, @"if\s+not\s+VolumeKeepsPermissions\(" + Regex.Escape(chain) + @",\s*szWhy\)\s+then\s+begin\s+Result\s*:=\s*'Setup does not install into '"),
               "The volume of " + chain + " is not asked.");
         string volume = CodeBody(extension, "VolumeKeepsPermissions");
         Assert.IsTrue(Regex.IsMatch(volume, @"\(flags\s+and\s+HM_PERSISTENT_ACLS\)\s*=\s*0") && Regex.IsMatch(extension, @"HM_PERSISTENT_ACLS\s*=\s*\$8;"),
            "Whether a volume keeps permissions is not FILE_PERSISTENT_ACLS.");
         Assert.AreEqual(1, Regex.Matches(volume, @"Result\s*:=\s*true").Count, "A volume that cannot be told is taken for one that keeps permissions.");
         Assert.IsTrue(Regex.IsMatch(extension, @"external\s+'GetVolumeInformationByHandleW@kernel32\.dll stdcall';"), "The volume is not read through the held folder.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "SplitAtRoot"), @"else\s+if\s+Copy\(Path,\s*1,\s*2\)\s*=\s*'\\\\'\s+then\s+Why\s*:="), "A program folder on a share is not refused.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_DRIVE_REMOTE\s*=\s*4;"), "DRIVE_REMOTE is not 4.");
      }

      [Test]
      [Description("Work item 78, R1 (check 9 of R1 to R4, 31 October 2026): a program folder path that Windows reads as another - typed on the directory page with a name ending in a dot or a space, which Windows drops, or with . or .. - is refused before the walk, with the path Windows reads, and MakeFolders makes no name ending in a dot or a space. Inno Setup takes such a path as typed and installs by the path Windows reads, while setup made the folder the name named relative to the folder above, which no path reaches, so that Inno Setup's own folder inherited what the folder above hands down for the whole copy.")]
      public void AProgramFolderPathWindowsReadsAsAnotherIsRefused()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string check = CodeBody(extension, "CheckProgramFolders");
         Match refusal = Regex.Match(check, @"szWhy\s*:=\s*RemoveBackslashUnlessRoot\(ExpandFileName\(szApp\)\);\s*if\s+CompareText\(szWhy,\s*szApp\)\s*<>\s*0\s+then\s+begin\s+Result\s*:=\s*'Setup does not install into '\s*\+\s*szApp\s*\+\s*': Windows reads that path as '");
         Assert.IsTrue(refusal.Success, "A program folder path Windows reads as another is not refused, with the path Windows reads.");
         int walk = check.IndexOf("HoldFolderChain(", StringComparison.Ordinal);
         Assert.IsTrue(walk > refusal.Index, "The program folder path Windows reads as another is refused only after the walk has held what it names.");
         string make = CodeBody(extension, "MakeFolders");
         int guard = make.IndexOf("(Copy(szName, Length(szName), 1) = '.') or (Copy(szName, Length(szName), 1) = ' ')", StringComparison.Ordinal);
         int made = make.IndexOf("MakeFolderIn(", StringComparison.Ordinal);
         Assert.IsTrue(guard >= 0 && made > guard, "MakeFolders makes a name ending in a dot or a space, which no path reaches.");
      }

      /// <summary>
      ///    The entry lines of every [section] of an Inno script, comment lines dropped, with
      ///    their line numbers (section_entries).
      /// </summary>
      private static List<KeyValuePair<int, string>> SectionEntries(string script, string section)
      {
         var entries = new List<KeyValuePair<int, string>>();
         bool inside = false;
         string[] lines = ScriptText(script).Split('\n');
         for (int index = 0; index < lines.Length; index++)
         {
            string line = lines[index].Trim();
            Match header = Regex.Match(line, @"^\[([^\]]+)\]");
            if (header.Success)
            {
               inside = string.Equals(header.Groups[1].Value.Trim(), section, StringComparison.OrdinalIgnoreCase);
               continue;
            }
            if (inside && line.Length > 0 && !line.StartsWith(";") && !line.StartsWith("#"))
               entries.Add(new KeyValuePair<int, string>(index + 1, line));
         }
         return entries;
      }

      /// <summary>One parameter of an Inno entry line, or null (parameter).</summary>
      private static string EntryParameter(string entry, string name)
      {
         Match match = Regex.Match(entry, @"(?:^|;)\s*" + Regex.Escape(name) + @"\s*:\s*(""([^""]*)""|[^;]*)", RegexOptions.IgnoreCase);
         if (!match.Success)
            return null;
         return match.Groups[2].Success ? match.Groups[2].Value : match.Groups[1].Value.Trim();
      }

      [Test]
      [Description("Work item 78 (check 4 of R1 to R4, 27 October 2026): what Inno Setup does outside the [Code] between PrepareToInstall and the look after the files keeps what setup prepared - no [Dirs] or [Files] entry under {app} gives permissions, which Inno Setup applies as it makes the folder or copies the file; no [Run] entry without postinstall, which Inno Setup runs before ssPostInstall, runs anything but the .NET runtime from {tmp}, with nothing of {app} or of a {code:} in it; no entry names a procedure to run before or after it (BeforeInstall, AfterInstall), and the functions entries name for Inno Setup to call during the install are those held; ReleaseHeldFolders, ReleasePlaceholders and LetGoOfHeld are called where they are now alone, CurInstallProgressChanged is not defined and CurPageChanged runs no program (check 5 of R1 to R4, 27 October 2026: nine one-line changes of that kind passed), and neither are NeedRestart and RegisterPreviousData, which Inno Setup calls after the files and as it saves the uninstall information, nor do ShouldSkipPage, UpdateReadyMemo and the functions the entries name run a program or let anything go, by any routine they call (check 7 of R1 to R4, 28 October 2026: three such changes passed); nor is RegisterExtraCloseApplicationsResources, which Inno Setup runs after PrepareToInstall and before the install when it uses the Restart Manager, CancelButtonClick, which it runs on the installing page, is held as the pages' events are, the functions the [Setup] section names (a {code:...} in a directive, a function in a directive Inno Setup evaluates as an expression) are held as those the entries name, and every entry of the last page, in any script, runs only while the folders were as setup left them (check 8 of R1 to R4: six such changes passed); no file under {app} is registered during the copy; no folder but those setup made is given to the uninstaller whatever is in it; nothing but the state's reads and the settings file's move runs at ssInstall; whether the folders were checked is set and cleared by CheckProgramFolders and PrepareProgramFolders alone; and /ProtectFolders runs after the files. Each passed the checker and these tests before.")]
      public void WhatInnoSetupDoesOutsideTheCodeKeepsWhatSetupPrepared()
      {
         var failed = new List<string>();
         var called = new HashSet<string>();
         foreach (FileInfo script in InstallerScripts())
         {
            string text = ScriptText(File.ReadAllText(script.FullName));
            foreach (string[] section in new[] { new[] { "Dirs", "Name" }, new[] { "Files", "DestDir" } })
               foreach (KeyValuePair<int, string> entry in SectionEntries(text, section[0]))
               {
                  string where = (EntryParameter(entry.Value, section[1]) ?? "").ToLowerInvariant();
                  bool ours = where.StartsWith("{app}") || where.StartsWith("{code:");
                  string[] flags = (EntryParameter(entry.Value, "Flags") ?? "").ToLowerInvariant().Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                  if (ours && EntryParameter(entry.Value, "Permissions") != null)
                     failed.Add(script.Name + ":" + entry.Key + " gives permissions under {app}");
                  if (section[0] == "Files" && ours && flags.Any(flag => flag == "regserver" || flag == "regtypelib" || flag == "regserverx64" || flag == "regtypelibx64"))
                     failed.Add(script.Name + ":" + entry.Key + " registers a file under {app} during the copy");
                  if (section[0] == "Dirs" && flags.Contains("uninsalwaysuninstall") && EntryParameter(entry.Value, "Check") == null)
                     failed.Add(script.Name + ":" + entry.Key + " gives the uninstaller a folder whatever made it");
               }
            foreach (KeyValuePair<int, string> entry in SectionEntries(text, "Run"))
            {
               string[] flags = (EntryParameter(entry.Value, "Flags") ?? "").ToLowerInvariant().Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
               if ((EntryParameter(entry.Value, "Filename") ?? "").ToLowerInvariant().StartsWith("{app}") && !flags.Contains("postinstall"))
                  failed.Add(script.Name + ":" + entry.Key + " runs a program from {app} before ssPostInstall");
               // Only the .NET runtime from {tmp} runs before ssPostInstall, with nothing of
               // {app} or of a {code:} in it (check 5 of R1 to R4, 27 October 2026).
               string whole = entry.Value.ToLowerInvariant();
               if (!flags.Contains("postinstall") && (EntryParameter(entry.Value, "Filename") != @"{tmp}\{#DOTNET_RUNTIME_FILE}" || whole.Contains("{app}") || whole.Contains("{code:")))
                  failed.Add(script.Name + ":" + entry.Key + " runs something other than the .NET runtime from {tmp} before ssPostInstall");
               // A last-page entry of any script, not of section_run.iss alone, is held to
               // FoldersWereAsPrepared (check 8 of R1 to R4, 28 October 2026).
               if (flags.Contains("postinstall") && EntryParameter(entry.Value, "Check") != "FoldersWereAsPrepared")
                  failed.Add(script.Name + ":" + entry.Key + " is an entry of the last page that runs whether or not the folders setup prepared were as it left them");
            }
            // The [Setup] section's functions: a {code:...} in any directive, and what a
            // directive Inno Setup evaluates as an expression names (EvalDirectiveCheck: the
            // nine below) - check 8 of R1 to R4, 28 October 2026.
            foreach (KeyValuePair<int, string> entry in SectionEntries(text, "Setup"))
            {
               Match directive = Regex.Match(entry.Value, @"^(\w+)\s*=\s*(.*)$");
               if (!directive.Success)
                  continue;
               foreach (Match code in Regex.Matches(directive.Groups[2].Value, @"\{code:(\w+)"))
                  called.Add(code.Groups[1].Value);
               if (new[] { "changesassociations", "changesenvironment", "createuninstallregkey", "uninstallable", "usepreviousappdir", "usepreviousgroup",
                           "useprevioussetuptype", "useprevioustasks", "useprevioususerinfo" }.Contains(directive.Groups[1].Value.ToLowerInvariant()))
               {
                  string value = Regex.Replace(directive.Groups[2].Value, @"'(?:[^']|'')*'|""[^""]*""", " ");
                  foreach (Match word in Regex.Matches(value, @"\b[A-Za-z_]\w*\b"))
                     if (!new[] { "yes", "no", "true", "false", "not", "and", "or" }.Contains(word.Value.ToLowerInvariant()))
                        called.Add(word.Value);
               }
            }
            // No entry names a procedure to run before or after it, and the functions entries
            // name for Inno Setup to call during the install are those held here.
            foreach (string section in new[] { "Files", "Dirs", "Icons", "INI", "Registry", "Run", "UninstallRun", "InstallDelete", "UninstallDelete", "Tasks", "Components", "Types" })
               foreach (KeyValuePair<int, string> entry in SectionEntries(text, section))
               {
                  foreach (string hook in new[] { "BeforeInstall", "AfterInstall" })
                     if (EntryParameter(entry.Value, hook) != null)
                        failed.Add(script.Name + ":" + entry.Key + " runs " + EntryParameter(entry.Value, hook) + " as its " + hook + " during the install");
                  foreach (Match code in Regex.Matches(entry.Value, @"\{code:(\w+)"))
                     called.Add(code.Groups[1].Value);
                  string check = EntryParameter(entry.Value, "Check");
                  if (!string.IsNullOrEmpty(check))
                     foreach (Match name in Regex.Matches(check.Trim(), @"\b(?!not\b|and\b|or\b)(\w+)\s*(?:\(|$|\s)"))
                        called.Add(name.Groups[1].Value);
               }
         }
         CollectionAssert.IsEmpty(failed, "What Inno Setup does outside the [Code] undoes what setup prepared, or runs it before it is read again: " + string.Join("; ", failed));
         CollectionAssert.AreEquivalent(new[] { "DotNetDesktopMissing", "FoldersWereAsPrepared", "FolderMadeBySetup", "SetupMadeFolder", "SetupMadeProgramFolder", "SetupMadeBin", "SetupMadeControlPanel" },
            called.ToArray(), "The functions the installer's sections name for Inno Setup to call during the install are not those held.");

         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string step = CodeBody(extension, "CurStepChanged");
         int start = step.IndexOf("if CurStep = ssInstall then", StringComparison.Ordinal);
         int end = step.IndexOf("if CurStep = ssPostInstall then", StringComparison.Ordinal);
         Assert.IsTrue(start >= 0 && end > start, "CurStepChanged has no ssInstall step before its ssPostInstall step.");
         string block = Regex.Replace(step.Substring(start, end - start), @"//[^\n]*", " ");
         CollectionAssert.IsSubsetOf(Regex.Matches(block, @"\b(\w+)\s*\(").Cast<Match>().Select(match => match.Groups[1].Value).Distinct().ToArray(),
            new[] { "IsNewDatabase", "GetServiceAccountBefore", "IsNewService", "MoveIni" }, "CurStepChanged runs something at ssInstall, before the files.");
         var writers = RoutineBodies(extension).Values
            .SelectMany(body => Regex.Matches(body[1], @"g_bFoldersChecked\s*:=\s*(\w+)").Cast<Match>().Select(match => body[0] + "=" + match.Groups[1].Value)).ToArray();
         CollectionAssert.AreEquivalent(new[] { "CheckProgramFolders=true", "PrepareProgramFolders=false" }, writers,
            "Whether the folders were checked is set or cleared elsewhere than in CheckProgramFolders and PrepareProgramFolders.");
         StringAssert.Contains("ProtectFolders();", CodeBody(extension, "RunPostInstallTasks"), "RunPostInstallTasks no longer runs /ProtectFolders after the files.");

         // What lets go of what setup holds, or keeps in the folders it prepared, is called
         // where it is now alone; CurInstallProgressChanged, NeedRestart and
         // RegisterPreviousData, which Inno Setup calls between PrepareToInstall and
         // ssPostInstall, are not defined; and CurPageChanged, ShouldSkipPage, UpdateReadyMemo
         // and the functions the entries name run no program and let nothing go, by any
         // routine they call (check 7 of R1 to R4, 28 October 2026).
         var releases = new Dictionary<string, string[]>
         {
            { "ReleaseHeldFolders", new[] { "CheckProgramFolders", "DeinitializeSetup" } },
            { "ReleasePlaceholders", new[] { "CurStepChanged", "DeinitializeSetup" } },
            { "LetGoOfHeld", new[] { "RemakeEmptyFolder", "RemoveMadeFolder" } },
         };
         var routines = RoutineBodies(extension);
         foreach (KeyValuePair<string, string[]> release in releases)
            CollectionAssert.AreEquivalent(release.Value,
               routines.Where(pair => pair.Key != release.Key.ToLowerInvariant() && Regex.IsMatch(Regex.Replace(pair.Value[1], @"//[^\n]*|\{(?!#)[^}]*\}|'(?:[^'\n]|'')*'", " "), @"\b" + release.Key + @"\s*\("))
                       .Select(pair => pair.Value[0]).ToArray(),
               release.Key + " is called from elsewhere than " + string.Join(" and ", release.Value) + ", so that what setup holds is let go before the look after the files.");
         // RegisterExtraCloseApplicationsResources, which Inno Setup runs when it uses the
         // Restart Manager, after PrepareToInstall and before the install, is refused too, and
         // CancelButtonClick, which it runs on the installing page during the copy, is held
         // (check 8 of R1 to R4, 28 October 2026).
         foreach (string refused in new[] { "CurInstallProgressChanged", "NeedRestart", "RegisterPreviousData", "RegisterExtraCloseApplicationsResources" })
            Assert.IsFalse(routines.ContainsKey(refused.ToLowerInvariant()), refused + ", which Inno Setup calls between PrepareToInstall and ssPostInstall, is defined.");
         var runs = new List<string>();
         var held = new[] { "exec", "shellexec", "execasoriginaluser", "shellexecasoriginaluser", "protectfolders", "releaseheldfolders", "releaseplaceholders", "letgoofheld" };
         foreach (string name in new[] { "CurPageChanged", "ShouldSkipPage", "UpdateReadyMemo", "CancelButtonClick", "DotNetDesktopMissing", "FoldersWereAsPrepared", "FolderMadeBySetup", "SetupMadeFolder",
                                         "SetupMadeProgramFolder", "SetupMadeBin", "SetupMadeControlPanel" })
            if (routines.ContainsKey(name.ToLowerInvariant()))
               runs.AddRange(RoutineReach(routines, name).Intersect(held).OrderBy(call => call, StringComparer.Ordinal).Select(call => name + " runs " + call));
         Assert.IsEmpty(runs, "What Inno Setup calls before the folders are looked at, as the install begins or during it runs a program or lets what setup holds go: " + string.Join("; ", runs) + ".");
      }

      /// <summary>
      ///    The routines of the [Code] that Name calls, itself or through any routine it
      ///    calls, and the functions that run a program among them (Exec, ShellExec,
      ///    ExecAsOriginalUser, ShellExecAsOriginalUser), each in lower case, read from the
      ///    code of each with its comments and strings left out - as
      ///    build/check-installer-scripts.py's routine_reach reads them (check 7 of R1 to R4,
      ///    28 October 2026).
      /// </summary>
      private static HashSet<string> RoutineReach(Dictionary<string, string[]> routines, string name)
      {
         var runners = new HashSet<string> { "exec", "shellexec", "execasoriginaluser", "shellexecasoriginaluser" };
         var seen = new HashSet<string>();
         var todo = new Stack<string>();
         todo.Push(name.ToLowerInvariant());
         while (todo.Count > 0)
         {
            string[] body;
            if (!routines.TryGetValue(todo.Pop(), out body))
               continue;
            string code = CodeToken.Replace(body[1], match => match.Value.StartsWith("'", StringComparison.Ordinal) ? "''" : " ");
            foreach (Match word in Regex.Matches(code, @"\w+"))
            {
               string key = word.Value.ToLowerInvariant();
               if (key == name.ToLowerInvariant() || seen.Contains(key) || !(routines.ContainsKey(key) || runners.Contains(key)))
                  continue;
               seen.Add(key);
               if (routines.ContainsKey(key))
                  todo.Push(key);
            }
         }
         return seen;
      }

      [Test]
      [Description("Work item 78 (check 6 of R1 to R4, 28 October 2026): ISCC compiles what the Inno Setup preprocessor makes of the scripts, and the tests above read them as written. A line whose first character that is not white space is '#' - #if 0 and #endif, #emit, #include, #define, #pragma - and every inline directive, {#...}, which the preprocessor replaces with the text of its expression in a comment or a string too, is held to where it is now: the script, the routine of the [Code] or the section it is in, the line, the lines that are not blank before and after it, and how many times (build/check-installer-scripts.py's ISPP_PLACES). Check 6 found six one-line inline directives that switched R1, R2 or R3 off, and #if 0 around section_dirs.iss's {app} entry, passed by the checker and by this fixture.")]
      public void TheScriptsAreHeldToWhatThePreprocessorMakesOfThem()
      {
         var held = new Dictionary<string, int>();
         foreach (string[] place in IsppPlaces)
            held[string.Join("\u0001", place.Take(5))] = int.Parse(place[5]);
         var found = new Dictionary<string, int>();
         var numbers = new Dictionary<string, int>();
         foreach (string path in Directory.GetFiles(InstallationRoot).Where(file => file.EndsWith(".iss", StringComparison.OrdinalIgnoreCase)).OrderBy(file => file, StringComparer.Ordinal))
         {
            string name = Path.GetFileName(path);
            foreach (string[] line in IsppLines(ScriptText(File.ReadAllText(path))))
            {
               string key = string.Join("\u0001", new[] { name, line[0], line[1], line[2], line[3] });
               found[key] = found.TryGetValue(key, out int count) ? count + 1 : 1;
               if (!numbers.ContainsKey(key))
                  numbers[key] = int.Parse(line[4]);
            }
         }
         var failed = new List<string>();
         foreach (string key in found.Keys.Union(held.Keys).OrderBy(key => key, StringComparer.Ordinal))
         {
            found.TryGetValue(key, out int have);
            held.TryGetValue(key, out int want);
            if (have == want)
               continue;
            string[] parts = key.Split('\u0001');
            failed.Add(want == 0 ? parts[0] + " line " + numbers[key] + " (" + parts[1] + "): \"" + parts[3] + "\" is read by the preprocessor and not held there"
                       : have == 0 ? parts[0] + " (" + parts[1] + "): \"" + parts[3] + "\", after \"" + parts[2] + "\", is held there and is not there"
                       : parts[0] + " line " + numbers[key] + ": \"" + parts[3] + "\" is there " + have + " times, held " + want);
         }
         Assert.IsEmpty(failed, "What the Inno Setup preprocessor reads in the installer's scripts is not where work item 78 holds it:\n" + string.Join("\n", failed));
      }

      /// <summary>
      ///    The lines of one installer script the Inno Setup preprocessor reads - a line whose
      ///    first character that is not white space is '#', or one with "{#" - each as where it
      ///    is (the routine of the [Code] by its name, or else "[section]" in lower case, or ""
      ///    before the first section), the nearest line before it that is not blank, the line,
      ///    the nearest after it, all trimmed, and its number (build/check-installer-scripts.py's
      ///    ispp_lines).
      /// </summary>
      private static List<string[]> IsppLines(string text)
      {
         text = ScriptText(text);
         string[] lines = text.Split('\n');
         var routine = new string[lines.Length];
         var starts = new List<int> { 0 };
         foreach (string one in lines)
            starts.Add(starts[starts.Count - 1] + one.Length + 1);
         Func<int, int> lineOf = offset => { int at = starts.BinarySearch(offset); return at >= 0 ? at : ~at - 1; };
         var externals = new HashSet<string>(ExternalDeclaration.Matches(text).Cast<Match>().Select(match => match.Groups[1].Value.ToLowerInvariant()));
         var closing = new Regex(@"^end;", RegexOptions.Multiline);
         foreach (Match header in RoutineHeader.Matches(text))
         {
            if (externals.Contains(header.Groups[1].Value.ToLowerInvariant()))
               continue;
            Match end = closing.Match(text, header.Index + header.Length);
            if (!end.Success)
               continue;
            int first = lineOf(header.Index);
            int last = lineOf(end.Index);
            for (int index = first; index <= last; index++)
               if (string.IsNullOrEmpty(routine[index]))
                  routine[index] = header.Groups[1].Value;
         }
         var found = new List<string[]>();
         string section = "";
         var sectionHeader = new Regex(@"^\s*\[([^\]]+)\]\s*$");
         var directive = new Regex(@"^[ \t]*#");
         for (int index = 0; index < lines.Length; index++)
         {
            Match header = sectionHeader.Match(lines[index]);
            if (header.Success && string.IsNullOrEmpty(routine[index]))
               section = "[" + header.Groups[1].Value.Trim().ToLowerInvariant() + "]";
            if (!directive.IsMatch(lines[index]) && !lines[index].Contains("{#"))
               continue;
            string before = Enumerable.Range(0, index).Reverse().Select(other => lines[other].Trim()).FirstOrDefault(other => other.Length > 0) ?? "";
            string after = Enumerable.Range(index + 1, lines.Length - index - 1).Select(other => lines[other].Trim()).FirstOrDefault(other => other.Length > 0) ?? "";
            found.Add(new[] { string.IsNullOrEmpty(routine[index]) ? section : routine[index], before, lines[index].Trim(), after, (index + 1).ToString() });
         }
         return found;
      }

      // build/check-installer-scripts.py's ISPP_PLACES: (script, where, the line before, the
      // line, the line after, how many times).
      private static readonly string[][] IsppPlaces =
      {
         new[] { "hMailServer64.iss", "", "", "#define HMAILSERVERLIBS = GetEnv(\"hMailServerLibs\")", "#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + \"\\openssl-4.0.3\\out64\\bin\"", "1" },
         new[] { "hMailServer64.iss", "", "#define DOTNET_CHANNEL \"10.0\"", "#define DOTNET_RUNTIME_FILE \"windowsdesktop-runtime-\" + DOTNET_CHANNEL + \"-win-x64.exe\"", "#include \"section_setup.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#define DOTNET_MAJOR \"10\"", "#define DOTNET_CHANNEL \"10.0\"", "#define DOTNET_RUNTIME_FILE \"windowsdesktop-runtime-\" + DOTNET_CHANNEL + \"-win-x64.exe\"", "1" },
         new[] { "hMailServer64.iss", "", "#define DOTNET_RUNTIME_FILE \"windowsdesktop-runtime-\" + DOTNET_CHANNEL + \"-win-x64.exe\"", "#include \"section_setup.iss\"", "#include \"section_setup_64.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#define HMAILSERVERLIBS = GetEnv(\"hMailServerLibs\")", "#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + \"\\openssl-4.0.3\\out64\\bin\"", "#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + \"\\postgresql-18.6\\builddir\\src\\interfaces\\libpq\"", "1" },
         new[] { "hMailServer64.iss", "", "#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + \"\\openssl-4.0.3\\out64\\bin\"", "#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + \"\\postgresql-18.6\\builddir\\src\\interfaces\\libpq\"", "; The bundled .NET Desktop Runtime, defined in one place because it used to be", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_components.iss\"", "#include \"section_files_common.iss\"", "#include \"section_files_64.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_custom_messages.iss\"", "#include \"section_languages.iss\"", "#include \"section_istool.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_dirs.iss\"", "#include \"section_run.iss\"", "#include \"section_uninstallrun.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_files_64.iss\"", "#include \"section_messages.iss\"", "#include \"section_ini.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_files_common.iss\"", "#include \"section_files_64.iss\"", "#include \"section_messages.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_icons.iss\"", "#include \"hMailServerInnoExtension.iss\"", "", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_ini.iss\"", "#include \"section_registry.iss\"", "#include \"section_dirs.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_istool.iss\"", "#include \"section_types.iss\"", "#include \"section_components.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_languages.iss\"", "#include \"section_istool.iss\"", "#include \"section_types.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_messages.iss\"", "#include \"section_ini.iss\"", "#include \"section_registry.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_registry.iss\"", "#include \"section_dirs.iss\"", "#include \"section_run.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_run.iss\"", "#include \"section_uninstallrun.iss\"", "#include \"section_icons.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_setup.iss\"", "#include \"section_setup_64.iss\"", "#include \"section_custom_messages.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_setup_64.iss\"", "#include \"section_custom_messages.iss\"", "#include \"section_languages.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_types.iss\"", "#include \"section_components.iss\"", "#include \"section_files_common.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "#include \"section_uninstallrun.iss\"", "#include \"section_icons.iss\"", "#include \"hMailServerInnoExtension.iss\"", "1" },
         new[] { "hMailServer64.iss", "", "; forward across minor and patch versions but never across a major version.", "#define DOTNET_MAJOR \"10\"", "#define DOTNET_CHANNEL \"10.0\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#define DOTNET_CHANNEL \"10.0\"", "#define DOTNET_RUNTIME_FILE \"windowsdesktop-runtime-\" + DOTNET_CHANNEL + \"-win-arm64.exe\"", "; The version is read from section_setup_64.iss, so that the release's version", "1" },
         new[] { "hMailServerARM64.iss", "", "#define DOTNET_MAJOR \"10\"", "#define DOTNET_CHANNEL \"10.0\"", "#define DOTNET_RUNTIME_FILE \"windowsdesktop-runtime-\" + DOTNET_CHANNEL + \"-win-arm64.exe\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#define HMAILSERVERLIBS = GetEnv(\"hMailServerLibs\")", "#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + \"\\openssl-4.0.3-arm64\\out64\\bin\"", "#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + \"\\postgresql-18.6-arm64\\builddir\\src\\interfaces\\libpq\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#define HM_APP_VERSION ReadIni(SourcePath + \"\\section_setup_64.iss\", \"Setup\", \"AppVersion\", \"\")", "#define HM_VERSION_INFO ReadIni(SourcePath + \"\\section_setup_64.iss\", \"Setup\", \"VersionInfoVersion\", \"\")", "#if HM_APP_VERSION == \"\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#define HM_ARM64", "#define HMAILSERVERLIBS = GetEnv(\"hMailServerLibs\")", "#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + \"\\openssl-4.0.3-arm64\\out64\\bin\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#define HM_VERSION_INFO ReadIni(SourcePath + \"\\section_setup_64.iss\", \"Setup\", \"VersionInfoVersion\", \"\")", "#if HM_APP_VERSION == \"\"", "#error section_setup_64.iss names no AppVersion for the ARM64 installer to take", "1" },
         new[] { "hMailServerARM64.iss", "", "#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + \"\\openssl-4.0.3-arm64\\out64\\bin\"", "#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + \"\\postgresql-18.6-arm64\\builddir\\src\\interfaces\\libpq\"", "; As in hMailServer64.iss, which says why these are defined once: the ARM64 build", "1" },
         new[] { "hMailServerARM64.iss", "", "#endif", "#include \"section_setup.iss\"", "#include \"section_setup_arm64.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#error section_setup_64.iss names no AppVersion for the ARM64 installer to take", "#endif", "#include \"section_setup.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#if HM_APP_VERSION == \"\"", "#error section_setup_64.iss names no AppVersion for the ARM64 installer to take", "#endif", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_components.iss\"", "#include \"section_files_common.iss\"", "#include \"section_files_arm64.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_custom_messages.iss\"", "#include \"section_languages.iss\"", "#include \"section_istool.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_dirs.iss\"", "#include \"section_run.iss\"", "#include \"section_uninstallrun.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_files_arm64.iss\"", "#include \"section_messages.iss\"", "#include \"section_ini.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_files_common.iss\"", "#include \"section_files_arm64.iss\"", "#include \"section_messages.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_icons.iss\"", "#include \"hMailServerInnoExtension.iss\"", "", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_ini.iss\"", "#include \"section_registry.iss\"", "#include \"section_dirs.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_istool.iss\"", "#include \"section_types.iss\"", "#include \"section_components.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_languages.iss\"", "#include \"section_istool.iss\"", "#include \"section_types.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_messages.iss\"", "#include \"section_ini.iss\"", "#include \"section_registry.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_registry.iss\"", "#include \"section_dirs.iss\"", "#include \"section_run.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_run.iss\"", "#include \"section_uninstallrun.iss\"", "#include \"section_icons.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_setup.iss\"", "#include \"section_setup_arm64.iss\"", "#include \"section_custom_messages.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_setup_arm64.iss\"", "#include \"section_custom_messages.iss\"", "#include \"section_languages.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_types.iss\"", "#include \"section_components.iss\"", "#include \"section_files_common.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "#include \"section_uninstallrun.iss\"", "#include \"section_icons.iss\"", "#include \"hMailServerInnoExtension.iss\"", "1" },
         new[] { "hMailServerARM64.iss", "", "; and build\\get-dotnet-runtime.ps1 -Platform ARM64 places this one).", "#define DOTNET_MAJOR \"10\"", "#define DOTNET_CHANNEL \"10.0\"", "1" },
         new[] { "hMailServerARM64.iss", "", "; has no ARM64 build, and an ARM64 process cannot load an x64 OLE DB provider.", "#define HM_ARM64", "#define HMAILSERVERLIBS = GetEnv(\"hMailServerLibs\")", "1" },
         new[] { "hMailServerARM64.iss", "", "; stamp (RELEASE.md step 7) stays one file for the two installers.", "#define HM_APP_VERSION ReadIni(SourcePath + \"\\section_setup_64.iss\", \"Setup\", \"AppVersion\", \"\")", "#define HM_VERSION_INFO ReadIni(SourcePath + \"\\section_setup_64.iss\", \"Setup\", \"VersionInfoVersion\", \"\")", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#define HM_CONSTANTS_FILE AddBackslash(SourcePath) + \"..\\source\\Server\\Common\\Application\\Constants.h\"", "#define HM_REQUIRED_DB_VERSION \"\"", "#define HmConstantsHandle 0", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#define HM_REQUIRED_DB_VERSION \"\"", "#define HmConstantsHandle 0", "#define HmConstantsLine \"\"", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#define HmConstantsHandle 0", "#define HmConstantsLine \"\"", "#sub HmReadConstantsLine", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#define HmConstantsLine \"\"", "#sub HmReadConstantsLine", "#expr HmConstantsLine = Trim(FileRead(HmConstantsHandle))", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#endif", "#endsub", "#for {HmConstantsHandle = FileOpen(HM_CONSTANTS_FILE); HmConstantsHandle && !FileEof(HmConstantsHandle); 0} HmReadConstantsLine", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#endif", "#if HM_REQUIRED_DB_VERSION == \"\" || Int(HM_REQUIRED_DB_VERSION, 0) < 6000", "#error REQUIRED_DB_VERSION could not be read from Constants.h", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#endsub", "#for {HmConstantsHandle = FileOpen(HM_CONSTANTS_FILE); HmConstantsHandle && !FileEof(HmConstantsHandle); 0} HmReadConstantsLine", "#if HmConstantsHandle", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#error REQUIRED_DB_VERSION could not be read from Constants.h", "#endif", "[code]", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#expr FileClose(HmConstantsHandle)", "#endif", "#if HM_REQUIRED_DB_VERSION == \"\" || Int(HM_REQUIRED_DB_VERSION, 0) < 6000", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#expr HM_REQUIRED_DB_VERSION = Trim(Copy(HmConstantsLine, Len(\"#define REQUIRED_DB_VERSION \") + 1))", "#endif", "#endsub", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#expr HmConstantsLine = Trim(FileRead(HmConstantsHandle))", "#if Pos(\"#define REQUIRED_DB_VERSION \", HmConstantsLine) == 1", "#expr HM_REQUIRED_DB_VERSION = Trim(Copy(HmConstantsLine, Len(\"#define REQUIRED_DB_VERSION \") + 1))", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#for {HmConstantsHandle = FileOpen(HM_CONSTANTS_FILE); HmConstantsHandle && !FileEof(HmConstantsHandle); 0} HmReadConstantsLine", "#if HmConstantsHandle", "#expr FileClose(HmConstantsHandle)", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#if HM_REQUIRED_DB_VERSION == \"\" || Int(HM_REQUIRED_DB_VERSION, 0) < 6000", "#error REQUIRED_DB_VERSION could not be read from Constants.h", "#endif", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#if HmConstantsHandle", "#expr FileClose(HmConstantsHandle)", "#endif", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#if Pos(\"#define REQUIRED_DB_VERSION \", HmConstantsLine) == 1", "#expr HM_REQUIRED_DB_VERSION = Trim(Copy(HmConstantsLine, Len(\"#define REQUIRED_DB_VERSION \") + 1))", "#endif", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "#sub HmReadConstantsLine", "#expr HmConstantsLine = Trim(FileRead(HmConstantsHandle))", "#if Pos(\"#define REQUIRED_DB_VERSION \", HmConstantsLine) == 1", "1" },
         new[] { "hMailServerInnoExtension.iss", "", "// variable of its own, gone when it returns.", "#define HM_CONSTANTS_FILE AddBackslash(SourcePath) + \"..\\source\\Server\\Common\\Application\\Constants.h\"", "#define HM_REQUIRED_DB_VERSION \"\"", "1" },
         new[] { "hMailServerInnoExtension.iss", "CheckServiceAfterMove", "iVersion := ReadDatabaseVersion('after the move, through the service running as NT SERVICE\\hMailServer');", "if iVersion = {#HM_REQUIRED_DB_VERSION} then", "Exit;", "1" },
         new[] { "hMailServerInnoExtension.iss", "CheckServiceAfterMove", "if iVersion > 0 then", "szWhat := 'it opened its database at version ' + IntToStr(iVersion) + ', where this build needs {#HM_REQUIRED_DB_VERSION}'", "else", "1" },
         new[] { "hMailServerInnoExtension.iss", "CreateWizardPages", "Checked := False;", "#endif", "end;", "1" },
         new[] { "hMailServerInnoExtension.iss", "CreateWizardPages", "Checked := True;", "#endif", "end;", "1" },
         new[] { "hMailServerInnoExtension.iss", "CreateWizardPages", "OnClick := @rdoUseExternal_Click;", "#ifdef HM_ARM64", "// MySQL and MariaDB need an ARM64 libmysql.dll that this installer does not carry.", "1" },
         new[] { "hMailServerInnoExtension.iss", "CreateWizardPages", "OnClick := @rdoUseInternal_Click;", "#ifdef HM_ARM64", "// SQL Server Compact has no ARM64 build (docs/WindowsOnArm.md).", "1" },
         new[] { "hMailServerInnoExtension.iss", "DotNetDesktopMissing", "Result := True;", "if not FindFirst(ExpandConstant('{pf64}\\dotnet\\shared\\Microsoft.WindowsDesktop.App\\{#DOTNET_MAJOR}.*'), findRec) then", "Exit;", "1" },
         new[] { "hMailServerInnoExtension.iss", "EndDatabaseStep", "end", "else if (g_iDatabaseVersionBefore >= {#HM_REQUIRED_DB_VERSION}) and (g_iDatabaseVersionAfter <= 0) then", "begin", "1" },
         new[] { "hMailServerInnoExtension.iss", "EndDatabaseStep", "g_bDatabaseChanged := false;", "Log('Database step left the database unchanged, at version ' + IntToStr(g_iDatabaseVersionBefore) + ': this build needs version {#HM_REQUIRED_DB_VERSION}, ' +", "'which the database had reached before the step, and nothing in the step moves a database at or past it, though its version after the step could not be read.');", "1" },
         new[] { "hMailServerInnoExtension.iss", "InitializeSetup", "DeleteFile(RefusalFileName());", "// The .NET tools require the .NET {#DOTNET_MAJOR} Desktop Runtime; it is", "// bundled and installed automatically when missing, so no up-front framework", "1" },
         new[] { "hMailServerInnoExtension.iss", "InitializeWizard", "g_bUseInternal := false;", "#endif", "OverrideInstallationFolder();", "1" },
         new[] { "hMailServerInnoExtension.iss", "InitializeWizard", "g_bUseInternal := false;", "#ifdef HM_ARM64", "// No built-in database on ARM64, whatever the command line says: a new", "1" },
         new[] { "hMailServerInnoExtension.iss", "InstallDotNetRuntime", "Exit;", "if (Exec(ExpandConstant('{tmp}\\{#DOTNET_RUNTIME_FILE}'), '/install /quiet /norestart', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) = False) then", "begin", "1" },
         new[] { "hMailServerInnoExtension.iss", "InstallDotNetRuntime", "begin", "SuppressibleMsgBox('The .NET {#DOTNET_MAJOR} Desktop Runtime could not be installed. The database setup tools will not work until it is installed.' + #13#10 +", "SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);", "1" },
         new[] { "hMailServerInnoExtension.iss", "InstallDotNetRuntime", "begin", "SuppressibleMsgBox('The .NET {#DOTNET_MAJOR} Desktop Runtime installation failed with exit code ' + IntToStr(ResultCode) + '.' + #13#10 +", "'The database setup tools will not work until the runtime is installed. ' +", "1" },
         new[] { "hMailServerInnoExtension.iss", "InstallSQLCE", "Exit;", "#endif", "// x64 only: ArchitecturesAllowed=x64 in section_setup_64.iss, so IsWin64 is", "1" },
         new[] { "hMailServerInnoExtension.iss", "InstallSQLCE", "Exit;", "#ifdef HM_ARM64", "// Unreachable: InitializeWizard never chooses the built-in database on ARM64,", "1" },
         new[] { "hMailServerInnoExtension.iss", "PrepareToInstallRefusal", "'The service will not start until an ARM64 one is there (WindowsOnArm.md in the documentation says how to build it).', mbInformation, MB_OK, IDOK);", "#endif", "Result := PrepareProgramFolders(PreviousInstallationFolder());", "1" },
         new[] { "hMailServerInnoExtension.iss", "PrepareToInstallRefusal", "end;", "#ifdef HM_ARM64", "// The ARM64 installer only (hMailServerARM64.iss).", "1" },
         new[] { "hMailServerInnoExtension.iss", "RunPostInstallTasks", "ProgressPage.SetProgress(1,6);", "// The database tools below are .NET {#DOTNET_MAJOR} apps; make sure the", "// runtime is in place before they are executed. section_run.iss's [Run]", "1" },
         new[] { "hMailServerInnoExtension.iss", "RunPostInstallTasks", "ProgressPage.Show();", "ProgressPage.SetText('Installing the .NET {#DOTNET_MAJOR} Desktop Runtime...', '');", "ProgressPage.SetProgress(1,6);", "1" },
         new[] { "hMailServerInnoExtension.iss", "StepsNotCompleteBeforeDatabase", "if not bRuntimeReady then", "AppendStep(szList, 'the .NET {#DOTNET_MAJOR} Desktop Runtime');", "if not bDatabaseBackendReady then", "1" },
         new[] { "hMailServerInnoExtension.iss", "[code]", "// Called from RunPostInstallTasks before the database tools are executed - they", "// are .NET {#DOTNET_MAJOR} apps and run from [Code] at ssPostInstall.", "//", "1" },
         new[] { "hMailServerInnoExtension.iss", "[code]", "// The current download page for the runtime, quoted in the failure message.", "DOTNET_DOWNLOAD_URL = 'https://dotnet.microsoft.com/download/dotnet/{#DOTNET_CHANNEL}';", "// The Service Control Manager's functions, as winsvc.h declares them, through their", "1" },
         new[] { "hMailServerInnoExtension.iss", "[code]", "end;", "#include \"OriginalHMailServer.iss\"", "// Whether the installation this setup would replace is one of the original", "1" },
         new[] { "hMailServerInnoExtension.iss", "[code]", "end;", "// Installs the bundled .NET {#DOTNET_MAJOR} Desktop Runtime when it is missing.", "// Called from RunPostInstallTasks before the database tools are executed - they", "1" },
         new[] { "hMailServerInnoExtension.iss", "[code]", "end;", "// Returns true when the .NET {#DOTNET_MAJOR} Desktop Runtime is not installed, in", "// which case the bundled runtime installer is executed. Also the Check function on", "1" },
         new[] { "section_files_64.iss", "[files]", "; .NET {#DOTNET_MAJOR} Desktop Runtime, installed silently when missing. The server", "; component needs it too: DBSetup/DBSetupQuick/DBUpdater are .NET {#DOTNET_MAJOR}", "; apps that run during installation, from [Code] at ssPostInstall. The [Run] entry", "1" },
         new[] { "section_files_64.iss", "[files]", "; OpenSSL", "Source: \"{#OPENSSL_LIBS_PATH}\\libcrypto-4-x64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "Source: \"{#OPENSSL_LIBS_PATH}\\libssl-4-x64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "1" },
         new[] { "section_files_64.iss", "[files]", "; PQSQL (PostgreSQL client)", "Source: \"{#POSTGRESQL_LIBPQ_PATH}\\*.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "", "1" },
         new[] { "section_files_64.iss", "[files]", "; versions - they already had.", "Source: \"DotNet\\{#DOTNET_RUNTIME_FILE}\"; DestDir: \"{tmp}\"; Flags: deleteafterinstall; Components: server controlpanel;", "Source: \"SQLCE\\SSCERuntime_x64-ENU.msi\"; Flags: deleteafterinstall ; Excludes: \".svn\"; DestDir: \"{tmp}\"; Components: server;", "1" },
         new[] { "section_files_64.iss", "[files]", "Source: \"..\\source\\Tools\\ControlPanel\\publish\\*\"; DestDir: \"{app}\\ControlPanel\"; Flags: ignoreversion recursesubdirs; Components: controlpanel;", "; .NET {#DOTNET_MAJOR} Desktop Runtime, installed silently when missing. The server", "; component needs it too: DBSetup/DBSetupQuick/DBUpdater are .NET {#DOTNET_MAJOR}", "1" },
         new[] { "section_files_64.iss", "[files]", "Source: \"{#OPENSSL_LIBS_PATH}\\libcrypto-4-x64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "Source: \"{#OPENSSL_LIBS_PATH}\\libssl-4-x64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "; PQSQL (PostgreSQL client)", "1" },
         new[] { "section_files_arm64.iss", "[files]", "; OpenSSL, ARM64 (VC-WIN64-ARM)", "Source: \"{#OPENSSL_LIBS_PATH}\\libcrypto-4-arm64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "Source: \"{#OPENSSL_LIBS_PATH}\\libssl-4-arm64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "1" },
         new[] { "section_files_arm64.iss", "[files]", "; PQSQL (PostgreSQL client), ARM64", "Source: \"{#POSTGRESQL_LIBPQ_PATH}\\*.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "", "1" },
         new[] { "section_files_arm64.iss", "[files]", "; exactly as section_files_64.iss's x64 one.", "Source: \"ARM64\\DotNet\\{#DOTNET_RUNTIME_FILE}\"; DestDir: \"{tmp}\"; Flags: deleteafterinstall; Components: server controlpanel;", "; No SQL Server Compact runtime: Microsoft never built one for ARM64, and the x64", "1" },
         new[] { "section_files_arm64.iss", "[files]", "Source: \"..\\source\\Tools\\ControlPanel\\publish-arm64\\*\"; DestDir: \"{app}\\ControlPanel\"; Flags: ignoreversion recursesubdirs; Components: controlpanel;", "; The ARM64 .NET {#DOTNET_MAJOR} Desktop Runtime, installed silently when missing,", "; exactly as section_files_64.iss's x64 one.", "1" },
         new[] { "section_files_arm64.iss", "[files]", "Source: \"{#OPENSSL_LIBS_PATH}\\libcrypto-4-arm64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "Source: \"{#OPENSSL_LIBS_PATH}\\libssl-4-arm64.dll\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;", "; PQSQL (PostgreSQL client), ARM64", "1" },
         new[] { "section_files_common.iss", "[files]", "#endif", "#ifndef HM_ARM64", "; Two files 6.2.3 to 6.3.6 put in Bin\\plugin beside the client's plugins, which this", "1" },
         new[] { "section_files_common.iss", "[files]", "; hMailServer64.iss these lines are what they always were.", "#ifndef HM_ARM64", "Source: \".\\Extras\\7za.exe\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion; Components: server;", "1" },
         new[] { "section_files_common.iss", "[files]", "Source: \".\\Extras\\7za.exe\"; DestDir: \"{app}\\Bin\"; Flags: ignoreversion; Components: server;", "#endif", "; Database scripts", "1" },
         new[] { "section_files_common.iss", "[files]", "Source: \".\\Extras\\dh2048.pem\"; DestDir: \"{app}\\Bin\";  Flags: ignoreversion; Components: server;", "#ifndef HM_ARM64", "; MySQL/MariaDB client library (x64) - MariaDB Connector/C 3.4.11, loaded at runtime as", "1" },
         new[] { "section_files_common.iss", "[files]", "Source: \".\\Extras\\plugin\\sha256_password.dll\"; DestDir: \"{app}\\Bin\\plugin\"; Flags: ignoreversion uninsrestartdelete; Components: server;", "#endif", "#ifndef HM_ARM64", "1" },
         new[] { "section_files_common.iss", "[installdelete]", "Type: files; Name: \"{app}\\Bin\\plugin\\authentication_windows_client.dll\"", "#endif", "", "1" },
         new[] { "section_run.iss", "[run]", "; never go false.", "Filename: \"{tmp}\\{#DOTNET_RUNTIME_FILE}\"; Parameters: \"/install /quiet /norestart\"; StatusMsg: \"Installing .NET {#DOTNET_MAJOR} Desktop Runtime...\"; Check: DotNetDesktopMissing; Components: server controlpanel;", "; The Control Panel on the last page runs only when the folders setup prepared were as", "1" },
         new[] { "section_run.iss", "[run]", "[Run]", "; Install the bundled .NET {#DOTNET_MAJOR} Desktop Runtime, but only when it is", "; missing. This entry is the FIRST attempt, for every component: Inno processes a", "1" },
         new[] { "section_setup_arm64.iss", "[setup]", "; server, and hMailServer64.iss is the installer for it.", "OutputBaseFilename=hMailServer-{#HM_APP_VERSION}-arm64", "AppVerName=hMailServer {#HM_APP_VERSION}-ARM64", "1" },
         new[] { "section_setup_arm64.iss", "[setup]", "AppVersion={#HM_APP_VERSION}", "VersionInfoVersion={#HM_VERSION_INFO}", "; As section_setup_64.iss: the architecture the Control Panel's install checks.", "1" },
         new[] { "section_setup_arm64.iss", "[setup]", "ArchitecturesAllowed=arm64", "AppVersion={#HM_APP_VERSION}", "VersionInfoVersion={#HM_VERSION_INFO}", "1" },
         new[] { "section_setup_arm64.iss", "[setup]", "OutputBaseFilename=hMailServer-{#HM_APP_VERSION}-arm64", "AppVerName=hMailServer {#HM_APP_VERSION}-ARM64", "ArchitecturesInstallIn64BitMode=arm64", "1" },
      };

      [Test]
      [Description("Work item 78, R2 and R3 (check 4 of R1 to R4, 27 October 2026): a folder of the program that is there and that another account could change before this installation - its permissions or its owner, or what is in one that holds something - keeps that right in a handle such an account opened then, whatever setup does to the permissions after. Setup hardens it before the service is stopped, records in a volatile key that it is not to install there until Windows has restarted, and refuses; a refusal whose remedy ends in a restart is recorded the same way. Nothing is removed after the files, and before them an empty folder is removed only through a handle read as that folder; a folder setup hardened in place keeps a file of setup's until the files are in. In the third repair the folder was hardened and installed into, and after the files setup removed links and empty folders by their paths, through a junction too.")]
      public void WhatAnotherAccountCouldChangeIsHardenedAndRefusedUntilARestart()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         var bodies = RoutineBodies(extension);

         // One place hardens and refuses, before the service is stopped, after a check that
         // refused nothing; and the check is reached only through it.
         CollectionAssert.AreEquivalent(new[] { "checkfoldersbeforethestop" },
            bodies.Where(pair => pair.Key != "hardenforrestart" && Regex.IsMatch(pair.Value[1], @"\bHardenForRestart\(")).Select(pair => pair.Key).ToArray(),
            "HardenForRestart is called from elsewhere than CheckFoldersBeforeTheStop.");
         CollectionAssert.AreEquivalent(new[] { "checkfoldersbeforethestop" },
            bodies.Where(pair => pair.Key != "checkprogramfolders" && Regex.IsMatch(pair.Value[1], @"\bCheckProgramFolders\(")).Select(pair => pair.Key).ToArray(),
            "CheckProgramFolders is called from elsewhere than CheckFoldersBeforeTheStop, so that what another account could change may go unhardened and unrefused.");
         CollectionAssert.AreEquivalent(new[] { "hardenforrestart" },
            bodies.Where(pair => Regex.IsMatch(pair.Value[1], @"\bHardenHeldFolder\([^;]*,\s*(?!false\s*,)[\w.]+\s*,\s*szWhy\)")).Select(pair => pair.Key).ToArray(),
            "A folder another account owns is taken outside HardenForRestart, and so installed into without a restart.");
         CollectionAssert.AreEqual(new[] { "g_bFoldersChanged := false" }, Regex.Matches(extension, @"g_bFoldersChanged\s*:=\s*false").Cast<Match>().Select(match => match.Value).ToArray(),
            "Whether a folder was found changed is cleared other than once.");
         StringAssert.Contains("g_bFoldersChanged := false", CodeBody(extension, "CheckProgramFolders"), "Whether a folder was found changed is not cleared when the check starts.");

         // After the files nothing is removed; before them an empty folder only through a
         // handle read as that folder.
         Assert.IsFalse(Regex.IsMatch(CodeBody(extension, "NothingTurnedIn"), @"\b(DeleteFile|RemoveDir|DelTree|RemoveHeldFolder)\("),
            "NothingTurnedIn removes what it finds after the files, by its path or through a folder made a junction.");
         foreach (string name in new[] { "CheckWhatIsInside", "CheckWhatElseIsInside" })
            Assert.IsFalse(Regex.IsMatch(CodeBody(extension, name), @"\b(DeleteFile|RemoveDir|DelTree)\("), name + " removes an empty folder by its path.");
         StringAssert.Contains("HM_REG_OPTION_VOLATILE, HM_KEY_SET_VALUE_64", CodeBody(extension, "RecordRestartNeeded"), "What setup records until the restart is not volatile.");
         // Recorded and looked up by where the walks ended too, so that another name for the
         // same folder finds the record (check 5 of R1 to R4, 27 October 2026).
         StringAssert.Contains("written := WriteKeyText(key, szFinal, 0, HM_REG_SZ, szText, (Length(szText) + 1) * 2);", CodeBody(extension, "RecordRestartNeeded"),
            "The record of a restart is written by {app} as it was typed alone.");
         string checkFolders = CodeBody(extension, "CheckProgramFolders");
         Assert.Greater(checkFolders.IndexOf("if RestartRecordedForWalked(szWhy) then", StringComparison.Ordinal), checkFolders.IndexOf("WalksAgree(g_szControlPanelHeld", StringComparison.Ordinal),
            "The record of a restart is not looked up by where the walks ended, once they are done.");
         Assert.Less(checkFolders.IndexOf("if RestartRecordedForWalked(szWhy) then", StringComparison.Ordinal), checkFolders.IndexOf("NoteFoldersOthersCouldChange();", StringComparison.Ordinal),
            "The record of a restart is looked up after setup has noted what to harden.");
         StringAssert.Contains("FinalPathOfHeld(handle)", CodeBody(extension, "WalkedFinal"), "Where a walk ended is not read through its held handle.");
         StringAssert.Contains("HM_PLACEHOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_CREATE_NEW, HM_PLACEHOLDER_FLAGS", CodeBody(extension, "MakePlaceholder"),
            "The file setup keeps in a folder it hardened in place is shared for deleting, or kept after setup.");
         Assert.IsTrue(Regex.IsMatch(extension, @"HM_PLACEHOLDER_FLAGS\s*=\s*\$04000102;"), "The file setup keeps is not deleted when setup lets it go (FILE_FLAG_DELETE_ON_CLOSE).");

         // Folders found changed end in 12, and hMailServer.Updater puts no earlier version
         // back over them - on 12, or on a refusal whose log has the installer's line - in
         // either of its modes.
         StringAssert.Contains("if g_bFoldersNotAsPrepared then", CodeBody(extension, "GetCustomSetupExitCode"), "Folders found not as setup left them do not end in 12.");
         string updater = ReadRepositoryFile(@"hmailserver\source\Server\hMailServer.Updater\hMailServer.Updater.cpp");
         StringAssert.Contains("const int InstallerFoldersChanged = 12;", updater, "The Updater does not know exit code 12.");
         Assert.AreEqual(2, Regex.Matches(updater, @"if \(code == InstallerFoldersChanged \|\| \(code != 0 && FoldersFoundChanged\(installLog\)\)\)").Count,
            "One of the Updater's two modes puts the earlier version back over folders the installer found changed.");
         Match came = Regex.Match(updater, @"bool ServiceCameBack\(int code, const Arguments &arguments\)\s*\{\s*if \(([^)]*)\)");
         Assert.IsTrue(came.Success && came.Groups[1].Value.Contains("InstallerFoldersChanged"), "The Updater waits for a service the installer set not to start (exit code 12) to come back.");
         Match prefix = Regex.Match(updater, @"const std::wstring prefix = L""([^""]+)"";");
         Match line = Regex.Match(extension, @"function FoldersChangedLine\(\): String;\s*begin\s*Result := '([^']+)';");
         Assert.IsTrue(prefix.Success && line.Success && line.Groups[1].Value.StartsWith(prefix.Groups[1].Value, StringComparison.Ordinal),
            "What the Updater looks for in the installer's log is not the start of FoldersChangedLine.");
      }

      [Test]
      [Description("Work item 78, R4: what is in Bin and ControlPanel at any depth - the scope of the live update's check of an installation - is read through a handle on each, before the service is stopped and again once the two are hardened, and setup does not install where the service and the administrators would run what another account controls: a link, or a program, a library or a folder whose owner is not SYSTEM, the Administrators or TrustedInstaller, or cannot be read, or an entry of whose own lets another account change it - each named, with what to do. What else another account owns, and an entry kept on one of the three folders that lets an account other than the three write, is said in the setup log and in one box in an attended installation. In 6.3.7 and earlier none of it was said, and the first R4 of 6.3.8 said and used such a program (the review of 26 October 2026).")]
      public void WhatAnAdministratorIsToLookAtInTheProgramFolderIsSaid()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         string inside = CodeBody(extension, "CheckWhatIsInside");
         foreach (string word in new[] { "FindFirst(AddBackslash(Folder) + '*', rec)", "HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0)",
                                         "Log('The program folder: refused, ' + szText);", "Log('The program folder: ' + szText);", "g_szFolderFindings", "/setowner *S-1-5-32-544", "FindClose(rec)", "/reset" })
            StringAssert.Contains(word, inside, "CheckWhatIsInside does not use " + word + ".");
         Assert.IsFalse(Regex.IsMatch(inside, @"ReadFolder(Permissions|Owner)\("), "The owners in Bin and ControlPanel are read by their paths.");

         // Before the service is stopped, and again after the hardening.
         string check = CodeBody(extension, "CheckProgramFolders");
         string prepare = CodeBody(extension, "PrepareProgramFolders");
         Assert.IsTrue(check.Contains("CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBin), 0, false, false, found, szWhy)") &&
                       check.Contains("CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanel), 0, false, false, found, szWhy)"),
            "What is in Bin and ControlPanel is not looked at before the service is stopped.");
         // And what else in the program folder holds what is run (the review of 26 October
         // 2026): its own level, and Addons, Events, DBScripts, WebAdmin and Languages at
         // any depth with scripts counted as programs, in both passes.
         Assert.IsTrue(check.Contains("CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, false, found, szWhy)") &&
                       prepare.Contains("CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, true, found, szWhy)"),
            "What else is in the program folder is not looked at before the service is stopped, or again once the folders are hardened.");
         string elsewhere = CodeBody(extension, "CheckWhatElseIsInside");
         foreach (string word in new[] { "'Addons'", "'Events'", "'DBScripts'", "'WebAdmin'", "'Languages'", "'MySQL'", "CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why)", "LinkIsAdministratorsAlone(handle, -1, szText)",
                                         "HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0)", "IsVolumeRoot(App)" })
            StringAssert.Contains(word, elsewhere, "CheckWhatElseIsInside does not use " + word + ".");
         Assert.IsFalse(Regex.IsMatch(elsewhere, @"ReadFolder(Permissions|Owner)\("), "What is in the program folder is read by its path.");
         foreach (string extensionName in new[] { "'.vbs'", "'.js'", "'.sql'", "'.php'", "'.ps1'", "'.cmd'", "'.bat'" })
            StringAssert.Contains(extensionName, CodeBody(extension, "IsScriptFile"), "IsScriptFile does not count " + extensionName + ".");
         int lastHarden = Math.Max(prepare.LastIndexOf("HardenHeldFolder(", StringComparison.Ordinal), prepare.LastIndexOf("MakeFolders(", StringComparison.Ordinal));
         Assert.Greater(prepare.IndexOf("CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, true, false, found, szWhy)", StringComparison.Ordinal), lastHarden,
            "What is in Bin is looked at for the last time before setup has hardened the folders, when another account may still add to them.");
         Assert.Greater(prepare.IndexOf("CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, true, false, found, szWhy)", StringComparison.Ordinal), lastHarden,
            "What is in ControlPanel is looked at for the last time before setup has hardened the folders.");

         // The entries kept that let another account write, after icacls: every account
         // but the three, not the groups of every user alone, whose entries are taken
         // away.
         string held = CodeBody(extension, "HardenHeldFolder");
         Assert.Greater(held.IndexOf("SayWriteEntriesKept(Folder, HeldHandle)", StringComparison.Ordinal), held.IndexOf("icacls hardened", StringComparison.Ordinal),
            "The entries a trusted owner's folder keeps through icacls are not said after icacls.");
         string kept = CodeBody(extension, "SayWriteEntriesKept");
         Assert.IsFalse(kept.Contains("IsBroadSid("), "SayWriteEntriesKept says the groups of every user alone.");
         foreach (string word in new[] { "ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION", "HM_FOLDER_WRITE_RIGHTS", "Log('The program folder: ' + szText);", "g_szFolderFindings", "/remove:g *" })
            StringAssert.Contains(word, kept, "SayWriteEntriesKept does not use " + word + ".");

         // Said in one box when somebody is there to read it.
         Assert.IsTrue(Regex.IsMatch(prepare, @"if\s+\(g_szFolderFindings\s*<>\s*''\)\s+and\s+\(not\s+WizardSilent\(\)\)\s+then\s+SuppressibleMsgBox\("),
            "An attended installation does not show what setup found in the program folder.");
         AssertNoPlainMessageBox(extension, "SayWriteEntriesKept", "CheckWhatIsInside", "OwnEntriesLetOthers", "IsProgramFile", "CheckWhatElseIsInside", "IsScriptFile", "IsDataFolderName");
      }

      [Test]
      [Description("Work item 78, R2 and R4 (check 5 of R1 to R4, 27 October 2026): a data folder of the program folder - Data, Logs, Temp, Database - and the folder a data folder's link leads to are held to a rule of their own before anything changes: the owner SYSTEM, the Administrators, TrustedInstaller or the service, and no entry of their own that lets another account but the service write them, and the Database's files the same; otherwise refused with what to do, but for the earlier installation's that another account owns, which setup takes, keeping the service's entries, and installs into after a restart. hMailServer.exe /ProtectFolders keeps a data folder's own entries and its owner, so before this an account that made an empty Data in a program folder an administrator made at a drive's root kept the message store after the installation. A program, a library, a script or a settings file whose inherited entry, kept from where it was made, lets another account change it notes its folder for the restart rule; and the restart rule counts only the entries setup takes away, so that an entry setup keeps for a named account no longer refuses every run.")]
      public void ADataFolderAnotherAccountControlsIsRefused()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");
         string allowed = CodeBody(extension, "DataFolderIsAllowed");
         foreach (string word in new[] { "ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION", "(not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))",
                                         "OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText)", "/setowner *S-1-5-32-544", "/remove:g *", "restarts Windows" })
            StringAssert.Contains(word, allowed, "DataFolderIsAllowed does not use " + word + ".");
         Assert.AreEqual(1, Regex.Matches(allowed, @"Result\s*:=\s*true").Count, "A data folder is taken where its owner or its entries were not read as they may be.");

         string elsewhere = CodeBody(extension, "CheckWhatElseIsInside");
         Assert.IsTrue(Regex.IsMatch(elsewhere, @"\n\s*if\s+bLink\s+or\s+bCode\s+or\s+bFolder\s+then\s+begin\s+Found\s*:=\s*Found\s*\+\s*1;"),
            "A data folder another account controls is said, not refused.");
         Assert.IsTrue(Regex.IsMatch(elsewhere, @"if\s+bOwner\s+and\s+g_bEarlierInstallation\s+and\s+\(not\s+Final\)\s+then\s+begin\s+AddRestartFolder\(szItem,\s*-1,\s*true,"),
            "The earlier installation's data folder another account owns is not taken before a restart alone.");
         StringAssert.Contains("szWho := DatabaseFilesWhy(szItem, szRemedy);", elsewhere, "The Database's files are not held to their rule.");
         StringAssert.Contains("if not DataFolderIsAllowed(szReal, hHeld, bOwner, szWho, szRemedy) then", CodeBody(extension, "DataLinkLeadsWhereAllowed"),
            "Where a data folder's link leads is not held to the rule of a data folder.");
         string data = CodeBody(extension, "HardenDataFolder");
         StringAssert.Contains("RebuildFolderOfUntrustedOwner(Folder, pDacl, false, true, Why)", data, "A data folder is rebuilt as a program folder, the service's entries dropped or Users given Read.");
         StringAssert.Contains("HeldFolderIsHardened(HeldHandle, true, true, Why)", data, "A data folder setup rebuilt is not read again.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "HardenForRestart"), @"else\s+if\s+g_aRestartData\[index\]\s+then\s+begin\s+if\s+not\s+HardenDataFolder\("),
            "HardenForRestart hardens a data folder as a program folder.");

         // A program's entry it kept from where it was made, and in Bin, ControlPanel and the
         // folders that hold what is run a folder's too, which every file in it then has
         // (check 7 of R1 to R4, 28 October 2026).
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "CheckWhatElseIsInside"), @"else\s+if\s+bCode\s+and\s+\(not\s+Final\)\s+and\s+StaleEntryLetsOthers\("),
            "CheckWhatElseIsInside does not note a program whose inherited entry lets another account change it.");
         Assert.IsTrue(Regex.IsMatch(CodeBody(extension, "CheckWhatIsInside"), @"else\s+if\s+bCode\s+or\s+bFolder\s+then\s+begin\s+if\s+OwnEntriesLetOthers\([^;]*;[^;]*;\s*end\s+else\s+if\s+\(not\s+Final\)\s+and\s+StaleEntryLetsOthers\(pDacl,\s*pFolderDescriptor,\s*pFolderDacl,\s*bKeyStore,\s*bFolder,\s*szText\)"),
            "CheckWhatIsInside does not note a program, or a folder, whose inherited entry lets another account change it.");
         string stale = CodeBody(extension, "StaleEntryLetsOthers");
         foreach (string word in new[] { "HM_INHERITED_ACE", "HM_INHERIT_ONLY", "HM_FOLDER_WRITE_RIGHTS", "not IsTrustedSid(pAce + 8)", "not HandsDownWriteTo(pFolderDescriptor, pFolderDacl, pAce + 8, inherit)", "inherit := HM_CONTAINER_INHERIT" })
            StringAssert.Contains(word, stale, "StaleEntryLetsOthers does not use " + word + ".");
         StringAssert.Contains("szWhat := StaleItemIn(Folder);", CodeBody(extension, "NoteFolderOthersCouldChange"), "A folder whose program kept a stale entry is not noted for the restart rule.");

         // The restart rule counts what setup takes away, not what it keeps.
         string taken = CodeBody(extension, "TakenEntriesLetOthers");
         StringAssert.Contains("(((aceFlags and HM_INHERITED_ACE) <> 0) or IsBroadSid(pAce + 8))", taken, "The restart rule counts an entry setup keeps, or not one it takes away.");
         string others = CodeBody(extension, "OthersCouldChange");
         Assert.IsFalse(others.Contains("AclLetsOthers("), "The restart rule counts a named account's own entry, which setup keeps, so that every run is refused.");
         Assert.AreEqual(2, Regex.Matches(others, @"TakenEntriesLetOthers\(pDacl,").Count, "The restart rule does not ask who may change a folder's permissions and who may write one that holds something.");
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
