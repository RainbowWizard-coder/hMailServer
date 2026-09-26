// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Security.Cryptography;
using System.Text;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    hMailServer has always claimed to migrate legacy account passwords to the
   ///    preferred hash scheme, and it never did. PersistentAccount::ReadObject re-hashed
   ///    an unencrypted stored password into the in-memory Account object and nothing
   ///    wrote it back, so accountpwencryption kept its legacy value for the life of the
   ///    installation: the clear-text password stayed in the database row, readable by
   ///    anyone with a copy of it, no matter how many times the user logged in.
   ///
   ///    The upgrade now happens at the one moment it can: immediately after the
   ///    clear-text password has been verified against the account. These tests pin both
   ///    halves of that - that the row really changes, and that it only changes on a
   ///    successful logon - plus the ordering bug it exposed: MinimumAcceptedHashAlgorithm
   ///    used to be enforced against the stored hash type *before* the password was
   ///    compared, which refused the correct password of a legacy account and so made the
   ///    setting a permanent lockout, since the upgrade that would satisfy it can only
   ///    ever happen on a successful logon.
   ///
   ///    Reading the stored hash type back is awkward on purpose: the COM Database
   ///    interface can execute SQL but cannot return rows, and the account's hash type is
   ///    not exposed through the API. The tests copy the value into accountmaxsize, which
   ///    is exposed as Account.MaxSize, read it through the API and put the column back.
   /// </summary>
   [TestFixture]
   public class PasswordHashUpgrade : TestFixtureBase
   {

      // Crypt::EncryptionType
      private const int CryptNone = 0;
      private const int CryptMd5 = 2;
      private const int CryptPbkdf2 = 4;

      private const string Password = "SeC-r3t Pass!";

      /// <summary>
      ///    Rewrites the account row the way an older hMailServer stored it. Goes straight
      ///    to the database because the API deliberately offers no way to store a password
      ///    under a legacy scheme.
      /// </summary>
      private void ForceStoredPassword(Account account, string storedPassword, int encryptionType)
      {
         string sql = string.Format(
            "update hm_accounts set accountpassword = '{0}', accountpwencryption = {1} where accountid = {2}",
            TestSetup.Escape(storedPassword), encryptionType, account.ID);

         _application.Database.ExecuteSQL(sql);

         // The server caches Account objects; drop them so the next logon reads the row
         // that was just written.
         _settings.Cache.Clear();
      }

      /// <summary>
      ///    Reads accountmaxsize back through the API and resets it to 0. Used as a channel
      ///    for the columns the API does not expose - see the fixture comment.
      /// </summary>
      private int ReadProbe(Account account)
      {
         _settings.Cache.Clear();

         int value = _domain.Accounts.get_ItemByAddress(account.Address).MaxSize;

         _application.Database.ExecuteSQL("update hm_accounts set accountmaxsize = 0 where accountid = " + account.ID);
         _settings.Cache.Clear();

         return value;
      }

      private int ReadStoredEncryptionType(Account account)
      {
         _application.Database.ExecuteSQL(
            "update hm_accounts set accountmaxsize = accountpwencryption where accountid = " + account.ID);

         return ReadProbe(account);
      }

      private bool StoredPasswordIs(Account account, string value)
      {
         _application.Database.ExecuteSQL("update hm_accounts set accountmaxsize = 0 where accountid = " + account.ID);
         _application.Database.ExecuteSQL(string.Format(
            "update hm_accounts set accountmaxsize = 1 where accountid = {0} and accountpassword = '{1}'",
            account.ID, TestSetup.Escape(value)));

         return ReadProbe(account) == 1;
      }

      /// <summary>
      ///    The legacy MD5 scheme: lowercase hex of the unsalted digest, which is what
      ///    Crypt::EnCrypt(ETMD5) produces via HashCreator::GenerateHashNoSalt.
      /// </summary>
      private static string LegacyMd5Hash(string password)
      {
         using (var md5 = MD5.Create())
         {
            byte[] digest = md5.ComputeHash(Encoding.ASCII.GetBytes(password));

            var hex = new StringBuilder(digest.Length * 2);
            foreach (byte b in digest)
               hex.Append(b.ToString("x2"));

            return hex.ToString();
         }
      }

      private void SetMinimumAcceptedHashAlgorithm(int value)
      {
         // Stored through the settings store, the door the Control Panel and the REST API
         // use: from schema 6042 a value edited into hMailServer.ini is put back at the
         // next start and reported as HM5804.
         IniFileSetting.Write("MinimumAcceptedHashAlgorithm", value.ToString());

         // The setting is cached in IniFileSettings at startup; Reinitialize reloads it.
         _application.Reinitialize();
      }

      /// <summary>
      /// A POP3 logon that hands the mailbox back afterwards.
      ///
      /// POP3 locks the mailbox for the lifetime of a session, so logging on a second
      /// time while the first connection is still open is refused with
      /// "-ERR Your mailbox is already locked". That is indistinguishable, from the
      /// caller's point of view, from the password having been rejected - which is
      /// exactly the wrong conclusion to draw in a test whose whole purpose is to prove
      /// that the password still works after the stored hash was rewritten. This
      /// fixture logs on repeatedly, so every successful logon has to disconnect.
      /// </summary>
      private static bool LogonAndRelease(string address, string password)
      {
         var client = new Pop3ClientSimulator();
         bool loggedOn = client.ConnectAndLogon(address, password);

         if (loggedOn)
            client.Disconnect();

         return loggedOn;
      }

      private static bool LogonAndRelease(string address, string password, out string errorMessage)
      {
         var client = new Pop3ClientSimulator();
         bool loggedOn = client.ConnectAndLogon(address, password, out errorMessage);

         if (loggedOn)
            client.Disconnect();

         return loggedOn;
      }

      /// <summary>
      ///    A PBKDF2 hash as every version before 6.3.4 made it of a Latin-1 password: over
      ///    its characters as bytes (Latin-1), not their UTF-8. The iteration count is above
      ///    the default, so that a re-hash of it is never one for its work factor.
      /// </summary>
      private static string LegacyLatin1Pbkdf2Hash(string password, int iterations)
      {
         byte[] salt = new byte[16];
         using (var rng = RandomNumberGenerator.Create())
            rng.GetBytes(salt);

         byte[] key;
         using (var pbkdf2 = new Rfc2898DeriveBytes(Encoding.GetEncoding(28591).GetBytes(password), salt, iterations, HashAlgorithmName.SHA256))
            key = pbkdf2.GetBytes(32);

         return "$h1$" + iterations + "$" + Hex(salt) + "$" + Hex(key);
      }

      private static string Hex(byte[] bytes)
      {
         var hex = new StringBuilder(bytes.Length * 2);
         foreach (byte b in bytes)
            hex.Append(b.ToString("x2"));
         return hex.ToString();
      }

      private static string Scram(string address, string password)
      {
         using (var tc = new TcpConnection())
         {
            Assert.IsTrue(tc.Connect(110));
            tc.ReadUntil("+OK");
            string final = RegressionTests.POP3.Pop3SaslTestClient.AuthenticateScram(tc, address, password);
            tc.Disconnect();
            return final;
         }
      }

      [Test]
      [Description("A PBKDF2 hash made before 6.3.4 of a Latin-1 password, over its Latin-1 bytes, opens to the " +
                   "password sent by a client that sends UTF-8 (POP3 PASS, IMAP LOGIN: the bytes widened one to a " +
                   "character), and that sign-in makes it again over the password's UTF-8 - so SCRAM-SHA-256, " +
                   "whose client derives its key from the UTF-8, works for it afterwards. Before, only the " +
                   "characters themselves (SASL PLAIN, COM) opened such a hash, and nothing moved it, because " +
                   "its scheme and work factor needed nothing.")]
      public void ALatin1HashMadeBefore634OpensToUtf8AndIsMadeAgainOverIt()
      {
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         const string latin1Password = "passw\u00f6rd-\u00e9t\u00e9";
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hashupgradelatin1@example.test", latin1Password);

         try
         {
            string legacy = LegacyLatin1Pbkdf2Hash(latin1Password, 400000);
            ForceStoredPassword(account, legacy, CryptPbkdf2);
            Assert.IsTrue(StoredPasswordIs(account, legacy), "The fixture failed to store the legacy hash.");

            // Over the Latin-1 bytes, SCRAM (UTF-8) cannot open it.
            string before = Scram(account.Address, latin1Password);
            Assert.IsTrue(before.StartsWith("-ERR"), "SCRAM derives from UTF-8, which this hash is not over. Got: " + before);
            Assert.IsTrue(StoredPasswordIs(account, legacy), "A failed SCRAM exchange must not touch the stored hash.");

            // POP3 PASS in UTF-8: the password arrives as its UTF-8 widened one byte to a
            // character. Refused before 6.3.4's fix, whatever the hash was over.
            Assert.IsTrue(LogonAndRelease(account.Address, latin1Password),
               "The Latin-1 password sent as UTF-8 must open the hash made of its Latin-1 bytes.");

            Assert.IsFalse(StoredPasswordIs(account, legacy),
               "The sign-in must have made the hash again, over the password's UTF-8.");
            Assert.AreEqual(CryptPbkdf2, ReadStoredEncryptionType(account),
               "The hash is made again under the scheme it had.");

            string after = Scram(account.Address, latin1Password);
            Assert.IsTrue(after.StartsWith("+OK"), "Over UTF-8 now, the hash serves SCRAM. Got: " + after);

            Assert.IsTrue(LogonAndRelease(account.Address, latin1Password), "The password still opens it.");
            string error;
            Assert.IsFalse(LogonAndRelease(account.Address, "passw\u00f6rd-\u00e9t\u00e8", out error),
               "A different password must still be refused.");
         }
         finally
         {
            _settings.ClearLogonFailureList();
            LogHandler.DeleteErrorLog();
         }
      }

      [Test]
      [Description("An account whose password is still stored unencrypted is re-hashed with the preferred " +
                   "scheme in the database on its first successful logon, and still authenticates afterwards.")]
      public void UnencryptedStoredPasswordIsUpgradedOnSuccessfulLogon()
      {
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hashupgrade@example.test", Password);

         try
         {
            // The pre-5.x layout: the password sitting in the row in clear text.
            ForceStoredPassword(account, Password, CryptNone);
            Assert.AreEqual(CryptNone, ReadStoredEncryptionType(account),
               "The fixture failed to put the account back on unencrypted storage.");
            Assert.IsTrue(StoredPasswordIs(account, Password),
               "The fixture failed to store the password in clear text.");

            // Logging on works before the fix as well - ReadObject hashes the stored
            // password in memory, so the comparison succeeds. What did not happen is the
            // write.
            Assert.IsTrue(LogonAndRelease(account.Address, Password),
               "The account must still authenticate on its legacy stored password.");

            // This is the defect. Before the fix accountpwencryption is still 0 here and
            // the clear-text password is still in the row, however often the user logs on.
            Assert.AreEqual(CryptPbkdf2, ReadStoredEncryptionType(account),
               "A successful logon must persist the upgraded hash type, not just compute it in memory.");
            Assert.IsFalse(StoredPasswordIs(account, Password),
               "The clear-text password must no longer be in the database row after the upgrade.");

            // The whole point of the upgrade is that it is invisible to the user: the
            // password that worked must keep working against the newly written hash. If the
            // wrong clear text had been hashed, this is where the account would be locked out.
            Assert.IsTrue(LogonAndRelease(account.Address, Password),
               "The account must still authenticate against the upgraded hash.");
            Assert.AreEqual(CryptPbkdf2, ReadStoredEncryptionType(account),
               "The second logon must leave the already-upgraded hash type alone.");

            // A wrong password is still a wrong password.
            string error;
            Assert.IsFalse(LogonAndRelease(account.Address, "wrong " + Password, out error),
               "An incorrect password must still be refused after the upgrade.");
         }
         finally
         {
            _settings.ClearLogonFailureList();
            LogHandler.DeleteErrorLog();
         }
      }

      [Test]
      [Description("A failed logon attempt against an account stored unencrypted must not rewrite the " +
                   "stored password: the upgrade only ever happens on a password that has been verified.")]
      public void FailedLogonDoesNotUpgradeStoredPassword()
      {
         // Negative control. It passes before the fix as well (nothing upgraded anything
         // then), and its job is to keep the new write from firing on the wrong event -
         // a bug that would let an attacker overwrite a stored password by guessing.
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hashupgradefail@example.test", Password);

         try
         {
            ForceStoredPassword(account, Password, CryptNone);

            string error;
            Assert.IsFalse(LogonAndRelease(account.Address, "not the password", out error),
               "An incorrect password must be refused.");

            Assert.AreEqual(CryptNone, ReadStoredEncryptionType(account),
               "A failed logon must leave the stored password exactly as it was.");
            Assert.IsTrue(StoredPasswordIs(account, Password),
               "A failed logon must leave the stored password exactly as it was.");
         }
         finally
         {
            _settings.ClearLogonFailureList();
            LogHandler.DeleteErrorLog();
         }
      }

      [Test]
      [Description("MinimumAcceptedHashAlgorithm is enforced after the password has been verified and the " +
                   "hash upgraded, so a legacy MD5 account with the correct password is upgraded and let in " +
                   "rather than being locked out of an account it can never upgrade.")]
      public void MinimumAcceptedHashAlgorithmDoesNotLockOutAnUpgradableAccount()
      {
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hashupgrademd5@example.test", Password);

         try
         {
            // First with the policy off. This half passes before the fix too, and it is here
            // to prove the fixture's MD5 encoding really is the one the server produces - if
            // it were not, the assertion below would fail for the wrong reason.
            ForceStoredPassword(account, LegacyMd5Hash(Password), CryptMd5);
            Assert.IsTrue(LogonAndRelease(account.Address, Password),
               "A legacy MD5 account must authenticate with the correct password.");
            Assert.AreEqual(CryptPbkdf2, ReadStoredEncryptionType(account),
               "A legacy MD5 account must be re-hashed with the preferred scheme on logon.");

            // Now the same MD5 row, with the administrator requiring PBKDF2. Before the fix
            // the policy was checked against the stored type before the password was
            // compared, so this logon was refused with the correct password - and because
            // the upgrade only happens on a successful logon, it could never recover. The
            // account was locked out permanently.
            ForceStoredPassword(account, LegacyMd5Hash(Password), CryptMd5);
            SetMinimumAcceptedHashAlgorithm(CryptPbkdf2);

            Assert.IsTrue(LogonAndRelease(account.Address, Password),
               "A legacy account that can be upgraded to meet the policy must be upgraded and let in, not locked out.");
            Assert.AreEqual(CryptPbkdf2, ReadStoredEncryptionType(account),
               "The logon must have persisted the upgrade that satisfies the policy.");

            // The policy itself still bites: a wrong password is refused, and an account
            // that cannot reach the minimum is still refused (covered by the HashPolicy
            // fixture, which pins the minimum-above-preferred case).
            string error;
            Assert.IsFalse(LogonAndRelease(account.Address, "not the password", out error),
               "An incorrect password must still be refused while the policy is active.");
         }
         finally
         {
            SetMinimumAcceptedHashAlgorithm(0);
            _settings.ClearLogonFailureList();
            LogHandler.DeleteErrorLog();
         }
      }
   }
}
