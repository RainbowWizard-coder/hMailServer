// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The operator's tools over REST, which were COM's alone: a mailbox exported
   ///    to the server's disk (POST /api/v1/accounts/{address}/export), an address's
   ///    traces erased (POST /api/v1/addresses/erase), and the administrator's
   ///    second factor read, enrolled and disabled (/api/v1/administrator/second-factor).
   ///
   ///    The second factor is enforced from the moment it is enrolled, over REST as
   ///    over COM: every request after the enrolment carries a code computed here
   ///    from the secret the enrolment answered - by an independent RFC 6238
   ///    implementation, so that what the server accepts is what an authenticator
   ///    app produces - and the factor is disabled again before the test ends,
   ///    whatever happened, because the rest of the suite signs in with the
   ///    password alone.
   /// </summary>
   [TestFixture]
   public class RestApiOperator : TestFixtureBase
   {
      private static int RestPort = 9595;
      private const string AdminPassword = "testar";
      private const string SecondFactorPath = "/api/v1/administrator/second-factor";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
      }

      [Test]
      [Description("The mailbox is written to the directory named as .eml files mirroring the folders, the count answered; no directory is 400 and an unknown account 404.")]
      public void TheMailboxIsExportedToTheServersDisk()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "export-rest@example.test", "test");
         var smtp = new SmtpClientSimulator();
         smtp.Send("sender@example.test", account.Address, "First", "Body one");
         smtp.Send("sender@example.test", account.Address, "Second", "Body two");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

         string directory = Path.Combine(Path.GetTempPath(), "hm-rest-export-" + Guid.NewGuid().ToString("N"));
         try
         {
            (int status, string body) answer = Http("POST", "/api/v1/accounts/" + account.Address + "/export", "{\"directory\":" + JsonSerializer.Serialize(directory) + "}");
            ClassicAssert.AreEqual(200, answer.status, answer.body);
            using (JsonDocument document = JsonDocument.Parse(answer.body))
            {
               ClassicAssert.AreEqual(2, document.RootElement.GetProperty("exported").GetInt32());
               ClassicAssert.AreEqual(directory, document.RootElement.GetProperty("directory").GetString());
            }

            string[] files = Directory.GetFiles(directory, "*.eml", SearchOption.AllDirectories);
            ClassicAssert.AreEqual(2, files.Length, string.Join(", ", files));
            string contents = string.Join("\n", files.Select(File.ReadAllText));
            StringAssert.Contains("Body one", contents);
            StringAssert.Contains("Body two", contents);

            ClassicAssert.AreEqual(400, Http("POST", "/api/v1/accounts/" + account.Address + "/export", "{\"directory\":\"\"}").status);
            ClassicAssert.AreEqual(400, Http("POST", "/api/v1/accounts/" + account.Address + "/export", "{}").status);
            ClassicAssert.AreEqual(404, Http("POST", "/api/v1/accounts/nobody@example.test/export", "{\"directory\":" + JsonSerializer.Serialize(directory) + "}").status);
         }
         finally
         {
            if (Directory.Exists(directory))
               Directory.Delete(directory, true);
         }
      }

      [Test]
      [Description("The traces an address leaves - here an alias pointing at it - are erased with the count answered; nothing found is success with 0; no address is 400.")]
      public void AnAddressesTracesAreErased()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "erase-rest@example.test", "test");
         const string erased = "erase-alias@example.test";

         Alias alias = _domain.Aliases.Add();
         alias.Name = erased;
         alias.Value = account.Address;
         alias.Active = true;
         alias.Save();
         ClassicAssert.IsNotNull(_domain.Aliases.get_ItemByName(erased));

         (int status, string body) answer = Http("POST", "/api/v1/addresses/erase", "{\"address\":\"" + erased + "\",\"include_archive\":false}");
         ClassicAssert.AreEqual(200, answer.status, answer.body);
         using (JsonDocument document = JsonDocument.Parse(answer.body))
         {
            ClassicAssert.GreaterOrEqual(document.RootElement.GetProperty("removed").GetInt32(), 1, "The alias is a trace.");
            ClassicAssert.IsFalse(document.RootElement.GetProperty("include_archive").GetBoolean());
         }
         _domain.Aliases.Refresh();
         Assert.Throws<System.Runtime.InteropServices.COMException>(() => _domain.Aliases.get_ItemByName(erased),
            "The alias is gone: the collection has no item by that name.");

         (int status, string body) again = Http("POST", "/api/v1/addresses/erase", "{\"address\":\"" + erased + "\"}");
         ClassicAssert.AreEqual(200, again.status, again.body);
         StringAssert.Contains("\"removed\":0", again.body, "Nothing found is success.");

         ClassicAssert.AreEqual(400, Http("POST", "/api/v1/addresses/erase", "{\"address\":\"not-an-address\"}").status);
         ClassicAssert.AreEqual(400, Http("POST", "/api/v1/addresses/erase", "{\"address\":\"" + erased + "\",\"include_archive\":\"yes\"}").status);
      }

      [Test]
      [Description("The second factor: not enrolled, then enrolled with an otpauth URI answered once, enforced on the next request, a second enrolment refused, and disabled again.")]
      public void TheAdministratorsSecondFactorIsEnrolledAndDisabled()
      {
         StringAssert.Contains("\"enrolled\":false", Http("GET", SecondFactorPath).body, "The suite runs with the password alone.");

         (int status, string body) enrolled = Http("POST", SecondFactorPath, "{}");
         ClassicAssert.AreEqual(200, enrolled.status, enrolled.body);
         string uri;
         using (JsonDocument document = JsonDocument.Parse(enrolled.body))
         {
            ClassicAssert.IsTrue(document.RootElement.GetProperty("enrolled").GetBoolean());
            uri = document.RootElement.GetProperty("uri").GetString();
         }
         StringAssert.StartsWith("otpauth://totp/Administrator", uri);
         string secret = SecretOf(uri);
         ClassicAssert.IsNotEmpty(secret);

         try
         {
            // Enforced from this moment: the password alone is refused, the
            // password with the code is accepted.
            ClassicAssert.AreEqual(401, Http("GET", SecondFactorPath).status, "The password alone no longer authenticates.");
            // Each request its own code: a code is taken once (SecondFactorGuard),
            // so the third request below waits for the authenticator's next one.
            (int status, string body) withCode = Http("GET", SecondFactorPath, null, "X-hMailServer-OTP: " + OneTimeCodes.Fresh(secret));
            ClassicAssert.AreEqual(200, withCode.status, withCode.body);
            StringAssert.Contains("\"enrolled\":true", withCode.body);

            (int status, string body) twice = Http("POST", SecondFactorPath, "{}", "X-hMailServer-OTP: " + OneTimeCodes.Fresh(secret));
            ClassicAssert.AreEqual(409, twice.status, "A second enrolment would replace the secret every app holds.");
         }
         finally
         {
            (int status, string body) disabled = Http("DELETE", SecondFactorPath, null, "X-hMailServer-OTP: " + OneTimeCodes.Fresh(secret));
            ClassicAssert.AreEqual(200, disabled.status, disabled.body);
         }

         StringAssert.Contains("\"enrolled\":false", Http("GET", SecondFactorPath).body, "The password alone signs in again.");
      }

      // ----------------------------------------------------------------- helpers

      private static (int status, string body) Http(string method, string path, string requestBody = null, string extraHeader = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody, extraHeader);
      }

      private static string SecretOf(string otpauthUri)
      {
         int at = otpauthUri.IndexOf("secret=", StringComparison.OrdinalIgnoreCase);
         if (at < 0)
            return "";
         string rest = otpauthUri.Substring(at + "secret=".Length);
         int end = rest.IndexOf('&');
         return end < 0 ? rest : rest.Substring(0, end);
      }
   }
}
