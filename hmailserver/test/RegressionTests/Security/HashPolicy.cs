// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using NUnit.Framework;
using RegressionTests.POP3;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    Exercises the MinimumAcceptedHashAlgorithm policy (a [Settings] value, held in the
   ///    settings store since schema 6042 and mirrored into hMailServer.ini).
   ///    An administrator can refuse authentication for accounts whose stored password
   ///    hash is weaker than a configured Crypt::EncryptionType threshold, forcing those
   ///    passwords to be reset to a strong scheme rather than continuing to be accepted
   ///    (and exposed in any database leak). Accounts created through the COM API are
   ///    stored with the preferred PBKDF2 (4) scheme, so a minimum of Argon2id (5) must
   ///    refuse them while a minimum of PBKDF2 (4) or 0 (disabled) accepts them.
   /// </summary>
   [TestFixture]
   public class HashPolicy : TestFixtureBase
   {

      private const int CryptPbkdf2 = 4;
      private const int CryptArgon2id = 5;

      private void SetMinimumAcceptedHashAlgorithm(int value)
      {
         // Stored through the settings store rather than written into hMailServer.ini: from
         // schema 6042 an edit to the file is undone at the next start, and the store writes
         // the file's copy itself.
         IniFileSetting.Write("MinimumAcceptedHashAlgorithm", value.ToString());

         // The setting is cached in IniFileSettings at startup; Reinitialize reloads it.
         _application.Reinitialize();
      }

      [Test]
      [Description("MinimumAcceptedHashAlgorithm refuses an account whose stored hash (PBKDF2) is weaker " +
                   "than the configured minimum (Argon2id) even with the correct password, and accepts it " +
                   "again once the minimum is lowered to the account's own scheme or disabled.")]
      public void TestMinimumAcceptedHashAlgorithmPolicy()
      {
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         // Accounts created through the COM API are stored with the preferred PBKDF2 scheme.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hashpolicy@example.test", "SeC-r3t Pass!");

         try
         {
            // Require Argon2id: the account's PBKDF2 hash is now below the minimum and
            // must be refused even though the password is correct.
            SetMinimumAcceptedHashAlgorithm(CryptArgon2id);
            string error;
            Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(account.Address, "SeC-r3t Pass!", out error),
               "A PBKDF2 account must be refused when the minimum hash type is Argon2id.");

            // Lower the minimum to PBKDF2: the account meets the policy and logs in.
            SetMinimumAcceptedHashAlgorithm(CryptPbkdf2);
            Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(account.Address, "SeC-r3t Pass!"),
               "A PBKDF2 account must be accepted when the minimum hash type is PBKDF2.");

            // Disable the policy: the account still logs in.
            SetMinimumAcceptedHashAlgorithm(0);
            Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(account.Address, "SeC-r3t Pass!"),
               "A PBKDF2 account must be accepted when the policy is disabled.");
         }
         finally
         {
            // Restore the default (disabled) so later tests are unaffected.
            SetMinimumAcceptedHashAlgorithm(0);
            _settings.ClearLogonFailureList();
         }
      }

      [Test]
      [Description("SCRAM can only be served from a PBKDF2 hash, so when MinimumAcceptedHashAlgorithm " +
                   "is raised above PBKDF2 (to Argon2id) a SCRAM-SHA-256 exchange must fail even with the " +
                   "correct password, and succeed again once the minimum is lowered back to PBKDF2.")]
      public void TestMinimumAcceptedHashAlgorithmDisablesScram()
      {
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hashpolicyscram@example.test", "SeC-r3t Pass!");

         try
         {
            // Require Argon2id: SCRAM (PBKDF2-only) must no longer authenticate anyone.
            SetMinimumAcceptedHashAlgorithm(CryptArgon2id);
            using (var tc = new TcpConnection())
            {
               Assert.IsTrue(tc.Connect(110));
               tc.ReadUntil("+OK"); // banner
               string final = Pop3SaslTestClient.AuthenticateScram(tc, account.Address, "SeC-r3t Pass!");
               Assert.IsTrue(final.StartsWith("-ERR"),
                  "SCRAM must be refused when the minimum hash type is above PBKDF2. Got: " + final);
               tc.Disconnect();
            }

            // Lower the minimum to PBKDF2: SCRAM works again.
            SetMinimumAcceptedHashAlgorithm(CryptPbkdf2);
            using (var tc = new TcpConnection())
            {
               Assert.IsTrue(tc.Connect(110));
               tc.ReadUntil("+OK"); // banner
               string final = Pop3SaslTestClient.AuthenticateScram(tc, account.Address, "SeC-r3t Pass!");
               Assert.IsTrue(final.StartsWith("+OK"),
                  "SCRAM must succeed when the minimum hash type is PBKDF2. Got: " + final);
               tc.Disconnect();
            }
         }
         finally
         {
            SetMinimumAcceptedHashAlgorithm(0);
            _settings.ClearLogonFailureList();
         }
      }

      [Test]
      [Description("A password of Cyrillic letters is hashed whole. Its hash was made over AnsiString(String), which on Windows is NUL bytes from the first letter above U+00FF on and on Linux a '?' for each, so any other password of the same length and shape - sent over SASL PLAIN, whose fields are UTF-8 - opened the account. Now another such password and the question marks are refused, the password itself is taken over SASL PLAIN, and SCRAM, whose client derives its key from the password's UTF-8, is taken too, which it never was for such a password.")]
      public void APasswordOutsideLatinOneIsHashedWhole()
      {
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         const string password = "\u043f\u0430\u0440\u043e\u043b\u044c-\u0442\u0435\u0441\u0442";
         const string sameShape = "\u043a\u043b\u044e\u0447\u0438\u043a-\u0432\u0445\u043e\u0434";
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "nonlatin@example.test", password);

         try
         {
            foreach (string wrong in new[] { sameShape, "??????-????" })
            {
               using (var tc = new TcpConnection())
               {
                  Assert.IsTrue(tc.Connect(110));
                  tc.ReadUntil("+OK");
                  string refused = Pop3SaslTestClient.AuthenticatePlain(tc, account.Address, wrong);
                  Assert.IsTrue(refused.StartsWith("-ERR"),
                     "Another password of the same shape must not open the account. Got: " + refused);
                  tc.Disconnect();
               }
            }

            using (var tc = new TcpConnection())
            {
               Assert.IsTrue(tc.Connect(110));
               tc.ReadUntil("+OK");
               string accepted = Pop3SaslTestClient.AuthenticatePlain(tc, account.Address, password);
               Assert.IsTrue(accepted.StartsWith("+OK"), "The password itself must open the account. Got: " + accepted);
               tc.Send("QUIT\r\n");
               tc.Disconnect();
            }

            using (var tc = new TcpConnection())
            {
               Assert.IsTrue(tc.Connect(110));
               tc.ReadUntil("+OK");
               string scram = Pop3SaslTestClient.AuthenticateScram(tc, account.Address, password);
               Assert.IsTrue(scram.StartsWith("+OK"),
                  "SCRAM-SHA-256 derives its key from the password's UTF-8, which is what the PBKDF2 hash is now made over. Got: " + scram);
               tc.Disconnect();
            }
         }
         finally
         {
            _settings.ClearLogonFailureList();
         }
      }
   }
}
