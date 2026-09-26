// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    A recovery address and one-time recovery codes (AccountRecovery.h), end to
   ///    end against a real server.
   ///
   ///    The recovery address is set with the account's own password, proved by
   ///    reading the link out of the message the server really sends to a second
   ///    mailbox on this server, and only ever answered masked. The forgotten-password
   ///    flow sends a single-use link to that address and nowhere else, gives the same
   ///    answer for an address it knows nothing about, refuses a stale or reused
   ///    token, applies the password rules, ends every browser session of the account,
   ///    and tells both mailboxes. A recovery code is accepted in place of the
   ///    one-time code at sign-in for an account whose second factor is enrolled, is
   ///    spent, and is refused the second time.
   ///
   ///    The links are read out of the mail rather than constructed, which is what
   ///    makes this a test of the feature and not of the test's idea of it.
   /// </summary>
   [TestFixture]
   public class RestApiAccountRecovery : TestFixtureBase
   {
      private static int RestPort = 9726;
      private const string AdminPassword = "testar";
      private const string Password = "Recovery-Passw0rd!";
      private const string NewPassword = "A-Brand-New-Passw0rd!";
      private const string BaseUrl = "https://mail.example.test";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         // The links have to point somewhere, and with no base URL configured the
         // whole feature answers "unavailable" - which is a state of its own and is
         // checked at the end of the last test rather than assumed here.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings",
            "{\"confidential_base_url\":\"" + BaseUrl + "\",\"account_recovery_enabled\":true}").status);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\",\"account_recovery_enabled\":true}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The recovery address is set with the account's own password, confirmed by the link that is really posted to it, answered only masked, and then used to reset a forgotten password: the link ends every session, the rules apply, both mailboxes are told, and the token works once.")]
      public void TheAddressIsProvedAndResetsAForgottenPassword()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery@" + _domain.Name, Password);
         Account elsewhere = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-elsewhere@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         // ---- setting it asks for the account's own password
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/recovery", me, "{\"password\":\"" + Password + "\"}").status,
            "An address is required.");
         Assert.AreEqual(403, Send("PUT", "/api/v1/me/recovery", me,
            "{\"address\":\"" + elsewhere.Address + "\",\"password\":\"not-the-password\"}").status,
            "The account's own password has to be right.");
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/recovery", me,
            "{\"address\":\"" + account.Address + "\",\"password\":\"" + Password + "\"}").status,
            "The account's own address is not a recovery address.");

         (int status, string body) set = Send("PUT", "/api/v1/me/recovery", me,
            "{\"address\":\"" + elsewhere.Address + "\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(200, set.status, set.body);

         (int status, string body) state = Send("GET", "/api/v1/me/recovery", me, null);
         Assert.AreEqual(200, state.status, state.body);
         StringAssert.Contains("\"address_set\":true", state.body);
         StringAssert.Contains("\"address_verified\":false", state.body);
         StringAssert.DoesNotContain(elsewhere.Address, state.body,
            "The address is never answered in full once it is set, not even to the account that set it.");
         StringAssert.Contains("\"address\":\"r", state.body, "The masked form is answered.");

         // ---- the confirmation really arrives at the other mailbox
         string confirmation = Pop3ClientSimulator.AssertGetFirstMessageText(elsewhere.Address, Password);
         StringAssert.Contains("/recovery/verify?token=", confirmation, confirmation);
         string verifyToken = TokenIn(confirmation, "/recovery/verify?token=");

         Assert.AreEqual(200, Send("GET", "/recovery/verify?token=" + verifyToken, null, null).status,
            "The page a scanner would follow shows a button and confirms nothing.");
         StringAssert.Contains("\"address_verified\":false", Send("GET", "/api/v1/me/recovery", me, null).body,
            "A GET of the link confirms nothing: a mail scanner follows links, and the button is the confirmation.");

         Assert.AreEqual(200, Send("POST", "/recovery/verify?token=" + verifyToken, null, null).status,
            "The button confirms it.");
         StringAssert.Contains("\"address_verified\":true", Send("GET", "/api/v1/me/recovery", me, null).body);
         Assert.AreEqual(410, Send("POST", "/recovery/verify?token=" + verifyToken, null, null).status,
            "The confirmation link works once.");

         // ---- what an administrator can see, and what they cannot
         (int status, string body) seen = Admin("GET", "/api/v1/accounts/" + account.Address, null);
         Assert.AreEqual(200, seen.status, seen.body);
         StringAssert.Contains("\"recovery_address_set\":true", seen.body);
         StringAssert.Contains("\"recovery_address_verified\":true", seen.body);
         StringAssert.DoesNotContain(elsewhere.Address, seen.body,
            "An administrator is shown the masked address and never the address itself.");

         // ---- an app password is a credential handed to a mail program: it can
         // read the mailbox and nothing else. It cannot take away the account's own
         // way back in, and it cannot make the server post a message to the address
         // the account named.
         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", me,
            "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(account.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);
         Assert.AreEqual(403, Send("DELETE", "/api/v1/me/recovery", asProgram, null).status,
            "Removing the recovery address is refused to a request authenticated with an app password.");
         Assert.AreEqual(403, Send("POST", "/api/v1/me/recovery/resend", asProgram, null).status,
            "So is making the server send the confirmation again.");
         StringAssert.Contains("\"address_verified\":true", Send("GET", "/api/v1/me/recovery", me, null).body,
            "And the address is still there, because neither refusal did anything.");

         // ---- a session that the reset must end
         (int status, string body) session = Send("POST", "/api/v1/session", me, null);
         Assert.AreEqual(201, session.status, session.body);

         // ---- forgotten: the same answer for an address this server knows nothing about
         (int status, string body) unknown = Send("POST", "/api/v1/password-reset", null,
            "{\"address\":\"nobody-at-all@" + _domain.Name + "\"}");
         (int status, string body) asked = Send("POST", "/api/v1/password-reset", null,
            "{\"address\":\"" + account.Address + "\"}");
         Assert.AreEqual(202, unknown.status, unknown.body);
         Assert.AreEqual(202, asked.status, asked.body);
         Assert.AreEqual(unknown.body, asked.body,
            "The answer must not say whether the address exists or has a recovery address.");

         string letter = Pop3ClientSimulator.AssertGetFirstMessageText(elsewhere.Address, Password);
         StringAssert.Contains("/portal/reset?token=", letter, letter);
         string resetToken = TokenIn(letter, "/portal/reset?token=");

         // ---- a token that is not one, and one that is not this one
         Assert.AreEqual(404, Send("GET", "/portal/reset?token=nonsense", null, null).status);
         Assert.AreEqual(410, Send("POST", "/api/v1/password-reset/complete", null,
            "{\"token\":\"" + new string('a', 64) + "\",\"password\":\"" + NewPassword + "\"}").status,
            "A token that names nothing is refused as a used one is.");

         Assert.AreEqual(200, Send("GET", "/portal/reset?token=" + resetToken, null, null).status,
            "The link opens the form.");

         // A password the route refuses leaves the link ALIVE: the person chooses
         // again on the same link rather than going back for another, and the link is
         // spent only by a password that was accepted.
         Assert.AreEqual(400, Send("POST", "/api/v1/password-reset/complete", null,
            "{\"token\":\"" + resetToken + "\",\"password\":\"\"}").status,
            "A password the route will not have is refused.");
         Assert.AreEqual(200, Send("GET", "/portal/reset?token=" + resetToken, null, null).status,
            "And the link still opens the form: a refusal does not spend it.");

         (int status, string body) done = Send("POST", "/api/v1/password-reset/complete", null,
            "{\"token\":\"" + resetToken + "\",\"password\":\"" + NewPassword + "\"}");
         Assert.AreEqual(200, done.status, done.body);

         Assert.AreEqual(410, Send("POST", "/api/v1/password-reset/complete", null,
            "{\"token\":\"" + resetToken + "\",\"password\":\"" + NewPassword + "2\"}").status,
            "The reset link works once.");

         // ---- the new password works and the old one does not
         Assert.AreEqual(201, Send("POST", "/api/v1/session", Basic(account.Address, NewPassword), null).status);
         Assert.AreEqual(401, Send("POST", "/api/v1/session", me, null).status, "The old password is gone.");

         // ---- and both mailboxes were told
         Pop3ClientSimulator.AssertMessageCount(account.Address, NewPassword, 1);
         StringAssert.Contains("password", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, NewPassword).ToLower());
         StringAssert.Contains("was changed", Pop3ClientSimulator.AssertGetFirstMessageText(elsewhere.Address, Password));

         // ---- an administrator clears it, and the person starts again
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/accounts/" + account.Address + "/recovery", null).status);
         StringAssert.Contains("\"recovery_address_set\":false", Admin("GET", "/api/v1/accounts/" + account.Address, null).body);
         Assert.AreEqual(404, Admin("DELETE", "/api/v1/accounts/nobody-at-all@" + _domain.Name + "/recovery", null).status);
      }

      [Test]
      [Description("A recovery code is accepted in place of the one-time code at sign-in for an account whose second factor is enrolled, is spent when the password was right, says how many are left, and is refused the second time; a wrong code leaves the account password refused.")]
      public void ARecoveryCodeStandsInForTheOneTimeCode()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-codes@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         Assert.AreEqual(403, Send("POST", "/api/v1/me/recovery/codes", me, "{\"password\":\"not-the-password\"}").status,
            "Making codes asks for the account's own password.");

         (int status, string body) made = Send("POST", "/api/v1/me/recovery/codes", me, "{\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, made.status, made.body);

         MatchCollection codes = Regex.Matches(made.body, "\"([2-9A-HJ-NP-TV-Z]{5}-[2-9A-HJ-NP-TV-Z]{5})\"");
         Assert.AreEqual(10, codes.Count, "Ten codes, shown once: " + made.body);

         StringAssert.Contains("\"unused\":10", Send("GET", "/api/v1/me/recovery", me, null).body);
         StringAssert.Contains("\"recovery_codes_unused\":10", Admin("GET", "/api/v1/accounts/" + account.Address, null).body);

         // The second factor, enrolled by an administrator, is what stops the
         // account's own password being a mailbox credential at all.
         account.EnrolTOTP();
         account.Save();

         Assert.AreEqual(401, Send("POST", "/api/v1/session", me, null).status,
            "With a second factor enrolled the account password alone does not sign in.");
         Assert.AreEqual(401, Send("POST", "/api/v1/session", me, null, "X-hMailServer-Recovery-Code: ZZZZZ-ZZZZZ").status,
            "A code that is not one of the account's changes nothing.");

         string first = codes[0].Groups[1].Value;
         (int status, string body) signedIn = Send("POST", "/api/v1/session", me, null, "X-hMailServer-Recovery-Code: " + first);
         Assert.AreEqual(201, signedIn.status, signedIn.body);
         StringAssert.Contains("\"recovery_codes_left\":9", signedIn.body);

         Assert.AreEqual(401, Send("POST", "/api/v1/session", me, null, "X-hMailServer-Recovery-Code: " + first).status,
            "A code is spent by the sign-in it made.");

         (int status, string body) again = Send("POST", "/api/v1/session", me, null,
            "X-hMailServer-Recovery-Code: " + codes[1].Groups[1].Value);
         Assert.AreEqual(201, again.status, again.body);
         StringAssert.Contains("\"recovery_codes_left\":8", again.body);

         // A code cannot be burned by somebody who does not have the password.
         Assert.AreEqual(401, Send("POST", "/api/v1/session", Basic(account.Address, "not-the-password"), null,
            "X-hMailServer-Recovery-Code: " + codes[2].Groups[1].Value).status);
         (int status, string body) third = Send("POST", "/api/v1/session", me, null,
            "X-hMailServer-Recovery-Code: " + codes[2].Groups[1].Value);
         Assert.AreEqual(201, third.status, third.body);
         StringAssert.Contains("\"recovery_codes_left\":7", third.body);

         // A code card somebody else has read. The account holder's own remedy is a
         // fresh set from the webmail, which is exactly what somebody locked out of
         // the webmail cannot reach, so an administrator can end the set.
         StringAssert.Contains("\"recovery_codes_unused\":7", Admin("GET", "/api/v1/accounts/" + account.Address, null).body);
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/accounts/" + account.Address + "/recovery/codes", null).status);
         StringAssert.Contains("\"recovery_codes_unused\":0", Admin("GET", "/api/v1/accounts/" + account.Address, null).body);
         Assert.AreEqual(401, Send("POST", "/api/v1/session", me, null,
            "X-hMailServer-Recovery-Code: " + codes[3].Groups[1].Value).status,
            "A code out of a set an administrator ended signs nothing in.");
         Assert.AreEqual(404, Admin("DELETE", "/api/v1/accounts/nobody-at-all@" + _domain.Name + "/recovery/codes", null).status);

         account.DisableTOTP();
         account.Save();
      }

      [Test]
      [Description("A one-time code asked for on top of the account's password - here at POST /api/v1/me/password, by a client holding an app password and the account password - is counted when it is wrong: after five wrong codes in fifteen minutes every code for the account is refused with 429, the right one included, and the password stays as it was. The negative control is the right code taken before the budget is spent.")]
      public void AWrongOneTimeCodeIsCountedAndTheBudgetEndsTheGuessing()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-guess@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         // An app password, made before the second factor is enrolled: it is the
         // credential the account's own password stops being once it is.
         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", me,
            "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(account.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);

         string uri = account.EnrolTOTP();
         account.Save();
         string secret = Regex.Match(uri, "secret=([A-Z2-7]+)").Groups[1].Value;
         Assert.IsNotEmpty(secret, uri);

         // No code: a client that has not been told one is needed, answered 401 with
         // the header named, and not counted.
         Assert.AreEqual(401, Send("POST", "/api/v1/me/password", asProgram,
            "{\"current\":\"" + Password + "\",\"new\":\"" + NewPassword + "\"}").status);

         // The negative control: the right code is taken while the budget stands.
         (int status, string body) changed = Send("POST", "/api/v1/me/password", asProgram,
            "{\"current\":\"" + Password + "\",\"new\":\"" + NewPassword + "\"}", "X-hMailServer-OTP: " + CurrentCode(secret));
         Assert.AreEqual(200, changed.status, "The right code was not taken: " + changed.body);

         // Five wrong codes, each refused as a wrong code is.
         const string Third = "A-Third-Passw0rd-Chosen!";
         string change = "{\"current\":\"" + NewPassword + "\",\"new\":\"" + Third + "\"}";
         string wrong = WrongCode(secret);
         for (int attempt = 1; attempt <= 5; attempt++)
         {
            Assert.AreEqual(401, Send("POST", "/api/v1/me/password", asProgram, change, "X-hMailServer-OTP: " + wrong).status,
               "Wrong code " + attempt + " was not refused.");
         }

         // And now the right one is refused too, for as long as the budget is spent -
         // which is what makes guessing stop, since a guess that happens to be right
         // looks exactly like this.
         (int status, string body) locked = Send("POST", "/api/v1/me/password", asProgram, change,
            "X-hMailServer-OTP: " + CurrentCode(secret));
         Assert.AreEqual(429, locked.status, "After five wrong codes the right one was taken: " + locked.body);
         StringAssert.Contains("second_factor_locked", locked.body);

         // The password did not change: the third one is not the current password.
         Assert.AreEqual(403, Send("POST", "/api/v1/me/password", asProgram,
            "{\"current\":\"" + Third + "\",\"new\":\"" + Password + "\"}").status,
            "The account password changed although the code was refused.");

         account.DisableTOTP();
         account.Save();
      }

      [Test]
      [Description("A one-time code asked for on top of the account's password is taken once (RFC 6238 section 5.2): the same code on a second request - here at POST /api/v1/me/password - is refused with 401 and the password stays as it was, six times without being counted as a guess, and the authenticator's next code is taken. The negative control is each code's first use.")]
      public void AOneTimeCodeIsTakenOnce()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-replay@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", me,
            "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(account.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);

         string uri = account.EnrolTOTP();
         account.Save();
         string secret = Regex.Match(uri, "secret=([A-Z2-7]+)").Groups[1].Value;
         Assert.IsNotEmpty(secret, uri);

         string code = OneTimeCodes.Fresh(secret);
         (int status, string body) changed = Send("POST", "/api/v1/me/password", asProgram,
            "{\"current\":\"" + Password + "\",\"new\":\"" + NewPassword + "\"}", "X-hMailServer-OTP: " + code);
         Assert.AreEqual(200, changed.status, "The code's first use was not taken: " + changed.body);

         // The same code again, as somebody who read it over a shoulder or out of a
         // log would send it, with the password that is now current.
         const string Third = "A-Third-Passw0rd-Chosen!";
         // Six times: refused every time, and not counted as a guess - so the
         // next code below is taken, where six counted guesses would have spent
         // the budget and answered it 429.
         for (int attempt = 1; attempt <= 6; attempt++)
         {
            (int status, string body) replayed = Send("POST", "/api/v1/me/password", asProgram,
               "{\"current\":\"" + NewPassword + "\",\"new\":\"" + Third + "\"}", "X-hMailServer-OTP: " + code);
            Assert.AreEqual(401, replayed.status, "A code taken once was taken again: " + replayed.body);
         }

         Assert.AreEqual(403, Send("POST", "/api/v1/me/password", asProgram,
            "{\"current\":\"" + Third + "\",\"new\":\"" + Password + "\"}").status,
            "The account password changed on a code that had been used already.");

         (int status, string body) next = Send("POST", "/api/v1/me/password", asProgram,
            "{\"current\":\"" + NewPassword + "\",\"new\":\"" + Third + "\"}", "X-hMailServer-OTP: " + OneTimeCodes.Fresh(secret));
         Assert.AreEqual(200, next.status, "The authenticator's next code was not taken: " + next.body);

         account.DisableTOTP();
         account.Save();
      }

      [Test]
      [Description("A peer on this machine - a webmail or reverse proxy on loopback - is not held to the per-client counter, and is held instead to thirty requests in any minute for every loopback request together: a sweep of thirty names, one request each, leaves the next request answered as every other is and nothing sent. The negative control is the same request a minute later, which sends the link.")]
      public void ASweepFromLoopbackMeetsACeilingOfItsOwn()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-sweep@" + _domain.Name, Password);
         Account elsewhere = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-sweep-to@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         Assert.AreEqual(200, Send("PUT", "/api/v1/me/recovery", me,
            "{\"address\":\"" + elsewhere.Address + "\",\"password\":\"" + Password + "\"}").status);
         string confirmation = Pop3ClientSimulator.AssertGetFirstMessageText(elsewhere.Address, Password);
         Assert.AreEqual(200, Send("POST", "/recovery/verify?token=" + TokenIn(confirmation, "/recovery/verify?token="), null, null).status);

         string answer;
         try
         {
            // Thirty names, one request each, from 127.0.0.1: none of them repeats an
            // address, so the per-address counter never refuses one.
            for (int i = 0; i < 30; i++)
            {
               Assert.AreEqual(202, Send("POST", "/api/v1/password-reset", null,
                  "{\"address\":\"nobody-" + i + "@" + _domain.Name + "\"}").status);
            }

            // The thirty-first, for a mailbox that could be reset: the same answer, and
            // nothing sent. Waited for as long as the queued work would take had there
            // been any, then counted once the delivery queue is empty.
            (int status, string body) held = Send("POST", "/api/v1/password-reset", null, "{\"address\":\"" + account.Address + "\"}");
            Assert.AreEqual(202, held.status, held.body);
            answer = held.body;
            System.Threading.Thread.Sleep(3000);
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(elsewhere.Address, Password, 0);
         }
         finally
         {
            // The ceiling is every loopback request's, the tests after this one included.
            System.Threading.Thread.Sleep(61000);
         }

         // A minute on, the same request sends the link, and was answered in the same words.
         (int status, string body) sent = Send("POST", "/api/v1/password-reset", null, "{\"address\":\"" + account.Address + "\"}");
         Assert.AreEqual(202, sent.status, sent.body);
         Assert.AreEqual(answer, sent.body, "A request held by the ceiling is answered exactly as one that was not.");
         Pop3ClientSimulator.AssertMessageCount(elsewhere.Address, Password, 1);
         StringAssert.Contains("/portal/reset?token=", Pop3ClientSimulator.AssertGetFirstMessageText(elsewhere.Address, Password));
      }

      /// <summary>RFC 6238 in the test, independently of the server: what an authenticator app shows.</summary>
      private static string CurrentCode(string base32Secret, int stepOffset = 0)
      {
         const string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
         var key = new System.Collections.Generic.List<byte>();
         int buffer = 0, bitsLeft = 0;
         foreach (char c in base32Secret)
         {
            int index = alphabet.IndexOf(char.ToUpperInvariant(c));
            if (index < 0)
               continue;
            buffer = (buffer << 5) | index;
            bitsLeft += 5;
            if (bitsLeft >= 8)
            {
               bitsLeft -= 8;
               key.Add((byte) ((buffer >> bitsLeft) & 0xFF));
            }
         }

         long counter = DateTimeOffset.UtcNow.ToUnixTimeSeconds() / 30 + stepOffset;
         byte[] counterBytes = BitConverter.GetBytes(counter);
         if (BitConverter.IsLittleEndian)
            Array.Reverse(counterBytes);

         byte[] hash;
         using (var hmac = new System.Security.Cryptography.HMACSHA1(key.ToArray()))
            hash = hmac.ComputeHash(counterBytes);

         int offset = hash[hash.Length - 1] & 0x0F;
         int binary = ((hash[offset] & 0x7F) << 24) | ((hash[offset + 1] & 0xFF) << 16) |
                      ((hash[offset + 2] & 0xFF) << 8) | (hash[offset + 3] & 0xFF);
         return (binary % 1000000).ToString("000000");
      }

      /// <summary>Six digits that are none of the codes the server would take in the next minute and a half.</summary>
      private static string WrongCode(string base32Secret)
      {
         var near = new System.Collections.Generic.HashSet<string>();
         for (int step = -2; step <= 3; step++)
            near.Add(CurrentCode(base32Secret, step));
         for (int candidate = 0; ; candidate += 111111)
         {
            string code = (candidate % 1000000).ToString("000000");
            if (!near.Contains(code))
               return code;
         }
      }

      [Test]
      [Description("Asking again within the minute answers exactly as the first, and no second link is sent; and with no public base URL the whole thing answers unavailable.")]
      public void AskingTooOftenAnswersTheSameAndSendsNothing()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-rate@" + _domain.Name, Password);
         Account elsewhere = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recovery-rate-to@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         Assert.AreEqual(200, Send("PUT", "/api/v1/me/recovery", me,
            "{\"address\":\"" + elsewhere.Address + "\",\"password\":\"" + Password + "\"}").status);
         // The confirmation is charged against the same per-address budget a reset
         // link is, so asking for it again within the minute is refused rather than
         // posting a second message at the address the account named.
         (int status, string body) tooSoon = Send("POST", "/api/v1/me/recovery/resend", me, null);
         Assert.AreEqual(429, tooSoon.status, tooSoon.body);
         StringAssert.Contains("retry_after", tooSoon.body);

         string confirmation = Pop3ClientSimulator.AssertGetFirstMessageText(elsewhere.Address, Password);
         Assert.AreEqual(200, Send("POST", "/recovery/verify?token=" + TokenIn(confirmation, "/recovery/verify?token="), null, null).status);

         (int status, string body) one = Send("POST", "/api/v1/password-reset", null, "{\"address\":\"" + account.Address + "\"}");
         (int status, string body) two = Send("POST", "/api/v1/password-reset", null, "{\"address\":\"" + account.Address + "\"}");
         Assert.AreEqual(202, one.status, one.body);
         Assert.AreEqual(202, two.status, two.body);
         Assert.AreEqual(one.body, two.body, "A request that was rate limited is answered exactly as one that was not.");

         // One message, not two: the second request was refused before anything was
         // looked up, so nothing else was queued. Counted once the queue is empty,
         // so that "only one arrived" is not "the second has not arrived yet".
         Pop3ClientSimulator.AssertMessageCount(elsewhere.Address, Password, 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(elsewhere.Address, Password, 1);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\"}").status);
         Assert.AreEqual(503, Send("POST", "/api/v1/password-reset", null, "{\"address\":\"" + account.Address + "\"}").status,
            "With no public address of its own the server cannot write a link, and says so rather than pretending.");
         StringAssert.Contains("\"available\":false", Send("GET", "/api/v1/me/recovery", me, null).body);
      }

      /// <summary>The 64 hex digits after the marker, out of the message as it was sent.</summary>
      private static string TokenIn(string text, string marker)
      {
         int at = text.IndexOf(marker, StringComparison.Ordinal);
         Assert.Greater(at, -1, "No " + marker + " in: " + text);
         Match token = Regex.Match(text.Substring(at + marker.Length), "^[0-9a-f]{64}");
         Assert.IsTrue(token.Success, "No token after " + marker + " in: " + text);
         return token.Value;
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody, string extraHeader)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody, extraHeader);
      }
   }
}
