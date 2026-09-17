// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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
   ///    Sending as an address elsewhere (SenderAddresses.h): the reader adds it at
   ///    /api/v1/me/sender-addresses, the code mailed to it - caught here by an SMTP
   ///    server the dummy-example.com route points at - is typed back, and from then
   ///    on the address is in the From picker and accepted in MAIL FROM with the
   ///    authenticated-sender check on. Before the code, after removing it, and for
   ///    an address on this server, it is not.
   /// </summary>
   [TestFixture]
   public class RestApiSenderAddresses : TestFixtureBase
   {
      private static int RestPort = 9618;
      private const string AdminPassword = "testar";
      private const string Password = "Send-As-Passw0rd!";
      private const string Elsewhere = "me@dummy-example.com";

      [OneTimeSetUp]
      public void TurnTheSenderCheckOn()
      {
         ServerIniFile.SetSetting("SmtpAuthenticatedSenderCheck", "1");
         RestartServerAndReacquireCom();
      }

      [OneTimeTearDown]
      public void TurnItOff()
      {
         ServerIniFile.SetSetting("SmtpAuthenticatedSenderCheck", null);
         RestartServerAndReacquireCom();
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings", "{\"reader_sender_addresses_max\":5}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An address elsewhere is sent as only once the code mailed to it has been typed back, in the From picker and in MAIL FROM alike, and not after it is removed; an address on this server and one added twice are refused.")]
      public void AnAddressIsSentAsOnceTheCodeMailedToItIsTypedBack()
      {
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice-sendas@" + _domain.Name, Password);
         string me = Basic(alice.Address, Password);

         string code = AddAndCatchCode(me, "{\"address\":\"ME@dummy-example.com\",\"name\":\"Alice Elsewhere\"}", out string id);

         StringAssert.DoesNotContain(Elsewhere, Send("GET", "/api/v1/me/identities", me, null).body, "Not in the picker before the code.");
         AssertRefused(SendAs(alice.Address, Elsewhere), "MAIL FROM is refused before the code.");

         string wrong = code == "000000" ? "111111" : "000000";
         (int status, string body) mistaken = Send("POST", "/api/v1/me/sender-addresses/" + id + "/verify", me, "{\"code\":\"" + wrong + "\"}");
         Assert.AreEqual(400, mistaken.status, mistaken.body);
         StringAssert.Contains("\"attempts_left\":4", mistaken.body);

         (int status, string body) proved = Send("POST", "/api/v1/me/sender-addresses/" + id + "/verify", me, "{\"code\":\"" + code + "\"}");
         Assert.AreEqual(200, proved.status, proved.body);
         StringAssert.Contains("\"verified\":true", proved.body);

         StringAssert.Contains("{\"address\":\"" + Elsewhere + "\",\"name\":\"Alice Elsewhere\",\"kind\":\"verified\"", Send("GET", "/api/v1/me/identities", me, null).body);
         Assert.AreEqual("", SendAs(alice.Address, Elsewhere), "MAIL FROM is accepted once the address is proved.");
         Assert.AreEqual(409, Send("POST", "/api/v1/me/sender-addresses/" + id + "/resend", me, null).status, "A proved address needs no new code.");

         Assert.AreEqual(400, Send("POST", "/api/v1/me/sender-addresses", me, "{\"address\":\"someone@" + _domain.Name + "\"}").status,
            "An address on this server is shared by its owner, not proved by mail.");
         Assert.AreEqual(409, Send("POST", "/api/v1/me/sender-addresses", me, "{\"address\":\"" + Elsewhere + "\"}").status);

         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/sender-addresses/" + id, me, null).status);
         StringAssert.DoesNotContain(Elsewhere, Send("GET", "/api/v1/me/identities", me, null).body);
         AssertRefused(SendAs(alice.Address, Elsewhere), "MAIL FROM is refused again once the address is removed.");

         // Another account never reaches it.
         Account bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob-sendas@" + _domain.Name, Password);
         Assert.AreEqual(404, Send("POST", "/api/v1/me/sender-addresses/" + id + "/verify", Basic(bob.Address, Password), "{\"code\":\"" + code + "\"}").status);
      }

      [Test]
      [Description("A new code goes out no sooner than a minute after the last; five wrong codes end one; the server's ceiling is kept, and 0 offers none.")]
      public void WrongCodesEndTheCodeAndTheCeilingIsKept()
      {
         Account carol = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "carol-sendas@" + _domain.Name, Password);
         string me = Basic(carol.Address, Password);

         string code = AddAndCatchCode(me, "{\"address\":\"" + Elsewhere + "\"}", out string id);

         (int status, string body) tooSoon = Send("POST", "/api/v1/me/sender-addresses/" + id + "/resend", me, null);
         Assert.AreEqual(429, tooSoon.status, tooSoon.body);
         StringAssert.Contains("\"retry_after\":", tooSoon.body);

         string wrong = code == "000000" ? "111111" : "000000";
         for (int left = 4; left >= 1; left--)
         {
            (int status, string body) mistaken = Send("POST", "/api/v1/me/sender-addresses/" + id + "/verify", me, "{\"code\":\"" + wrong + "\"}");
            Assert.AreEqual(400, mistaken.status, mistaken.body);
            StringAssert.Contains("\"attempts_left\":" + left, mistaken.body);
         }
         Assert.AreEqual(410, Send("POST", "/api/v1/me/sender-addresses/" + id + "/verify", me, "{\"code\":\"" + wrong + "\"}").status, "The fifth wrong code ends it.");
         Assert.AreEqual(410, Send("POST", "/api/v1/me/sender-addresses/" + id + "/verify", me, "{\"code\":\"" + code + "\"}").status, "And the right one no longer works.");
         StringAssert.Contains("\"verified\":false", Send("GET", "/api/v1/me/sender-addresses", me, null).body);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_sender_addresses_max\":1}").status);
         (int status, string body) second = Send("POST", "/api/v1/me/sender-addresses", me, "{\"address\":\"other@dummy-example.com\"}");
         Assert.AreEqual(403, second.status, second.body);
         StringAssert.Contains("at most 1", second.body);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_sender_addresses_max\":0}").status);
         Assert.AreEqual(403, Send("GET", "/api/v1/me/sender-addresses", me, null).status);
      }

      // Adds an address with the route to a local server in place, and returns the
      // code the message it received carries.
      private string AddAndCatchCode(string authorization, string body, out string id)
      {
         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { Elsewhere, 250 } });
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, port, false);

            var added = Send("POST", "/api/v1/me/sender-addresses", authorization, body);
            Assert.AreEqual(201, added.status, added.body);
            StringAssert.Contains("\"address\":\"" + Elsewhere + "\"", added.body, "Kept lower-cased.");
            StringAssert.Contains("\"verified\":false", added.body);
            StringAssert.Contains("\"code_sent\":true", added.body);
            id = Regex.Match(added.body, "\"id\":(\\d+)").Groups[1].Value;

            server.WaitForCompletion();
            string mail = server.MessageData ?? "";
            StringAssert.Contains("Subject: Your code to send mail as " + Elsewhere, mail);
            Match code = Regex.Match(mail, "The code is: (\\d{6})");
            Assert.IsTrue(code.Success, "No code in: " + mail);
            return code.Groups[1].Value;
         }
      }

      // "" when accepted; the refusal otherwise.
      private static string SendAs(string user, string from)
      {
         try
         {
            new SmtpClientSimulator().Send(false, user, Password, from, user, "Sent as another address", "Body.", out string ignored);
            return "";
         }
         catch (DeliveryFailedException ex)
         {
            return ex.Message;
         }
      }

      private static void AssertRefused(string outcome, string message)
      {
         StringAssert.Contains("550", outcome, message);
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
   }
}
