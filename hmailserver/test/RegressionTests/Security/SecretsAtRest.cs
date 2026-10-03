// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    Secrets at rest, all of them (docs/SecretsAtRest.md): the three the evidence
   ///    pack of 25 September 2026 found stored as written.
   ///
   ///    The administrator password: the Windows installer wrote it as an unsalted
   ///    MD5 digest and nothing re-hashed it. A sign-in that verifies an MD5 value now
   ///    writes it again as PBKDF2, over COM and over REST - but never over a newer
   ///    password another process has written into the file - and the installer has
   ///    the server write it through hMailServer.exe --set-admin-password, which takes
   ///    the password from the environment or standard input and never from its
   ///    command line.
   ///
   ///    An account's second factor and the [Settings] secrets: kept as written until
   ///    now, sealed from now on under the installation's stored-secrets key - an HMK1
   ///    envelope every node that shares the database opens once the key has been
   ///    carried to it (--export-stored-secrets-key, --import-stored-secrets-key), and
   ///    never a machine-bound one, which would lock a standby out. The key is made by
   ///    an elevated verb only (--create-stored-secrets-key, which this fixture runs
   ///    where the bench has no key), never by the service, and the database records
   ///    it: a second is refused, an import is checked against it, and when it is lost
   ///    --forget-sealed-secrets clears what it sealed. A value stored as written is
   ///    read as it is and sealed at the next start; switching
   ///    ProtectStoredSecretsWithDPAPI off opens them back; a backup carries them
   ///    opened and the restore seals them again.
   ///
   ///    The database probes read the SQL CE test database directly, as
   ///    RestApiSmimeAuthority's do; on another database the tests that need them are
   ///    inconclusive. REST port 10260 and metrics port 10261 are this fixture's.
   /// </summary>
   [TestFixture]
   public class SecretsAtRest : TestFixtureBase
   {
      private const int RestPortPreferred = 10260;
      private const int MetricsPort = 10261;
      private const string AdminPassword = "testar";
      private const string Password = "Secrets-At-Rest-Passw0rd!";

      // What ConfigureMetrics writes, put back as it was found.
      private static readonly string[] MetricsSettings =
      {
         "MetricsServerBindAddress", "MetricsServerPort", "MetricsServerAuthToken", "MetricsServerAuthUsername",
         "MetricsServerAuthPassword", "MetricsServerCertificateFile", "MetricsServerPrivateKeyFile"
      };

      // Every [Settings] key whose value is a secret: the nine the settings loader
      // reads, and the GSSAPI service password, which is read by name.
      private static readonly string[] SecretSettings =
      {
         "SRSSecret", "BATVSecret", "OAuth2HmacSecret", "OAuth2IntrospectionClientSecret",
         "OutboundOAuth2ClientSecret", "OutboundOAuth2FixedToken", "MetricsServerAuthToken",
         "MetricsServerAuthPassword", "ServiceAccountPassword", "GssapiServicePassword"
      };

      // ------------------------------------------------------------- the administrator password

      [Test]
      [Description("An administrator password stored as the installer's unsalted MD5 signs in over COM, and is written again as PBKDF2 by that sign-in; a wrong password changes nothing, and the second sign-in is against the PBKDF2 hash.")]
      public void AnAdministratorPasswordStoredAsMd5IsWrittenAgainAsPbkdf2WhenItSignsIn()
      {
         string before = AdministratorLine();
         string md5 = Md5Hex(AdminPassword);

         try
         {
            IniFileSetting.WriteFileOnly("Security", "AdministratorPassword", md5);
            _application.Reinitialize();
            ClassicAssert.AreEqual(md5, AdministratorLine(), "The precondition: the file holds what the installer used to write.");

            ClassicAssert.IsNull(new hMailServer.Application().Authenticate("Administrator", "not-" + AdminPassword),
               "A wrong password is refused.");
            ClassicAssert.AreEqual(md5, AdministratorLine(), "...and rewrites nothing.");

            ClassicAssert.IsNotNull(new hMailServer.Application().Authenticate("Administrator", AdminPassword),
               "The right password signs in against the MD5 value.");

            string after = AdministratorLine();
            StringAssert.StartsWith("$h1$", after, "The sign-in wrote the password again as PBKDF2.");
            ClassicAssert.AreNotEqual(md5, after);

            ClassicAssert.IsNotNull(new hMailServer.Application().Authenticate("Administrator", AdminPassword),
               "It signs in against what it was written as.");
            ClassicAssert.AreEqual(after, AdministratorLine(), "A PBKDF2 hash of the configured strength is not written again.");

            // And a server started on the file reads it: the in-memory value and the
            // file agree.
            _application.Reinitialize();
            ClassicAssert.IsNotNull(new hMailServer.Application().Authenticate("Administrator", AdminPassword));
         }
         finally
         {
            PutBackAdministratorLine(before);
         }
      }

      [Test]
      [Description("The REST API's sign-in re-hashes the installer's MD5 value too, before the session is made, and the session it makes goes on working.")]
      public void TheRestApiSignInWritesTheMd5ValueAgainAsPbkdf2()
      {
         string before = AdministratorLine();
         StoredSetting certificateBefore = ReadStoredSetting("RestApiCertificateFile");
         StoredSetting keyBefore = ReadStoredSetting("RestApiPrivateKeyFile");
         int restPort = RestListener.Start(RestPortPreferred);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         try
         {
            IniFileSetting.WriteFileOnly("Security", "AdministratorPassword", Md5Hex(AdminPassword));
            _application.Reinitialize();

            (int status, string body) wrong = RestRequest.Send("127.0.0.1", restPort, "GET", "/api/v1/status", Basic("Administrator", "not-" + AdminPassword), null);
            ClassicAssert.AreEqual(401, wrong.status, wrong.body);
            ClassicAssert.AreEqual(Md5Hex(AdminPassword), AdministratorLine(), "A refused sign-in rewrites nothing.");

            // A wrong administrator password costs half a second before the next
            // is looked at, from loopback too (RestApiHostAndThrottle).
            Thread.Sleep(1000);

            (int status, string body) right = RestRequest.Send("127.0.0.1", restPort, "GET", "/api/v1/status", Basic("Administrator", AdminPassword), null);
            ClassicAssert.AreEqual(200, right.status, right.body);
            StringAssert.StartsWith("$h1$", AdministratorLine(), "The REST sign-in wrote the password again as PBKDF2.");

            (int status, string body) again = RestRequest.Send("127.0.0.1", restPort, "GET", "/api/v1/status", Basic("Administrator", AdminPassword), null);
            ClassicAssert.AreEqual(200, again.status, again.body);
         }
         finally
         {
            RestListener.Stop();
            PutBackStoredSetting("RestApiCertificateFile", certificateBefore);
            PutBackStoredSetting("RestApiPrivateKeyFile", keyBefore);
            PutBackAdministratorLine(before);
         }
      }

      [Test]
      [Description("A sign-in with the old password never writes it back over a newer one another process wrote into the file: the service still holds the installer's MD5 of the old password, hMailServer.exe --set-admin-password writes a new one beside it, and the old password's sign-in - accepted by the running service - leaves the file's new line alone; after a reinitialise the new password signs in and the old one does not.")]
      public void ASignInWithTheOldPasswordDoesNotOverwriteANewerOneInTheFile()
      {
         string before = AdministratorLine();
         const string Newer = "a-newer-administrator-password";

         try
         {
            IniFileSetting.WriteFileOnly("Security", "AdministratorPassword", Md5Hex(AdminPassword));
            _application.Reinitialize();

            // Read from the file by its path from here on. AdministratorLine finds
            // the file through COM's Settings.Directories, which reloads the bootstrap
            // sections - the administrator password among them - and would itself
            // give the running service the newer password this test is about.
            string iniPath = IniFileSetting.ExistingIniFiles().First();
            ServerExecutable();

            Verb set = RunSetAdminPassword(null, Newer);
            ClassicAssert.AreEqual(0, set.ExitCode, set.Output);
            string newerLine = IniFile.GetValue("Security", "AdministratorPassword", string.Empty, iniPath);
            StringAssert.StartsWith("$h1$", newerLine, "The verb wrote the newer password.");

            ClassicAssert.IsNotNull(new hMailServer.Application().Authenticate("Administrator", AdminPassword),
               "The running service still verifies the password it loaded.");
            ClassicAssert.AreEqual(newerLine, IniFile.GetValue("Security", "AdministratorPassword", string.Empty, iniPath),
               "The old password's sign-in wrote itself back over the newer password in the file. A password reset because the old one leaked would be undone by whoever holds it.");

            _application.Reinitialize();
            ClassicAssert.IsNotNull(new hMailServer.Application().Authenticate("Administrator", Newer), "The newer password is the one in force.");
            ClassicAssert.IsNull(new hMailServer.Application().Authenticate("Administrator", AdminPassword), "The old one is not.");
         }
         finally
         {
            PutBackAdministratorLine(before);
         }
      }

      [Test]
      [Description("hMailServer.exe --set-admin-password writes PBKDF2, from HM_ADMIN_PASSWORD or from one line of standard input, the environment first; with neither it refuses (exit 2) and changes nothing; it never prints the password.")]
      public void TheSetAdminPasswordVerbWritesPbkdf2FromTheEnvironmentOrStandardInput()
      {
         string before = AdministratorLine();

         try
         {
            Verb none = RunSetAdminPassword(null, null);
            ClassicAssert.AreEqual(2, none.ExitCode, none.Output);
            StringAssert.Contains("No password was given", none.Output);
            ClassicAssert.AreEqual(before, AdministratorLine(), "Refused, nothing was changed.");

            Verb piped = RunSetAdminPassword(AdminPassword + "\r\n", null);
            ClassicAssert.AreEqual(0, piped.ExitCode, piped.Output);
            StringAssert.Contains("PBKDF2", piped.Output);
            StringAssert.DoesNotContain(AdminPassword, piped.Output, "The verb never prints the password.");
            string fromInput = AdministratorLine();
            StringAssert.StartsWith("$h1$", fromInput, "Standard input: written as PBKDF2.");

            _application.Reinitialize();
            ClassicAssert.IsNotNull(new hMailServer.Application().Authenticate("Administrator", AdminPassword),
               "The service reads what the verb wrote.");

            // The environment comes first, and standard input is then not read at all:
            // the installer sets the variable and has no pipe to write to.
            Verb fromEnvironment = RunSetAdminPassword("not-" + AdminPassword + "\r\n", AdminPassword);
            ClassicAssert.AreEqual(0, fromEnvironment.ExitCode, fromEnvironment.Output);
            StringAssert.DoesNotContain(AdminPassword, fromEnvironment.Output);
            string fromVariable = AdministratorLine();
            StringAssert.StartsWith("$h1$", fromVariable);
            ClassicAssert.AreNotEqual(fromInput, fromVariable, "A new salt: the verb wrote the file again.");

            _application.Reinitialize();
            ClassicAssert.IsNotNull(new hMailServer.Application().Authenticate("Administrator", AdminPassword),
               "HM_ADMIN_PASSWORD was the password written.");
            ClassicAssert.IsNull(new hMailServer.Application().Authenticate("Administrator", "not-" + AdminPassword),
               "Standard input, which carried another, was not read.");
         }
         finally
         {
            PutBackAdministratorLine(before);
         }
      }

      // ------------------------------------------------------------- an account's second factor

      [Test]
      [Description("A second factor enrolled now is stored as an HMK1 envelope under the stored-secrets key the database records and not as the secret, a code from an independent RFC 6238 implementation still verifies, and saving the account for another reason keeps it. Its envelope copied into another account's row does not open there: that account keeps a second factor that refuses the code (HM7493).")]
      public void ASecondFactorIsSealedUnderTheStoredSecretsKeyAndStillVerifies()
      {
         RequireProtectedStore();

         string address = "totp-at-rest@" + _domain.Name;
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         string secret = Enrol(account);

         string stored = StoredTotp(account.ID);
         StringAssert.StartsWith("HMK1:" + RecordedKeyId() + ":", stored, "The second factor is sealed under the key the database records.");
         StringAssert.DoesNotContain(secret, stored, "...and not as the secret.");

         ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)),
            "A code computed with .NET's own HMACSHA1 against the secret verifies.");
         ClassicAssert.IsFalse(AuthenticatesWithCode(address, Password, CurrentCode(secret, 5)),
            "A code five steps out does not: the secret opened is the secret, not something that accepts anything.");

         account.PersonFirstName = "Totp";
         account.Save();
         StringAssert.StartsWith("HMK1:", StoredTotp(account.ID), "Saved again, it is sealed again.");
         ClassicAssert.IsTrue(account.TOTPEnabled);
         ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)), "...and still verifies.");

         // The envelope copied into another account's row: the account's id is in the
         // associated data, so it does not open there.
         string otherAddress = "totp-copied@" + _domain.Name;
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, otherAddress, Password);
         ClassicAssert.IsTrue(TryExecute("update hm_accounts set accounttotpsecret = '" + StoredTotp(account.ID) + "' where accountid = " + other.ID));
         _settings.Cache.Clear();

         ClassicAssert.IsFalse(AuthenticatesWithCode(otherAddress, Password, OneTimeCodes.At(secret, 1)),
            "Another account's envelope opened in this account's row: the second factor is not bound to its account.");
         ClassicAssert.IsTrue(_application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(otherAddress).TOTPEnabled,
            "The account keeps a second factor, which refuses every code, rather than none.");
         ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)), "The account whose envelope it is still verifies.");

         // HM7493 is said once a process: here, or by an earlier test.
         ClearErrorLogOf("HM7493");
      }

      [Test]
      [Description("A second factor stored as generated, as every one enrolled before this was, is read as it is, and the next start protects it; the code verifies before and after.")]
      public void ASecondFactorStoredAsGeneratedIsReadAsItIsAndProtectedAtTheNextStart()
      {
         RequireProtectedStore();

         string address = "totp-legacy@" + _domain.Name;
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         Enrol(account);

         // As an older server left it.
         string secret = NewBase32Secret();
         ClassicAssert.IsTrue(TryExecute("update hm_accounts set accounttotpsecret = '" + secret + "' where accountid = " + account.ID));
         _settings.Cache.Clear();

         ClassicAssert.AreEqual(secret, StoredTotp(account.ID), "The precondition: the row holds the secret as generated.");
         ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)),
            "A secret stored as generated is read as it is.");

         _application.Reinitialize();

         string stored = StoredTotp(account.ID);
         StringAssert.StartsWith("HMK1:", stored, "The start sealed the row.");
         StringAssert.DoesNotContain(secret, stored);
         ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)),
            "The protected secret is the same secret.");
      }

      [Test]
      [Description("A backup carries an account's second factor, and a [Settings] secret, opened, and a restore protects each again under the restoring machine's store - a new envelope, not the old one copied - so that they come back on another machine too; the code and the setting work afterwards.")]
      public void ABackupCarriesTheSecondFactorAndTheSettingsSecretsOpenedAndARestoreProtectsThemAgain()
      {
         RequireProtectedStore();

         string address = "totp-backup@" + _domain.Name;
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         string secret = Enrol(account);
         string envelopeBefore = StoredTotp(account.ID);
         StringAssert.StartsWith("HMK1:", envelopeBefore);

         const string SettingName = "SRSSecret";
         StoredSetting settingBefore = ReadStoredSetting(SettingName);
         string settingSecret = "sar-backup-" + Guid.NewGuid().ToString("N");
         IniFileSetting.Write(SettingName, settingSecret);
         string settingEnvelopeBefore = IniFileSetting.Read(SettingName);
         StringAssert.StartsWith("HMK1:", settingEnvelopeBefore);

         string backupDirectory = Paths.Combine(Path.GetTempPath(), TestSetup.UniqueString());
         Directory.CreateDirectory(backupDirectory);

         try
         {
            RunBackup(backupDirectory);
            DeleteAndRestore(backupDirectory);

            Account restored = _application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(address);
            ClassicAssert.IsTrue(restored.TOTPEnabled, "The second factor came back.");

            string envelopeAfter = StoredTotp(restored.ID);
            StringAssert.StartsWith("HMK1:", envelopeAfter, "Sealed on the way in.");
            StringAssert.DoesNotContain(secret, envelopeAfter);
            ClassicAssert.AreNotEqual(envelopeBefore, envelopeAfter,
               "A new envelope: the archive carried the secret opened and the restore sealed it again. The old " +
               "envelope copied would open only where this key is held, and a restore onto a server with another would lose the second factor.");
            ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)), "The same secret.");

            ClassicAssert.AreEqual(settingSecret, _application.Settings.GetIniSetting(SettingName), "The setting came back whole.");
            string settingEnvelopeAfter = IniFileSetting.Read(SettingName);
            StringAssert.StartsWith("HMK1:", settingEnvelopeAfter, "Sealed again on the way in, in the file's line.");
            ClassicAssert.AreNotEqual(settingEnvelopeBefore, settingEnvelopeAfter, "...under a new envelope, as the second factor.");
         }
         finally
         {
            try { Directory.Delete(backupDirectory, true); } catch (IOException) { }
            PutBackStoredSetting(SettingName, settingBefore);
         }
      }

      // ------------------------------------------------------------- the [Settings] secrets

      [Test]
      [Description("Every [Settings] key that holds a secret is stored as an HMK1 envelope in the row and in hMailServer.ini, never as written, and is read back whole; saving the same value again keeps the envelope; a key that holds no secret is stored as written.")]
      public void EverySettingThatHoldsASecretIsStoredProtectedAndReadBackWhole()
      {
         RequireProtectedStore();

         var before = SecretSettings.ToDictionary(name => name, ReadStoredSetting);
         const string Plain = "AuthenticationResultsIdentity";
         StoredSetting plainBefore = ReadStoredSetting(Plain);

         try
         {
            foreach (string name in SecretSettings)
            {
               string secret = "sar-" + name + "-" + Guid.NewGuid().ToString("N");
               IniFileSetting.Write(name, secret);

               ClassicAssert.AreEqual(secret, _settings.GetIniSetting(name),
                  name + ": read back whole, as the server's own reader reads it.");

               string line = IniFileSetting.Read(name);
               StringAssert.StartsWith("HMK1:", line, name + ": the file's line is an envelope.");
               StringAssert.DoesNotContain(secret, line, name + ": the file does not hold the secret.");

               string row;
               if (TryStoredSettingRow(name, out row))
               {
                  StringAssert.StartsWith("HMK1:", row, name + ": the row is an envelope.");
                  ClassicAssert.AreEqual(line, row, name + ": the row and the line are the same envelope.");
               }

               IniFileSetting.Write(name, secret);
               ClassicAssert.AreEqual(line, IniFileSetting.Read(name), name + ": the same value saved again keeps its envelope.");
            }

            // The negative control: a name that holds no secret is stored as written.
            IniFileSetting.Write(Plain, "sar.example.test");
            ClassicAssert.AreEqual("sar.example.test", IniFileSetting.Read(Plain), "A setting that holds no secret is not protected.");
         }
         finally
         {
            foreach (KeyValuePair<string, StoredSetting> entry in before)
               PutBackStoredSetting(entry.Key, entry.Value);
            PutBackStoredSetting(Plain, plainBefore);
         }
      }

      [Test]
      [Description("A [Settings] secret an older server stored as written, in the row and in the file, is protected by the next start, and reads the same.")]
      public void ASettingSecretStoredAsWrittenIsProtectedAtTheNextStart()
      {
         RequireProtectedStore();

         const string Name = "BATVSecret";
         StoredSetting before = ReadStoredSetting(Name);
         string plain = "sar-legacy-" + Guid.NewGuid().ToString("N");

         try
         {
            // A row to rewrite, then the row and the line as an older server left them,
            // agreeing with each other, so that the start sees no edit to report.
            IniFileSetting.Write(Name, "placeholder");
            ClassicAssert.IsTrue(TryExecute("update hm_inisettings set inisettingvalue = '" + plain + "', inisettingfilevalue = '" + plain + "' where inisettingname = '" + Name + "'"));
            IniFileSetting.WriteFileOnly(Name, plain);

            _application.Reinitialize();

            ClassicAssert.AreEqual(plain, _application.Settings.GetIniSetting(Name), "The value is what it was.");
            string line = IniFileSetting.Read(Name);
            StringAssert.StartsWith("HMK1:", line, "The start sealed the file's line.");
            string row;
            ClassicAssert.IsTrue(TryStoredSettingRow(Name, out row));
            StringAssert.StartsWith("HMK1:", row, "...and the row.");
            ClassicAssert.AreEqual(line, row);
         }
         finally
         {
            PutBackStoredSetting(Name, before);
         }
      }

      [Test]
      [Description("Switching ProtectStoredSecretsWithDPAPI to 0 undoes what was sealed: at the next start a sealed [Settings] secret and a sealed second factor are stored as written again, in the row and in the file, and read the same; a secret saved while it is 0 is stored as written. Switched back on, the next start seals them again.")]
      public void SwitchingTheStoreOffOpensWhatWasSealed()
      {
         RequireProtectedStore();

         const string Name = "BATVSecret";
         string protectionBefore = _settings.GetIniSetting("ProtectStoredSecretsWithDPAPI");
         StoredSetting before = ReadStoredSetting(Name);
         string plain = "sar-off-" + Guid.NewGuid().ToString("N");

         string address = "totp-off@" + _domain.Name;
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         string secret = Enrol(account);

         try
         {
            IniFileSetting.Write(Name, plain);
            StringAssert.StartsWith("HMK1:", IniFileSetting.Read(Name), "The precondition: sealed while the store is on.");
            StringAssert.StartsWith("HMK1:", StoredTotp(account.ID));

            IniFileSetting.Write("ProtectStoredSecretsWithDPAPI", "0");
            _application.Reinitialize();

            ClassicAssert.AreEqual(plain, IniFileSetting.Read(Name), "Switched off, the start put the setting back as written in the file.");
            string row;
            ClassicAssert.IsTrue(TryStoredSettingRow(Name, out row));
            ClassicAssert.AreEqual(plain, row, "...and in the row.");
            ClassicAssert.AreEqual(plain, _application.Settings.GetIniSetting(Name));

            ClassicAssert.AreEqual(secret, StoredTotp(account.ID), "...and the second factor as generated.");
            ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)), "It verifies as it did.");

            string other = plain + "-2";
            IniFileSetting.Write(Name, other);
            ClassicAssert.AreEqual(other, IniFileSetting.Read(Name), "A secret saved while the store is off is kept as written.");

            IniFileSetting.Write("ProtectStoredSecretsWithDPAPI", "1");
            _application.Reinitialize();
            StringAssert.StartsWith("HMK1:", IniFileSetting.Read(Name), "Switched back on, the next start sealed it again.");
            StringAssert.StartsWith("HMK1:", StoredTotp(account.ID));
            ClassicAssert.AreEqual(other, _application.Settings.GetIniSetting(Name));
         }
         finally
         {
            PutBackStoredSetting(Name, before);

            if (string.IsNullOrEmpty(protectionBefore))
               IniFileSetting.Delete("ProtectStoredSecretsWithDPAPI");
            else
               IniFileSetting.Write("ProtectStoredSecretsWithDPAPI", protectionBefore);
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A metrics bearer token kept under the secret store still guards the listener: refused without it, answered with it.")]
      public void AProtectedMetricsTokenStillGuardsTheListener()
      {
         RequireProtectedStore();

         string token = "sar" + Guid.NewGuid().ToString("N");
         var before = MetricsSettings.ToDictionary(name => name, ReadStoredSetting);

         try
         {
            ConfigureMetrics(MetricsPort, token);
            _application.Reinitialize();

            StringAssert.StartsWith("HMK1:", IniFileSetting.Read("MetricsServerAuthToken"), "The token is sealed.");

            ClassicAssert.AreEqual(401, MetricsStatus(null), "No token: refused.");
            ClassicAssert.AreEqual(401, MetricsStatus("Bearer not-" + token), "Another token: refused.");
            ClassicAssert.AreEqual(200, MetricsStatus("Bearer " + token), "The token, as the listener read it opened: answered.");
         }
         finally
         {
            foreach (KeyValuePair<string, StoredSetting> entry in before)
               PutBackStoredSetting(entry.Key, entry.Value);
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------- the stored-secrets key

      [Test]
      [Description("The stored-secrets key moves between nodes as the verbs carry it. --export-stored-secrets-key writes it to a new file readable by SYSTEM and the administrators only, never over a file that exists and never to the output, and records the export, which turns the setup checklist's stored_secrets_key item from not_exported to exported. A node without the key - this one, with its [Security] StoredSecretsKey line taken out - makes none while the database records one, says so (HM7495) and refuses every code of a sealed second factor; --import-stored-secrets-key makes the key its own and the code verifies again. A key file whose id does not match its key, and a key the database does not record, are refused.")]
      public void TheKeyIsCarriedToANodeWithoutItByExportAndImport()
      {
         RequireProtectedStore();

         string address = "totp-standby@" + _domain.Name;
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         string secret = Enrol(account);
         StringAssert.StartsWith("HMK1:", StoredTotp(account.ID));

         string keyLine = IniFileSetting.Read("Security", "StoredSecretsKey");
         StringAssert.StartsWith("DPAPI:", keyLine, "The verb made the key under this machine's DPAPI.");

         string directory = Paths.Combine(Path.GetTempPath(), TestSetup.UniqueString());
         Directory.CreateDirectory(directory);
         string keyFile = Paths.Combine(directory, "stored-secrets.key");

         try
         {
            // Not exported yet, as far as the checklist knows.
            ClassicAssert.IsTrue(TryExecute("delete from hm_settings where settingname = 'StoredSecretsKeyExported'"));
            ClassicAssert.AreEqual("stored_secrets_key_not_exported", KeyChecklistCode(),
               "The checklist asks for an export until one has been recorded.");

            Verb exported = RunServerVerb("--export-stored-secrets-key \"" + keyFile + "\"", null, null);
            ClassicAssert.AreEqual(0, exported.ExitCode, exported.Output);

            string[] lines = File.ReadAllLines(keyFile);
            ClassicAssert.AreEqual(3, lines.Length, "Three lines: the format, the id and the key.");
            ClassicAssert.AreEqual("hMailServer stored secrets key", lines[0]);
            ClassicAssert.AreEqual("id " + RecordedKeyId(), lines[1], "The file names the key the database records.");
            StringAssert.Contains(RecordedKeyId(), exported.Output, "The verb names the key by its id.");
            StringAssert.DoesNotContain(lines[2].Substring(4), exported.Output, "The verb never prints the key.");
            ClassicAssert.AreEqual("stored_secrets_key_exported", KeyChecklistCode(), "The export is recorded, and the checklist item is done.");

            if (new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
            {
               FileSecurity security = new FileInfo(keyFile).GetAccessControl();
               ClassicAssert.IsTrue(security.AreAccessRulesProtected, "The key file inherits its directory's permissions.");

               var allowed = new[]
               {
                  new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                  new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
                  WindowsIdentity.GetCurrent().User,
               };
               foreach (FileSystemAccessRule rule in security.GetAccessRules(true, true, typeof(SecurityIdentifier)))
               {
                  ClassicAssert.IsTrue(allowed.Any(sid => sid.Equals(rule.IdentityReference)),
                     "The key file grants " + rule.IdentityReference + " " + rule.FileSystemRights);
               }
            }

            Verb again = RunServerVerb("--export-stored-secrets-key \"" + keyFile + "\"", null, null);
            ClassicAssert.AreEqual(2, again.ExitCode, "A file that exists is not written over: " + again.Output);

            // The node without the key.
            IniFileSetting.DeleteFileOnly("Security", "StoredSecretsKey");
            _application.Reinitialize();

            ClassicAssert.AreEqual(string.Empty, IniFileSetting.Read("Security", "StoredSecretsKey"),
               "A node without the key made one of its own - a start never makes one, and the database records another.");
            ClassicAssert.IsTrue(_application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(address).TOTPEnabled,
               "The second factor stays enrolled where its secret does not open - never switched off.");
            ClassicAssert.IsFalse(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)),
               "Without the key every code is refused.");
            CustomAsserts.AssertReportedError("7495", "--import-stored-secrets-key");

            // A damaged file: the id line does not name the key.
            string damaged = Paths.Combine(directory, "damaged.key");
            // settings-store-exempt: a stored-secrets key file in a temporary directory, for the import verb; it is not hMailServer.ini.
            File.WriteAllLines(damaged, new[] { lines[0], "id 0000000000000000", lines[2] });
            Verb refused = RunServerVerb("--import-stored-secrets-key \"" + damaged + "\"", null, null);
            ClassicAssert.AreEqual(1, refused.ExitCode, refused.Output);
            StringAssert.Contains("damaged", refused.Output);

            Verb imported = RunServerVerb("--import-stored-secrets-key \"" + keyFile + "\"", null, null);
            ClassicAssert.AreEqual(0, imported.ExitCode, imported.Output);
            StringAssert.DoesNotContain(lines[2].Substring(4), imported.Output, "The verb never prints the key.");
            StringAssert.StartsWith("DPAPI:", IniFileSetting.Read("Security", "StoredSecretsKey"), "Imported under this machine's own store.");

            _application.Reinitialize();
            ClassicAssert.IsTrue(AuthenticatesWithCode(address, Password, OneTimeCodes.Fresh(secret)), "With the key imported the code verifies again.");

            // A key the database does not record is refused, and the key held is not touched.
            byte[] otherKey = new byte[32];
            using (RandomNumberGenerator generator = RandomNumberGenerator.Create())
               generator.GetBytes(otherKey);
            string otherFile = Paths.Combine(directory, "other.key");
            // settings-store-exempt: a stored-secrets key file in a temporary directory, for the import verb; it is not hMailServer.ini.
            File.WriteAllLines(otherFile, new[] { lines[0], "id " + KeyIdOf(otherKey), "key " + Convert.ToBase64String(otherKey) });
            string held = IniFileSetting.Read("Security", "StoredSecretsKey");
            Verb other = RunServerVerb("--import-stored-secrets-key \"" + otherFile + "\"", null, null);
            ClassicAssert.AreEqual(1, other.ExitCode, other.Output);
            StringAssert.Contains("the database records " + RecordedKeyId(), other.Output);
            ClassicAssert.AreEqual(held, IniFileSetting.Read("Security", "StoredSecretsKey"), "The key held is left as it was.");
         }
         finally
         {
            IniFileSetting.WriteFileOnly("Security", "StoredSecretsKey", keyLine);
            _application.Reinitialize();
            try { Directory.Delete(directory, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
         }
      }

      [Test]
      [Description("The key is recorded in the database as it is made, and the database is the arbiter: every envelope names the recorded key; --create-stored-secrets-key on the node that holds it changes nothing; on a node without it - its line taken out - it is refused, naming the import, and neither a line nor a second record is made.")]
      public void TheKeyIsRecordedInTheDatabaseAndASecondIsRefused()
      {
         RequireProtectedStore();

         string recorded = RecordedKeyId();
         StringAssert.IsMatch("^[0-9a-f]{16}$", recorded, "The database records the key by its id.");

         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "totp-recorded@" + _domain.Name, Password);
         Enrol(account);
         StringAssert.StartsWith("HMK1:" + recorded + ":", StoredTotp(account.ID), "A new envelope names the recorded key.");

         Verb again = RunServerVerb("--create-stored-secrets-key", null, null);
         ClassicAssert.AreEqual(0, again.ExitCode, again.Output);
         StringAssert.Contains("already holds the stored-secrets key " + recorded, again.Output);

         string keyLine = IniFileSetting.Read("Security", "StoredSecretsKey");
         try
         {
            // The node without the key. The running service keeps what it loaded; only
            // the verb, which reads the file, sees the line gone.
            IniFileSetting.DeleteFileOnly("Security", "StoredSecretsKey");

            Verb second = RunServerVerb("--create-stored-secrets-key", null, null);
            ClassicAssert.AreEqual(1, second.ExitCode, second.Output);
            StringAssert.Contains(recorded, second.Output, "The refusal names the database's key.");
            StringAssert.Contains("--import-stored-secrets-key", second.Output, "...and the import.");

            ClassicAssert.AreEqual(string.Empty, IniFileSetting.Read("Security", "StoredSecretsKey"), "A second key was written into the file.");
            ClassicAssert.AreEqual(recorded, RecordedKeyId(), "The database's record was changed.");
         }
         finally
         {
            IniFileSetting.WriteFileOnly("Security", "StoredSecretsKey", keyLine);
         }
      }

      [Test]
      [Description("--forget-sealed-secrets KEYID, with the service stopped, clears what was sealed under the lost key it names - a second factor and a [Settings] secret - and nothing else: the account enrols again, the setting reads as not set, a second factor sealed under the server's own key still verifies, and the database's record, which names that key, is kept. Every item is printed, and said again in one HM7497 entry - on the verb's standard error on Windows, where an administrator's command writes no log, and in the error log on Linux. Refused while the service runs (exit 4), without a key id (exit 2), and for the key this server holds (exit 1).")]
      public void ForgettingSealedSecretsClearsOnlyWhatNoKeyHereOpens()
      {
         RequireProtectedStore();

         if (TestSetup.ConsoleServer)
            Assert.Ignore("The server under test is a console server, which the verb's service check cannot see stopped.");

         const string LostKeyId = "00000000000000ff";

         Verb running = RunServerVerb("--forget-sealed-secrets " + LostKeyId, null, null);
         ClassicAssert.AreEqual(4, running.ExitCode, running.Output);
         StringAssert.Contains("service is running", running.Output);

         string domainName = _domain.Name;
         string recorded = RecordedKeyId();

         string keptAddress = "totp-kept@" + domainName;
         Account kept = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, keptAddress, Password);
         string keptSecret = Enrol(kept);

         // Sealed under a key nobody holds: the id is not this server's, and the body
         // opens under no key.
         byte[] body = new byte[44];
         using (RandomNumberGenerator generator = RandomNumberGenerator.Create())
            generator.GetBytes(body);
         string lostEnvelope = "HMK1:" + LostKeyId + ":" + Convert.ToBase64String(body);

         string lostAddress = "totp-lost@" + domainName;
         Account lost = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, lostAddress, Password);
         Enrol(lost);
         int lostId = lost.ID;
         ClassicAssert.IsTrue(TryExecute("update hm_accounts set accounttotpsecret = '" + lostEnvelope + "' where accountid = " + lostId));

         const string Name = "OutboundOAuth2FixedToken";
         StoredSetting before = ReadStoredSetting(Name);
         IniFileSetting.Write(Name, "sar-forget-placeholder");
         ClassicAssert.IsTrue(TryExecute("update hm_inisettings set inisettingvalue = '" + lostEnvelope + "', inisettingfilevalue = '" + lostEnvelope + "' where inisettingname = '" + Name + "'"));

         // HM7497, the one entry naming every item, from the verb's own process: on
         // Windows on its standard error, since an administrator's command writes
         // nothing to the logs (hMailServer.cpp, ForbidServiceFolderWrites); on Linux,
         // run as the service's user, in the error log.
         string forgotOutput = null;

         try
         {
            ServerExecutable();
            StopService();
            try
            {
               // No key id: nothing can be told apart, so nothing is touched.
               Verb noId = RunServerVerb("--forget-sealed-secrets", null, null);
               ClassicAssert.AreEqual(2, noId.ExitCode, noId.Output);
               StringAssert.Contains("needs the lost key's id", noId.Output);

               // The key this server holds is not lost: carried, never forgotten.
               Verb held = RunServerVerb("--forget-sealed-secrets " + recorded, null, null);
               ClassicAssert.AreEqual(1, held.ExitCode, held.Output);
               StringAssert.Contains("this node holds the stored-secrets key " + recorded, held.Output);

               Verb forgot = RunServerVerb("--forget-sealed-secrets " + LostKeyId, null, null);
               ClassicAssert.AreEqual(0, forgot.ExitCode, forgot.Output);
               StringAssert.Contains(lostAddress + ": second factor removed", forgot.Output);
               StringAssert.Contains("[Settings] " + Name + ": emptied", forgot.Output);
               StringAssert.DoesNotContain(keptAddress, forgot.Output, "A second factor the server's own key opens was touched.");
               StringAssert.DoesNotContain("record of the stored-secrets key", forgot.Output, "The database's record of the key this node holds was touched.");
               forgotOutput = forgot.Output;
            }
            finally
            {
               RestartServerAndReacquireCom();
            }

            if (Environment.OSVersion.Platform == PlatformID.Win32NT)
            {
               string entry = forgotOutput.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                  .FirstOrDefault(line => line.StartsWith("HM7497: ", StringComparison.Ordinal));
               ClassicAssert.IsNotNull(entry, "--forget-sealed-secrets said no HM7497 on its output: " + forgotOutput);
               StringAssert.Contains(lostAddress, entry);
               StringAssert.Contains(Name, entry);
            }
            else
               CustomAsserts.AssertReportedError("7497", lostAddress, Name);

            ClassicAssert.AreEqual(string.Empty, StoredTotp(lostId), "The lost second factor is cleared.");
            ClassicAssert.IsFalse(_application.Domains.get_ItemByName(domainName).Accounts.get_ItemByAddress(lostAddress).TOTPEnabled,
               "The account has no second factor now, and enrols again.");

            string row;
            ClassicAssert.IsTrue(TryStoredSettingRow(Name, out row));
            ClassicAssert.AreEqual(string.Empty, row, "The lost setting is emptied in the row.");
            ClassicAssert.AreEqual(string.Empty, _application.Settings.GetIniSetting(Name), "...and reads as not set.");

            ClassicAssert.IsTrue(AuthenticatesWithCode(keptAddress, Password, OneTimeCodes.Fresh(keptSecret)), "The second factor the key opens still verifies.");
            ClassicAssert.AreEqual(recorded, RecordedKeyId(), "The database still records this server's key.");
         }
         finally
         {
            PutBackStoredSetting(Name, before);
         }
      }

      // ------------------------------------------------------------- helpers: the administrator line

      private static string AdministratorLine()
      {
         return IniFileSetting.Read("Security", "AdministratorPassword");
      }

      private void PutBackAdministratorLine(string before)
      {
         if (string.IsNullOrEmpty(before))
            IniFileSetting.DeleteFileOnly("Security", "AdministratorPassword");
         else
            IniFileSetting.WriteFileOnly("Security", "AdministratorPassword", before);

         _application.Reinitialize();
      }

      private static string Md5Hex(string text)
      {
         using (MD5 md5 = MD5.Create())
         {
            byte[] digest = md5.ComputeHash(Encoding.UTF8.GetBytes(text));
            return string.Concat(digest.Select(b => b.ToString("x2")));
         }
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private sealed class Verb
      {
         public int ExitCode;
         public string Output;
      }

      private Verb RunSetAdminPassword(string input, string environmentPassword)
      {
         return RunServerVerb("--set-admin-password", input, environmentPassword);
      }

      /// <summary>
      ///    hMailServer.exe with these arguments beside the service, with the given
      ///    standard input (closed at once when null) and HM_ADMIN_PASSWORD set to the
      ///    given value, or absent when null. Standard input is always a pipe the test
      ///    closes, so a verb can never wait on one inherited from the runner.
      /// </summary>
      private Verb RunServerVerb(string arguments, string input, string environmentPassword)
      {
         var start = new ProcessStartInfo(ServerExecutable(), arguments)
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };

         foreach (string name in start.EnvironmentVariables.Keys.Cast<string>().ToList())
         {
            if (name.StartsWith("HM_", StringComparison.OrdinalIgnoreCase))
               start.EnvironmentVariables.Remove(name);
         }

         if (environmentPassword != null)
            start.EnvironmentVariables["HM_ADMIN_PASSWORD"] = environmentPassword;

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();

            // The bytes themselves, so no encoder puts a preamble in front of them. A
            // verb that took the password from the environment reads none of it and
            // may have finished already, closing the pipe under the write.
            try
            {
               if (input != null)
               {
                  byte[] bytes = Encoding.UTF8.GetBytes(input);
                  process.StandardInput.BaseStream.Write(bytes, 0, bytes.Length);
                  process.StandardInput.BaseStream.Flush();
               }
               process.StandardInput.Close();
            }
            catch (IOException) when (environmentPassword != null)
            {
               // The pipe was closed by a verb that never needed it.
            }

            if (!process.WaitForExit(120 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
                  // It finished between the wait and the kill.
               }

               Assert.Fail(arguments + " did not finish within two minutes, and was stopped.");
            }
            process.WaitForExit();

            return new Verb { ExitCode = process.ExitCode, Output = output.Result + error.Result };
         }
      }

      // Found once, while the service runs: the verbs that need it stopped ask for
      // it after it has gone, when COM has nobody to answer.
      private static string serverExecutable_;

      /// <summary>The hMailServer.exe the service runs, from the program directory it reports.</summary>
      private string ServerExecutable()
      {
         if (serverExecutable_ != null)
            return serverExecutable_;

         string program = _settings.Directories.ProgramDirectory;
         foreach (string candidate in new[] { Path.Combine(program, "hMailServer.exe"), Path.Combine(program, "Bin", "hMailServer.exe") })
         {
            if (File.Exists(candidate))
            {
               serverExecutable_ = candidate;
               return candidate;
            }
         }

         Assert.Ignore("hMailServer.exe is not in the program directory the server reports (" + program + "), so the verb cannot be run here.");
         return null;
      }

      // ------------------------------------------------------------- helpers: the second factor

      private static string Enrol(Account account)
      {
         string uri = account.EnrolTOTP();
         account.Save();

         Match secret = Regex.Match(uri, "secret=([A-Z2-7]+)");
         ClassicAssert.IsTrue(secret.Success, "No base32 secret in the URI. Got: " + uri);
         return secret.Groups[1].Value;
      }

      private static bool AuthenticatesWithCode(string address, string password, string code)
      {
         return new hMailServer.Application().AuthenticateWithCode(address, password, code) != null;
      }

      private static string NewBase32Secret()
      {
         const string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
         byte[] random = new byte[32];
         using (RandomNumberGenerator generator = RandomNumberGenerator.Create())
            generator.GetBytes(random);
         return new string(random.Select(b => alphabet[b % 32]).ToArray());
      }

      /// <summary>
      ///    RFC 6238 in the test, with .NET's HMACSHA1 - what an authenticator app
      ///    computes, and nothing the server owns (as TwoFactorAuthentication does).
      ///    For a code the server is to TAKE, the tests ask OneTimeCodes.Fresh
      ///    instead: a code is taken once (SecondFactorGuard), so a second sign-in
      ///    with the same secret needs the authenticator's next code.
      /// </summary>
      private static string CurrentCode(string base32Secret, int stepOffset = 0)
      {
         byte[] key = Base32Decode(base32Secret);

         long counter = DateTimeOffset.UtcNow.ToUnixTimeSeconds() / 30 + stepOffset;

         byte[] counterBytes = BitConverter.GetBytes(counter);
         if (BitConverter.IsLittleEndian)
            Array.Reverse(counterBytes);

         byte[] hash;
         using (var hmac = new HMACSHA1(key))
            hash = hmac.ComputeHash(counterBytes);

         int offset = hash[hash.Length - 1] & 0x0F;

         int binary =
            ((hash[offset] & 0x7F) << 24) |
            ((hash[offset + 1] & 0xFF) << 16) |
            ((hash[offset + 2] & 0xFF) << 8) |
            (hash[offset + 3] & 0xFF);

         return (binary % 1000000).ToString("000000");
      }

      private static byte[] Base32Decode(string input)
      {
         const string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

         var output = new List<byte>();
         int buffer = 0, bitsLeft = 0;

         foreach (int index in input.Select(c => alphabet.IndexOf(char.ToUpperInvariant(c))).Where(index => index >= 0))
         {
            buffer = (buffer << 5) | index;
            bitsLeft += 5;

            if (bitsLeft >= 8)
            {
               bitsLeft -= 8;
               output.Add((byte) ((buffer >> bitsLeft) & 0xFF));
            }
         }

         return output.ToArray();
      }

      /// <summary>The first 16 hex digits of SHA-256 over the label and the key: the id StoredSecrets names a key by.</summary>
      private static string KeyIdOf(byte[] key)
      {
         byte[] label = Encoding.ASCII.GetBytes("hMailServer stored secrets key id\n");
         using (SHA256 sha = SHA256.Create())
         {
            byte[] digest = sha.ComputeHash(label.Concat(key).ToArray());
            return string.Concat(digest.Take(8).Select(b => b.ToString("x2")));
         }
      }

      /// <summary>The key id the database records as the installation's (hm_settings, StoredSecretsKeyId); empty when it records none.</summary>
      private static string RecordedKeyId()
      {
         object value;
         ClassicAssert.IsTrue(TryScalar("select max(settingstring) from hm_settings where settingname = 'StoredSecretsKeyId'", out value),
            "The database's record of the key could not be read.");
         return Convert.ToString(value).Trim();
      }

      /// <summary>The first finding's code of the setup checklist's stored_secrets_key item, as COM answers it.</summary>
      private string KeyChecklistCode()
      {
         string json = (string) _application.Utilities.GetSetupChecklist(false);
         using (JsonDocument document = JsonDocument.Parse(json))
         {
            foreach (JsonElement item in document.RootElement.GetProperty("items").EnumerateArray())
            {
               if (item.GetProperty("key").GetString() == "stored_secrets_key")
                  return item.GetProperty("findings")[0].GetProperty("code").GetString();
            }
         }

         Assert.Fail("The checklist has no stored_secrets_key item: " + json);
         return null;
      }

      /// <summary>
      ///    The error log deleted when every entry in it is one of these codes: an error
      ///    said once a process, which this test may or may not be the first to provoke.
      ///    Anything else is left for the fixture's own check to report.
      /// </summary>
      private static void ClearErrorLogOf(params string[] codes)
      {
         // Read without a lock, as LogHandler reads it, and without its assertion that
         // the file exists: no entry at all is the other answer this allows.
         string file = LogHandler.GetErrorLogFileName();
         if (!File.Exists(file))
            return;

         string log;
         using (var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
         using (var reader = new StreamReader(stream))
            log = reader.ReadToEnd();

         string[] entries = log.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
         if (entries.All(entry => codes.Any(code => entry.Contains(code))))
            LogHandler.DeleteErrorLog();
      }

      /// <summary>The service stopped, and its process gone, as RestApiProfiles stops it for its verbs.</summary>
      private static void StopService()
      {
         using (Process process = Process.Start(new ProcessStartInfo("net", "stop hMailServer")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
         }))
         {
            process.StandardOutput.ReadToEnd();
            process.StandardError.ReadToEnd();
            process.WaitForExit(90000);
         }

         DateTime deadline = DateTime.UtcNow.AddSeconds(60);
         while (DateTime.UtcNow < deadline && Process.GetProcessesByName("hmailserver").Length > 0)
            Thread.Sleep(250);

         using (var controller = new System.ServiceProcess.ServiceController("hMailServer"))
         {
            controller.Refresh();
            ClassicAssert.AreEqual(System.ServiceProcess.ServiceControllerStatus.Stopped, controller.Status, "The hMailServer service did not stop.");
         }
      }

      private static string StoredTotp(int accountId)
      {
         object value;
         ClassicAssert.IsTrue(TryScalar("select accounttotpsecret from hm_accounts where accountid = " + accountId, out value),
            "The account's row could not be read.");
         return Convert.ToString(value);
      }

      // ------------------------------------------------------------- helpers: the [Settings] store

      private sealed class StoredSetting
      {
         public bool Present;
         public string Line;
      }

      /// <summary>
      ///    A key as hMailServer.ini holds it - the server's copy of the store - and
      ///    whether it has a line at all, to be put back exactly: an envelope written
      ///    back is stored as it is.
      /// </summary>
      private static StoredSetting ReadStoredSetting(string name)
      {
         string file = IniFileSetting.ExistingIniFiles().First();
         string text = File.ReadAllText(file);
         bool present = Regex.IsMatch(text, @"(?mi)^\s*" + Regex.Escape(name) + @"\s*=");
         return new StoredSetting { Present = present, Line = IniFileSetting.Read(name) };
      }

      private static void PutBackStoredSetting(string name, StoredSetting before)
      {
         if (before.Present)
            IniFileSetting.Write(name, before.Line);
         else
            IniFileSetting.Delete(name);
      }

      private static bool TryStoredSettingRow(string name, out string row)
      {
         row = null;
         object value;
         if (!TryScalar("select inisettingvalue from hm_inisettings where inisettingname = '" + name + "'", out value))
            return false;
         row = Convert.ToString(value);
         return true;
      }

      private static void ConfigureMetrics(int port, string token)
      {
         IniFileSetting.Write("MetricsServerBindAddress", "127.0.0.1");
         IniFileSetting.Write("MetricsServerPort", port.ToString());
         IniFileSetting.Write("MetricsServerAuthToken", token);
         IniFileSetting.Write("MetricsServerAuthUsername", "");
         IniFileSetting.Write("MetricsServerAuthPassword", "");
         IniFileSetting.Write("MetricsServerCertificateFile", "");
         IniFileSetting.Write("MetricsServerPrivateKeyFile", "");
      }

      /// <summary>GET /metrics on this fixture's port, with the Authorization header given (none when null), and the status it answered.</summary>
      private static int MetricsStatus(string authorization)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", MetricsPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            client.ReceiveTimeout = 20000;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               string request = "GET /metrics HTTP/1.0\r\nHost: 127.0.0.1\r\n" +
                                (authorization == null ? "" : "Authorization: " + authorization + "\r\n") +
                                "Connection: close\r\n\r\n";
               byte[] bytes = Encoding.ASCII.GetBytes(request);
               stream.Write(bytes, 0, bytes.Length);
               stream.Flush();

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               Match status = Regex.Match(raw, @"^HTTP/\d\.\d\s+(\d{3})");
               ClassicAssert.IsTrue(status.Success, "No status line from the metrics listener: " + raw);
               return int.Parse(status.Groups[1].Value);
            }
         }
      }

      // ------------------------------------------------------------- helpers: the database probes

      /// <summary>
      ///    The probes read the SQL CE test database, and the envelopes they look for
      ///    are the ones ProtectStoredSecretsWithDPAPI 1 - the default - makes, under
      ///    the installation's key. The service never makes one: where the bench has
      ///    none, the verb does, as an administrator turning sealing on after an
      ///    upgrade would, and the service takes it at a reinitialise.
      /// </summary>
      private void RequireProtectedStore()
      {
         if (!IsCeDatabase())
            Assert.Inconclusive("The stored values are read from the SQL CE test database; this server runs another.");

         if (_settings.GetIniSetting("ProtectStoredSecretsWithDPAPI") == "0")
            Assert.Inconclusive("ProtectStoredSecretsWithDPAPI is 0 on this server, so secrets are stored as written by design.");

         if (string.IsNullOrEmpty(IniFileSetting.Read("Security", "StoredSecretsKey")))
         {
            Verb made = RunServerVerb("--create-stored-secrets-key", null, null);
            if (made.ExitCode != 0)
               Assert.Inconclusive("This server holds no stored-secrets key and --create-stored-secrets-key did not make one: " + made.Output);
            StringAssert.Contains("Export it NOW", made.Output, "Making the key says to export it at once.");
            _application.Reinitialize();
         }

         if (string.IsNullOrEmpty(RecordedKeyId()))
            _application.Reinitialize();
         ClassicAssert.IsNotEmpty(RecordedKeyId(), "The database records no stored-secrets key although this server holds one; its start records it.");
      }

      /// <summary>The ini's Type is MSSQLCE: the test database the probes can reach.</summary>
      private static bool IsCeDatabase()
      {
         string ini = IniFileSetting.ExistingIniFiles().FirstOrDefault();
         if (ini == null)
            return false;
         string type = Regex.Match(File.ReadAllText(ini), @"(?m)^\s*Type\s*=\s*(\S+)").Groups[1].Value;
         return string.Equals(type, "MSSQLCE", StringComparison.OrdinalIgnoreCase);
      }

      /// <summary>
      ///    An open ADODB connection to the SQL CE test database, as
      ///    RestApiSmimeAuthority reads it, or null when the server runs another.
      /// </summary>
      private static object OpenCe(out Type connectionType)
      {
         connectionType = null;
         if (!IsCeDatabase())
            return null;

         string text = File.ReadAllText(IniFileSetting.ExistingIniFiles().First());
         string folder = Regex.Match(text, @"(?m)^\s*DatabaseFolder\s*=\s*(.+?)\s*$").Groups[1].Value;
         string database = Regex.Match(text, @"(?m)^\s*Database\s*=\s*(.+?)\s*$").Groups[1].Value;
         string encoded = Regex.Match(text, @"(?m)^\s*Password\s*=\s*(.+?)\s*$").Groups[1].Value;
         string password = Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(encoded), null, DataProtectionScope.LocalMachine));
         string connectionString = "Provider=Microsoft.SQLSERVER.CE.OLEDB.4.0;Data Source=" + Paths.Combine(folder, database + ".sdf") +
                                   ";SSCE:Database Password=" + password;

         connectionType = Type.GetTypeFromProgID("ADODB.Connection", true);
         object connection = Activator.CreateInstance(connectionType);
         connectionType.InvokeMember("Open", BindingFlags.InvokeMethod, null, connection, new object[] { connectionString });
         return connection;
      }

      /// <summary>The first column of the first row of a query, or false when the probe cannot reach the database.</summary>
      private static bool TryScalar(string sql, out object value)
      {
         value = null;
         Type connectionType;
         object connection = OpenCe(out connectionType);
         if (connection == null)
            return false;

         try
         {
            object recordset = connectionType.InvokeMember("Execute", BindingFlags.InvokeMethod, null, connection, new object[] { sql });
            Type recordsetType = recordset.GetType();
            object fields = recordsetType.InvokeMember("Fields", BindingFlags.GetProperty, null, recordset, null);
            object field = fields.GetType().InvokeMember("Item", BindingFlags.GetProperty, null, fields, new object[] { 0 });
            value = field.GetType().InvokeMember("Value", BindingFlags.GetProperty, null, field, null);
            recordsetType.InvokeMember("Close", BindingFlags.InvokeMethod, null, recordset, null);
            return true;
         }
         finally
         {
            connectionType.InvokeMember("Close", BindingFlags.InvokeMethod, null, connection, null);
         }
      }

      /// <summary>A statement that changes rows under the server, or false when the probe cannot reach the database.</summary>
      private static bool TryExecute(string sql)
      {
         Type connectionType;
         object connection = OpenCe(out connectionType);
         if (connection == null)
            return false;

         try
         {
            connectionType.InvokeMember("Execute", BindingFlags.InvokeMethod, null, connection, new object[] { sql });
            return true;
         }
         finally
         {
            connectionType.InvokeMember("Close", BindingFlags.InvokeMethod, null, connection, null);
         }
      }

      // ------------------------------------------------------------- helpers: the backup

      private void RunBackup(string directory)
      {
         var settings = _application.Settings.Backup;
         settings.BackupDomains = true;
         settings.BackupMessages = false;
         settings.BackupSettings = true;
         settings.CompressDestinationFiles = true;
         settings.Destination = directory;

         CustomAsserts.AssertDeleteFile(settings.LogFile);

         _application.BackupManager.StartBackup();

         for (int attempt = 0; attempt < 240; attempt++)
         {
            try
            {
               string log = TestSetup.ReadExistingTextFile(settings.LogFile);

               if (log.Contains("BACKUP ERROR:"))
                  Assert.Fail("The backup failed. Log: " + log);

               if (log.Contains("Backup completed successfully"))
                  return;
            }
            catch (Exception sharing) when (!ExceptionPolicy.IsFatal(sharing))
            {
               // The server is writing the file; read it again in a moment.
            }

            Thread.Sleep(250);
         }

         Assert.Fail("The backup did not finish within a minute.");
      }

      private void DeleteAndRestore(string directory)
      {
         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         string startTime = _application.Status.StartTime;

         // Status.StartTime has one-second resolution; step past the second.
         Thread.Sleep(1100);

         FileInfo[] archives = new DirectoryInfo(directory).GetFiles("*.7z");
         ClassicAssert.AreEqual(1, archives.Length, "Expected exactly one archive in " + directory);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = true;
         backup.RestoreMessages = false;
         backup.RestoreSettings = true;
         backup.StartRestore();

         for (int attempt = 0; attempt < 600; attempt++)
         {
            try
            {
               string now = _application.Status.StartTime;
               if (now.Length > 0 && now != startTime)
               {
                  // The listeners come back with the reinitialisation.
                  Thread.Sleep(500);
                  return;
               }
            }
            catch (Exception talking) when (!ExceptionPolicy.IsFatal(talking))
            {
               // The COM server is reinitialising.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not finish within a minute.");
      }
   }
}
