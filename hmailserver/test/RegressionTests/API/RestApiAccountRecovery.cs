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
