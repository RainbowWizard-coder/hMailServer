// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    A second factor on the administrator credential, enforced by the SERVER.
   ///
   ///    TOTP protected the Control Panel's own logon for years, checked in C# after
   ///    the server had already accepted the password - a client-side lock on a
   ///    server-side door. This is the door: once a factor is enrolled, the
   ///    administrator password alone authenticates nowhere. Application.Authenticate
   ///    refuses it, AuthenticateWithCode is the only COM way in, and the REST API's
   ///    Basic credential needs the code in an X-hMailServer-OTP header. A code is
   ///    taken once, whichever of them it goes to, and wrong codes are counted
   ///    against one budget (SecondFactorGuard), so every code a test expects to be
   ///    taken comes from OneTimeCodes.Fresh.
   ///
   ///    Every test disables the factor again before it returns, and the fixture does
   ///    so once more at the end, because the whole suite authenticates as the
   ///    administrator with a password: a factor left enrolled would fail every
   ///    fixture that runs afterwards. The disable runs on the already-authenticated
   ///    Settings object, which an enrolled factor does not disturb.
   /// </summary>
   [TestFixture]
   public class AdministratorTwoFactor : TestFixtureBase
   {
      private const string AdminPassword = "testar";
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9512;

      [SetUp]
      public new void SetUp()
      {
         DisableFactor();
      }

      [TearDown]
      public new void TearDown()
      {
         DisableFactor();
         RestoreRestGuarded();
      }

      [OneTimeTearDown]
      public void FinalSafetyNet()
      {
         DisableFactor();
      }

      [Test]
      public void EnrolledTheAdministratorPasswordAloneNoLongerAuthenticatesOverCom()
      {
         string secret = Enrol();

         Assert.IsTrue((bool)_application.AdministratorTOTPEnabled, "The property must report the factor as enrolled.");

         // A brand-new session, the way a fresh client would connect: password
         // alone is refused.
         Assert.IsNull(new Application().Authenticate("Administrator", AdminPassword),
            "The password alone must not authenticate once a factor is enrolled.");

         // The code path lets it back in, and a wrong code does not.
         Assert.IsNotNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, OneTimeCodes.Fresh(secret)),
            "The password plus a current code must authenticate.");
         Assert.IsNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, OneTimeCodes.Wrong(secret)),
            "A wrong code must be refused.");
         Assert.IsNull(new Application().AuthenticateWithCode("Administrator", "wrong-password", OneTimeCodes.At(secret, 1)),
            "A wrong password with a right code must be refused.");
      }

      [Test]
      public void DisablingRestoresPasswordOnlyLogon()
      {
         Enrol();
         Assert.IsNull(new Application().Authenticate("Administrator", AdminPassword));

         _settings.DisableAdministratorTOTP();

         Assert.IsFalse((bool)_application.AdministratorTOTPEnabled);
         Assert.IsNotNull(new Application().Authenticate("Administrator", AdminPassword),
            "With the factor removed, the password alone signs in again.");
      }

      [Test]
      public void TheSecretIsNotReadableAfterEnrolment()
      {
         Enrol();

         // There is no property that hands the secret back: possessing the server
         // must not yield the factor. The only place it ever appeared was the URI.
         string ini = ReadAdministratorTotpSecretFromIni();
         Assert.IsNotEmpty(ini, "The secret is stored...");
         Assert.That(ini, Does.Not.Contain(" "), "...as the protected envelope, not the base32 secret.");
         StringAssert.StartsWith("DPAPI:", ini, "It is machine-protected at rest, like route passwords: " + ini);
      }

      [Test]
      public void TheRestApiNeedsTheCodeInAHeaderAndSaysSoWhenItIsMissing()
      {
         string secret = Enrol();
         EnableRest();

         // Password alone: refused, and told that a second factor is what is
         // missing - so a client knows to ask for a code rather than assuming the
         // password was wrong.
         var noCode = Http("GET", "/api/v1/status", BasicHeader(), null);
         Assert.AreEqual(401, noCode.status, noCode.body);
         StringAssert.Contains("X-hMailServer-OTP: required", noCode.rawHeaders,
            "The 401 must advertise that a one-time code is required. Headers: " + noCode.rawHeaders);

         // Password plus the code in the header: accepted.
         var withCode = Http("GET", "/api/v1/status", BasicHeader(), OneTimeCodes.Fresh(secret));
         Assert.AreEqual(200, withCode.status, withCode.body);

         // Password plus a wrong code: refused.
         var wrongCode = Http("GET", "/api/v1/status", BasicHeader(), OneTimeCodes.Wrong(secret));
         Assert.AreEqual(401, wrongCode.status, wrongCode.body);
      }

      [Test]
      [Description("A code is taken once over COM (RFC 6238 section 5.2): the same code a second time is refused though the password is right, and the next code is taken. The negative control is each code's first use.")]
      public void TheAdministratorsCodeIsTakenOnceOverCom()
      {
         string secret = Enrol();

         string code = OneTimeCodes.Fresh(secret);
         Assert.IsNotNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, code),
            "The password plus a current code must authenticate.");
         Assert.IsNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, code),
            "The same code a second time must be refused: it is a code somebody may have seen, not the one the authenticator shows now.");
         Assert.IsNotNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, OneTimeCodes.Fresh(secret)),
            "The next code is taken.");
      }

      [Test]
      [Description("An empty code over COM is no code: refused, and not counted as a guess - five of them leave the right code taken, as five requests with no X-hMailServer-OTP do at the REST API. Windows only, where the COM API is.")]
      public void AnEmptyCodeOverComIsNotCountedAsAGuess()
      {
         string secret = Enrol();

         for (int attempt = 1; attempt <= 5; attempt++)
         {
            Assert.IsNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, ""),
               "An empty code must not authenticate.");
         }

         Assert.IsNotNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, OneTimeCodes.Fresh(secret)),
            "Five empty codes spent the administrator's wrong-code budget.");
      }

      [Test]
      [Description("A code is taken once over the REST API as well, the administrator's Basic credential included, and a code taken over COM is not taken again over REST: the record of codes taken is one for every door. The negative control is each code's first use.")]
      public void TheAdministratorsCodeIsTakenOnceOverRest()
      {
         string secret = Enrol();
         EnableRest();

         string code = OneTimeCodes.Fresh(secret);
         var first = Http("GET", "/api/v1/status", BasicHeader(), code);
         Assert.AreEqual(200, first.status, first.body);

         var again = Http("GET", "/api/v1/status", BasicHeader(), code);
         Assert.AreEqual(401, again.status, "The same code on a second request was taken again: " + again.body);
         StringAssert.Contains("X-hMailServer-OTP: required", again.rawHeaders);

         var next = Http("GET", "/api/v1/status", BasicHeader(), OneTimeCodes.Fresh(secret));
         Assert.AreEqual(200, next.status, next.body);
      }

      [Test]
      [Description("A code taken once already is refused but not counted as a guess: a script that sends the administrator's password and the same code with every request gets 401 from the second request on, and neither spends the administrator's wrong-code budget nor locks the administrator out. Six of them, and the authenticator's next code is still taken. The negative control is the code's first use, and the first refusal.")]
      public void ACodeSentAgainIsRefusedButNotCountedAsAGuess()
      {
         string secret = Enrol();
         EnableRest();

         string code = OneTimeCodes.Fresh(secret);
         var first = Http("GET", "/api/v1/status", BasicHeader(), code);
         Assert.AreEqual(200, first.status, first.body);

         for (int attempt = 1; attempt <= 6; attempt++)
         {
            var again = Http("GET", "/api/v1/status", BasicHeader(), code);
            Assert.AreEqual(401, again.status, "The code sent again, attempt " + attempt + ", was not refused as a code taken once: " + again.body);
            StringAssert.Contains("X-hMailServer-OTP: required", again.rawHeaders);
         }

         var next = Http("GET", "/api/v1/status", BasicHeader(), OneTimeCodes.Fresh(secret));
         Assert.AreEqual(200, next.status, "Six refusals of a code taken once already spent the administrator's wrong-code budget: " + next.body);
      }

      [Test]
      [Description("A code taken by a COM sign-in is refused at the REST API's Basic sign-in: one record of codes taken, whichever door. Windows only, where the COM API is.")]
      public void ACodeTakenOverComIsNotTakenOverRest()
      {
         string secret = Enrol();
         EnableRest();

         string code = OneTimeCodes.Fresh(secret);
         Assert.IsNotNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, code));

         var overRest = Http("GET", "/api/v1/status", BasicHeader(), code);
         Assert.AreEqual(401, overRest.status, "A code taken over COM was taken again over REST: " + overRest.body);
      }

      [Test]
      [Description("Wrong codes at the administrator's Basic sign-in are counted against the administrator's own budget from a loopback peer too - which the auto-ban leaves alone, and which a reverse proxy makes every request - so after five in fifteen minutes the right code is answered 429 with Retry-After and code second_factor_locked. The negative control is the right code taken before the budget is spent.")]
      public void WrongCodesOnTheBasicSignInAreCountedFromLoopbackToo()
      {
         string secret = Enrol();
         EnableRest();

         // An administrator request of the fixture's own before the budget is spent.
         // On Windows it is a COM read. On Linux it goes over the REST API with the
         // session the fixture layer keeps while a factor stands (ServerApi), which
         // the reinitialise above has just ended: this starts the next one while a
         // code is still taken, so the teardown can disable the factor through it.
         Assert.IsTrue((bool)_application.AdministratorTOTPEnabled);

         var right = Http("GET", "/api/v1/status", BasicHeader(), OneTimeCodes.Fresh(secret));
         Assert.AreEqual(200, right.status, "The right code was not taken: " + right.body);

         string wrong = OneTimeCodes.Wrong(secret);
         for (int attempt = 1; attempt <= 5; attempt++)
         {
            var refused = Http("GET", "/api/v1/status", BasicHeader(), wrong);
            Assert.AreEqual(401, refused.status, "Wrong code " + attempt + " was not refused: " + refused.body);
         }

         var locked = Http("GET", "/api/v1/status", BasicHeader(), OneTimeCodes.Fresh(secret));
         Assert.AreEqual(429, locked.status, "After five wrong codes from loopback the right one was taken: " + locked.body);
         StringAssert.Contains("second_factor_locked", locked.body);
         StringAssert.Contains("Retry-After:", locked.rawHeaders);
      }

      [Test]
      [Description("Wrong codes at the COM sign-in are counted - against the same budget the REST API counts against - so after five in fifteen minutes the right code is refused over COM and answered 429 at the REST API. The negative control is the right code taken over COM before the budget is spent. Windows only, where the COM API is.")]
      public void WrongCodesOverComAreCountedAndTheRestApiSeesTheSameBudget()
      {
         string secret = Enrol();
         EnableRest();

         Assert.IsNotNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, OneTimeCodes.Fresh(secret)),
            "The right code was not taken over COM.");

         string wrong = OneTimeCodes.Wrong(secret);
         for (int attempt = 1; attempt <= 5; attempt++)
         {
            Assert.IsNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, wrong),
               "Wrong code " + attempt + " was taken.");
         }

         Assert.IsNull(new Application().AuthenticateWithCode("Administrator", AdminPassword, OneTimeCodes.Fresh(secret)),
            "After five wrong codes over COM the right one was taken over COM.");

         var overRest = Http("GET", "/api/v1/status", BasicHeader(), OneTimeCodes.At(secret));
         Assert.AreEqual(429, overRest.status, "After five wrong codes over COM the REST API looked at a code: " + overRest.body);
         StringAssert.Contains("second_factor_locked", overRest.body);
      }

      // ----------------------------------------------------------------------

      private string Enrol()
      {
         string uri = _settings.EnrolAdministratorTOTP();
         StringAssert.StartsWith("otpauth://totp/Administrator", uri, "Got: " + uri);

         Match match = Regex.Match(uri, "secret=([A-Z2-7]+)");
         Assert.IsTrue(match.Success, "No base32 secret in the URI. Got: " + uri);
         return match.Groups[1].Value;
      }

      private void DisableFactor()
      {
         try
         {
            _settings.DisableAdministratorTOTP();
         }
         catch (Exception ex) when (ex is COMException || ex is InvalidComObjectException)
         {
            // A fixture that cannot clear this would poison the rest of the run, so
            // it fails loudly rather than leaving the factor enrolled in silence.
            Assert.Fail("Could not disable the administrator second factor in teardown: " + ex.Message);
         }
      }

      private void EnableRest()
      {
         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         // The secret was written to the ini by Enrol and is read live by the REST
         // server, so reinitializing here (to bind the listener) keeps it. TearDown
         // disables the factor and then reinitializes with the port at 0.
         _application.Reinitialize();
      }

      private void RestoreRest()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      private static string BasicHeader()
      {
         return "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("administrator:" + AdminPassword));
      }

      private string ReadAdministratorTotpSecretFromIni()
      {
         const string key = "AdministratorTotpSecret=";

         return IniFileSetting.ExistingIniFiles()
            .SelectMany(iniPath => File.ReadAllLines(iniPath))
            .Select(line => line.Trim())
            .Where(line => line.StartsWith(key, StringComparison.OrdinalIgnoreCase))
            .Select(line => line.Substring(key.Length).Trim())
            .FirstOrDefault(value => value.Length > 0) ?? "";
      }

      // Issues one HTTP/1.0 request against the REST listener over plain TCP and
      // returns the status, the raw header block (so the OTP challenge can be
      // asserted) and the body. `otp`, when given, rides an X-hMailServer-OTP header.
      private (int status, string rawHeaders, string body) Http(string method, string path, string authorization, string otp)
      {
         {
            using (var client = new TcpClient())
            {
               Exception last = null;
               for (int attempt = 0; attempt < 25; attempt++)
               {
                  try { client.Connect("127.0.0.1", RestPort); last = null; break; }
                  catch (SocketException ex) { last = ex; Thread.Sleep(200); }
               }
               if (last != null)
                  throw last;

               var request = new StringBuilder();
               request.Append(method + " " + path + " HTTP/1.0\r\n");
               request.Append("Host: 127.0.0.1\r\n");
               request.Append("Authorization: " + authorization + "\r\n");
               if (otp != null)
                  request.Append("X-hMailServer-OTP: " + otp + "\r\n");
               request.Append("Connection: close\r\n\r\n");

               byte[] bytes = Encoding.ASCII.GetBytes(request.ToString());

               using (NetworkStream stream = client.GetStream())
               using (var memory = new MemoryStream())
               {
                  stream.Write(bytes, 0, bytes.Length);

                  byte[] chunk = new byte[4096];
                  int read;
                  while ((read = stream.Read(chunk, 0, chunk.Length)) > 0)
                     memory.Write(chunk, 0, read);

                  string response = Encoding.ASCII.GetString(memory.ToArray());
                  int split = response.IndexOf("\r\n\r\n", StringComparison.Ordinal);
                  string headers = split < 0 ? response : response.Substring(0, split);
                  string body = split < 0 ? "" : response.Substring(split + 4);

                  int status = 0;
                  Match statusMatch = Regex.Match(headers, @"^HTTP/\d\.\d (\d+)");
                  if (statusMatch.Success)
                     status = int.Parse(statusMatch.Groups[1].Value);

                  return (status, headers, body);
               }
            }
         }
      }

      private void RestoreRestGuarded()
      {
         try { RestoreRest(); }
         catch (Exception ex) when (ex is COMException || ex is InvalidComObjectException)
         {
            // Best effort in teardown; a port already 0 is fine.
         }
      }
   }
}
