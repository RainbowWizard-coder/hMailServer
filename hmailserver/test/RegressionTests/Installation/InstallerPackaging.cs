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
   ///    machine the tests run on, which is why the only end-to-end check lives in
   ///    the installer-smoke GitHub workflow on a throwaway runner. Everything in
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

      [Test]
      [Description("A new installation registers the service as the virtual account NT SERVICE\\hMailServer, an upgrade keeps the account the service has, the page offers the way to LocalSystem in every case, and a silent installation takes /serviceaccount= (docs/ServiceAccount.md).")]
      public void TestNewInstallationDefaultsToTheVirtualAccount()
      {
         string extension = ReadInstallerScript("hMailServerInnoExtension.iss");

         string choice = CodeBody(extension, "GetServiceAccountChoice");
         Assert.IsTrue(Regex.IsMatch(choice, @"if\s+IsNewService\(\)\s+then\s+Result\s*:=\s*'virtual'\s+else\s+Result\s*:=\s*'keep'", RegexOptions.IgnoreCase),
            "GetServiceAccountChoice does not default a new installation to the virtual account and an upgrade to the account the service has. " +
            "An upgrade that moved an existing service would change what it can reach without anybody choosing it.");
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
         string quick = File.ReadAllText(Paths.Combine(SourceRoot, @"Tools\DBSetupQuick\Program.cs"));
         Match upgrade = Regex.Match(quick, @"private static int UpgradeDatabase\(\)(.*?)\n      \}",
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
