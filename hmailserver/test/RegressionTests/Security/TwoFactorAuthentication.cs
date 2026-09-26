// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Linq;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Security
{
   /// <summary>
   ///    Per-account two-factor authentication.
   ///
   ///    TOTP has existed in this product for years and protected exactly one thing:
   ///    the admin tool's own logon, checked in C# by the Control Panel after the
   ///    server had already accepted the password. The server had no idea the feature
   ///    existed, which is why no mailbox account could ever have a second factor.
   ///
   ///    The obstacle was never the algorithm. IMAP, POP3 and SMTP have nowhere to
   ///    type a code, so an account required to present one could not be opened by any
   ///    client at all - and that is what app passwords were built to solve. The rule
   ///    here is the payoff: once a secret is enrolled, the account password stops
   ///    being a mailbox credential, and an app password becomes the only one.
   ///
   ///    The RFC 6238 vectors are pinned in the server's own self-tests, where
   ///    algorithm vectors belong. What this fixture proves is the part those cannot:
   ///    that the code is computed over the wire by an INDEPENDENT implementation
   ///    (.NET's HMACSHA1, below) and accepted - which is what "works with a real
   ///    authenticator app" actually means - and that the enforcement lands on every
   ///    protocol rather than on whichever one was checked first.
   /// </summary>
   [TestFixture]
   public class TwoFactorAuthentication : TestFixtureBase
   {
      private const string Password = "an-ordinary-account-password";

      private Account _account;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "secondfactor@example.test", Password);
      }

      /// <summary>Enrols, and returns the base32 secret out of the otpauth URI.</summary>
      private string Enrol()
      {
         string uri = _account.EnrolTOTP();
         _account.Save();

         StringAssert.StartsWith("otpauth://totp/", uri, "Got: " + uri);
         StringAssert.Contains("secondfactor%40example.test", uri,
            "The address must be percent-encoded or an authenticator app rejects the QR code. Got: " + uri);

         var secret = Regex.Match(uri, "secret=([A-Z2-7]+)");
         ClassicAssert.IsTrue(secret.Success, "No base32 secret in the URI. Got: " + uri);

         return secret.Groups[1].Value;
      }

      /// <summary>
      ///    RFC 6238 in the test, deliberately not calling anything the server owns.
      ///    A code the server computed itself would only prove the server agrees with
      ///    itself; this is what an authenticator app would produce.
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

         var output = new System.Collections.Generic.List<byte>();
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

      private static bool AuthenticatesOverCom(string address, string password, string code)
      {
         var application = new hMailServer.Application();

         return code == null
            ? application.Authenticate(address, password) != null
            : application.AuthenticateWithCode(address, password, code) != null;
      }

      [Test]
      [Description("Nothing is enrolled by default: an ordinary account is untouched by any of this")]
      public void NoSecondFactorByDefault()
      {
         ClassicAssert.IsFalse(_account.TOTPEnabled, "A new account must have no second factor.");

         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3.ConnectAndLogon(_account.Address, Password),
            "...and its password must work exactly as it always has.");
         pop3.Disconnect();
      }

      [Test]
      [Description("A code from an independent RFC 6238 implementation is accepted - which is what 'works with an authenticator app' means")]
      public void ACodeComputedElsewhereIsAccepted()
      {
         string secret = Enrol();

         ClassicAssert.IsTrue(_account.TOTPEnabled);

         // The adjacent steps are accepted, because a client clock a second out would
         // otherwise fail at the boundary of every window. Earliest first: a code is
         // taken once, and taking one puts every earlier step behind the account
         // (SecondFactorGuard), so the previous step's code is offered before the
         // current one and the next step's after it.
         ClassicAssert.IsTrue(AuthenticatesOverCom(_account.Address, Password, CurrentCode(secret, -1)),
            "The previous step must be accepted.");

         ClassicAssert.IsTrue(AuthenticatesOverCom(_account.Address, Password, CurrentCode(secret)),
            "The server must accept a code this test computed with .NET's own HMACSHA1. If it does not, " +
            "no authenticator app will work either, however self-consistent the server is.");

         ClassicAssert.IsTrue(AuthenticatesOverCom(_account.Address, Password, CurrentCode(secret, 1)),
            "The next step must be accepted.");

         // Two steps away is not.
         ClassicAssert.IsFalse(AuthenticatesOverCom(_account.Address, Password, CurrentCode(secret, 5)),
            "A code five steps out must be refused, or the window is not a window.");
      }

      [Test]
      [Description("A code is taken once (RFC 6238 section 5.2): the same code a second time is refused over COM though the password is right, and so is the code of the step before it; the next step's code is taken. The negative control is each code's first use.")]
      public void ACodeIsTakenOnceOverCom()
      {
         string secret = Enrol();

         string code = CurrentCode(secret);
         ClassicAssert.IsTrue(AuthenticatesOverCom(_account.Address, Password, code), "The code's first use is taken.");
         ClassicAssert.IsFalse(AuthenticatesOverCom(_account.Address, Password, code),
            "The same code a second time must be refused: it is a code somebody may have seen, not the one the authenticator shows now.");
         ClassicAssert.IsFalse(AuthenticatesOverCom(_account.Address, Password, CurrentCode(secret, -1)),
            "The step before one that was taken is behind the account too.");
         ClassicAssert.IsTrue(AuthenticatesOverCom(_account.Address, Password, CurrentCode(secret, 1)),
            "The next step's code is taken.");
      }

      [Test]
      [Description("A code taken once already is refused over COM but not counted as a guess: six of them leave the account's wrong-code budget as it was, and the next step's code is taken. The negative control is the code's first use. Its own account, so nothing it counts touches another test.")]
      public void ACodeSentAgainOverComIsNotCountedAsAGuess()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "secondfactor-again@example.test", Password);
         string uri = account.EnrolTOTP();
         account.Save();
         string secret = Regex.Match(uri, "secret=([A-Z2-7]+)").Groups[1].Value;
         ClassicAssert.IsNotEmpty(secret, uri);

         string code = OneTimeCodes.Fresh(secret);
         ClassicAssert.IsTrue(AuthenticatesOverCom(account.Address, Password, code), "The code's first use is taken.");

         for (int attempt = 1; attempt <= 6; attempt++)
         {
            ClassicAssert.IsFalse(AuthenticatesOverCom(account.Address, Password, code),
               "The code sent again, attempt " + attempt + ", was taken a second time.");
         }

         ClassicAssert.IsTrue(AuthenticatesOverCom(account.Address, Password, OneTimeCodes.Fresh(secret)),
            "Six refusals of a code taken once already spent the account's wrong-code budget.");

         account.DisableTOTP();
         account.Save();
      }

      [Test]
      [Description("Wrong codes at the COM sign-in are counted against the account's own budget: after five in fifteen minutes the right code is refused too, until the budget is renewed. The negative control is the right code taken before the budget is spent. Its own account, so the lock touches no other test.")]
      public void WrongCodesOverComAreCounted()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "secondfactor-guess@example.test", Password);
         string uri = account.EnrolTOTP();
         account.Save();
         string secret = Regex.Match(uri, "secret=([A-Z2-7]+)").Groups[1].Value;
         ClassicAssert.IsNotEmpty(secret, uri);

         ClassicAssert.IsTrue(AuthenticatesOverCom(account.Address, Password, OneTimeCodes.Fresh(secret)),
            "The right code is taken while the budget stands.");

         string wrong = OneTimeCodes.Wrong(secret);
         for (int attempt = 1; attempt <= 5; attempt++)
         {
            ClassicAssert.IsFalse(AuthenticatesOverCom(account.Address, Password, wrong), "Wrong code " + attempt + " was taken.");
         }

         ClassicAssert.IsFalse(AuthenticatesOverCom(account.Address, Password, OneTimeCodes.Fresh(secret)),
            "After five wrong codes the right one was taken: the guessing did not stop.");

         // A factor enrolled afresh is another secret, and starts with a budget of
         // its own.
         account.DisableTOTP();
         account.Save();
      }

      [Test]
      [Description("Both factors are required: neither the password nor the code is sufficient alone")]
      public void BothFactorsAreRequired()
      {
         string secret = Enrol();

         ClassicAssert.IsFalse(AuthenticatesOverCom(_account.Address, Password, "000000"),
            "The right password with a wrong code must be refused.");

         ClassicAssert.IsFalse(AuthenticatesOverCom(_account.Address, "wrong-password", CurrentCode(secret)),
            "A valid code with the wrong password must be refused - otherwise the code IS the credential.");

         ClassicAssert.IsFalse(AuthenticatesOverCom(_account.Address, Password, null),
            "And the password alone, through the path that cannot present a code, must be refused.");
      }

      [Test]
      [Description("THE point of the chain: the account password stops working in mail clients, and an app password takes over")]
      public void TheAccountPasswordStopsWorkingInMailClientsAndAnAppPasswordTakesOver()
      {
         Enrol();

         // Every protocol, not just the one that happened to be checked first. This is
         // the assertion that catches enforcement landing in a single handler.
         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsFalse(pop3.ConnectAndLogon(_account.Address, Password),
            "POP3 must refuse the account password once a second factor is enrolled.");
         pop3.Disconnect();

         var imap = new ImapClientSimulator();
         ClassicAssert.IsFalse(imap.ConnectAndLogon(_account.Address, Password),
            "IMAP must refuse it too.");
         imap.Disconnect();

         // The simulator throws rather than reporting a refused AUTH through its out
         // parameter, which is what a refusal looks like from here.
         var smtp = new SmtpClientSimulator();
         string smtpError;
         CustomAsserts.Throws<DeliveryFailedException>(() =>
            smtp.Send(false, _account.Address, Password, _account.Address, _account.Address,
               "denied", "This must not be accepted.", out smtpError));

         // ...and the credential that exists precisely so the mailbox is still usable.
         var appPassword = _account.AppPasswords.Add();
         appPassword.Name = "Phone";
         string issued = appPassword.Generate();
         appPassword.Save();

         var pop3WithApp = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3WithApp.ConnectAndLogon(_account.Address, issued),
            "An app password must still open the mailbox. Without this the second factor is " +
            "not two-factor authentication, it is an outage.");
         pop3WithApp.Disconnect();

         var imapWithApp = new ImapClientSimulator();
         ClassicAssert.IsTrue(imapWithApp.ConnectAndLogon(_account.Address, issued), "...over IMAP as well.");
         imapWithApp.Disconnect();
      }

      [Test]
      [Description("Removing the second factor restores the account password, and deliberately leaves app passwords alone")]
      public void DisablingRestoresTheAccountPassword()
      {
         Enrol();

         var appPassword = _account.AppPasswords.Add();
         appPassword.Name = "Laptop";
         string issued = appPassword.Generate();
         appPassword.Save();

         _account.DisableTOTP();
         _account.Save();

         ClassicAssert.IsFalse(_account.TOTPEnabled);

         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3.ConnectAndLogon(_account.Address, Password),
            "The account password must work again.");
         // Released, or the app-password logon below is refused with [IN-USE] and this
         // test reports a revocation that never happened.
         pop3.Disconnect();

         // The app password was a separately revocable credential before the second
         // factor existed and remains one after it goes. Deleting it here would break
         // every client the account holder had set up, as a side effect of an
         // unrelated administrative action.
         var pop3WithApp = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3WithApp.ConnectAndLogon(_account.Address, issued),
            "Disabling the second factor must not revoke app passwords.");
         pop3WithApp.Disconnect();
      }

      [Test]
      [Description("The secret is not readable after enrolment - possessing the account must not yield the factor")]
      public void TheSecretIsNotReadableAfterwards()
      {
         string first = Enrol();

         // There is no property that returns it: the only way to see a secret is to
         // enrol a new one, which replaces the old.
         var type = _account.GetType();
         foreach (string name in new[] { "TOTPSecret", "TotpSecret", "Secret" })
         {
            ClassicAssert.IsNull(type.GetProperty(name),
               "The COM surface must not expose a way to read the secret back (found: " + name + ").");
         }

         string second = _account.EnrolTOTP();
         _account.Save();

         ClassicAssert.IsFalse(second.Contains(first),
            "Re-enrolling must mint a NEW secret rather than returning the existing one.");
      }

      /// <summary>One SCRAM-SHA-256 exchange on each protocol; true for each that signed in.</summary>
      private static (bool smtp, bool imap, bool pop3) ScramOnEveryProtocol(string address, string password)
      {
         bool smtp;
         using (var sock = new TcpConnection())
         {
            ClassicAssert.IsTrue(sock.Connect(25));
            ClassicAssert.IsTrue(sock.Receive().StartsWith("220"));
            sock.Send("EHLO test.com\r\n");
            ClassicAssert.IsTrue(sock.Receive().StartsWith("250"));
            smtp = RegressionTests.SMTP.SmtpScramTestClient.Authenticate(sock, address, password).StartsWith("235");
         }

         bool imap;
         using (var con = new TcpConnection())
         {
            ClassicAssert.IsTrue(con.Connect(143));
            con.Receive();
            imap = RegressionTests.IMAP.ScramTestClient.Authenticate(con, "A01", address, password).Contains("A01 OK");
         }

         bool pop3;
         using (var tc = new TcpConnection())
         {
            ClassicAssert.IsTrue(tc.Connect(110));
            tc.ReadUntil("+OK");
            pop3 = RegressionTests.POP3.Pop3SaslTestClient.AuthenticateScram(tc, address, password).StartsWith("+OK");

            // Released before the next exchange, whose refusal must be the second
            // factor's and not [IN-USE].
            tc.Send("QUIT\r\n");
            tc.Receive();
         }

         return (smtp, imap, pop3);
      }

      [Test]
      [Description("SCRAM-SHA-256 proves the account's own password, so once a second factor is enrolled it is refused on SMTP, IMAP and POP3 as the PLAIN path refuses it")]
      public void ScramRefusesTheAccountPasswordOnceASecondFactorIsEnrolled()
      {
         var settings = SingletonProvider<TestSetup>.Instance.GetApp().Settings;
         bool autoBan = settings.AutoBanOnLogonFailure;
         bool saslPlain = settings.IMAPSASLPlainEnabled;
         settings.AutoBanOnLogonFailure = false;
         settings.IMAPSASLPlainEnabled = true;
         settings.ClearLogonFailureList();

         try
         {
            // The negative control: before enrolment the account is one SCRAM can
            // serve (a PBKDF2 hash, the default), so a refusal below is the second
            // factor and not the mechanism.
            var before = ScramOnEveryProtocol(_account.Address, Password);
            ClassicAssert.IsTrue(before.smtp && before.imap && before.pop3,
               "SCRAM must sign the account in before a second factor is enrolled (smtp " + before.smtp +
               ", imap " + before.imap + ", pop3 " + before.pop3 + ").");

            Enrol();

            // Until 26 September 2026 all three signed in here: SCRAM never reached
            // PasswordValidator, where the rule lived, so a phished password of an
            // account with TOTP submitted mail and read the mailbox.
            var after = ScramOnEveryProtocol(_account.Address, Password);
            ClassicAssert.IsFalse(after.smtp, "SMTP AUTH SCRAM-SHA-256 accepted the account password of an account with a second factor.");
            ClassicAssert.IsFalse(after.imap, "IMAP AUTHENTICATE SCRAM-SHA-256 accepted it.");
            ClassicAssert.IsFalse(after.pop3, "POP3 AUTH SCRAM-SHA-256 accepted it.");
         }
         finally
         {
            settings.AutoBanOnLogonFailure = autoBan;
            settings.IMAPSASLPlainEnabled = saslPlain;
            settings.ClearLogonFailureList();
         }
      }
   }
}
