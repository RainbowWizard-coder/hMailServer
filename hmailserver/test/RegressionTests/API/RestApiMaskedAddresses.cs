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
   ///    Masked addresses (MaskedAddresses.h): an address the reader makes at
   ///    /api/v1/me/masked-addresses delivers mail from anyone to the mailbox, counts
   ///    it, is offered in the From picker and accepted in MAIL FROM with the
   ///    authenticated-sender check on - and once switched off or removed, mail to it
   ///    is refused at RCPT and the account no longer writes as it.
   /// </summary>
   [TestFixture]
   public class RestApiMaskedAddresses : TestFixtureBase
   {
      private static int RestPort = 9619;
      private const string AdminPassword = "testar";
      private const string Password = "Masked-Passw0rd!";

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
            Admin("PUT", "/api/v1/settings", "{\"reader_masked_addresses_max\":20}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A masked address delivers to its owner and counts it, is written as while on, and once off or removed refuses mail at RCPT and is no longer written as; another account cannot change it.")]
      public void AMaskedAddressDeliversUntilItIsSwitchedOff()
      {
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "masked-owner@" + _domain.Name, Password);
         string me = Basic(owner.Address, Password);

         (int status, string body) made = Send("POST", "/api/v1/me/masked-addresses", me, "{\"label\":\"The Coffee Shop\"}");
         Assert.AreEqual(201, made.status, made.body);
         string masked = Regex.Match(made.body, "\"address\":\"([^\"]+)\"").Groups[1].Value;
         string id = Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value;
         StringAssert.IsMatch("^thecoffeeshop\\.[a-z2-9]{6}@" + Regex.Escape(_domain.Name) + "$", masked);
         StringAssert.Contains("\"active\":true", made.body);
         StringAssert.Contains("\"label\":\"The Coffee Shop\"", made.body);

         // Mail from anyone arrives, and is counted.
         SmtpClientSimulator.StaticSend("shop@dummy-example.com", masked, "Your order", "Thank you for your order.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(owner.Address, Password, 1);
         StringAssert.IsMatch("\"received\":[1-9]", Send("GET", "/api/v1/me/masked-addresses", me, null).body);

         // Written as while it is on.
         StringAssert.Contains("{\"address\":\"" + masked + "\",\"name\":\"\",\"kind\":\"masked\"", Send("GET", "/api/v1/me/identities", me, null).body);
         Assert.AreEqual("", SendAs(owner.Address, masked), "The owner may send as the masked address.");

         Account neighbour = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "masked-neighbour@" + _domain.Name, Password);
         Assert.AreEqual(404, Send("PUT", "/api/v1/me/masked-addresses/" + id, Basic(neighbour.Address, Password), "{\"active\":false}").status);
         StringAssert.Contains("550", SendAs(neighbour.Address, masked), "Another account may not send as it.");

         // Switched off: refused at RCPT, not written as.
         (int status, string body) off = Send("PUT", "/api/v1/me/masked-addresses/" + id, me, "{\"active\":false}");
         Assert.AreEqual(200, off.status, off.body);
         StringAssert.Contains("\"active\":false", off.body);
         StringAssert.Contains("550", Refusal(masked), "Mail to a switched-off masked address is refused.");
         StringAssert.DoesNotContain(masked, Send("GET", "/api/v1/me/identities", me, null).body);
         StringAssert.Contains("550", SendAs(owner.Address, masked));

         // On again, and then removed.
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/masked-addresses/" + id, me, "{\"active\":true,\"label\":\"Coffee\"}").status);
         Assert.AreEqual("", Refusal(masked), "On again, mail is taken.");
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/masked-addresses/" + id, me, null).status);
         StringAssert.Contains("550", Refusal(masked), "A removed masked address is an unknown address.");
      }

      [Test]
      [Description("The server's ceiling is kept, and 0 offers none; a label is at most 255 characters.")]
      public void TheCeilingIsKept()
      {
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "masked-ceiling@" + _domain.Name, Password);
         string me = Basic(owner.Address, Password);

         Assert.AreEqual(400, Send("POST", "/api/v1/me/masked-addresses", me, "{\"label\":\"" + new string('x', 256) + "\"}").status);
         Assert.AreEqual(201, Send("POST", "/api/v1/me/masked-addresses", me, "{\"label\":\"\"}").status, "A label may be empty; the address says masked.");
         StringAssert.IsMatch("\"address\":\"masked\\.[a-z2-9]{6}@", Send("GET", "/api/v1/me/masked-addresses", me, null).body);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_masked_addresses_max\":1}").status);
         (int status, string body) second = Send("POST", "/api/v1/me/masked-addresses", me, "{\"label\":\"Second\"}");
         Assert.AreEqual(403, second.status, second.body);
         StringAssert.Contains("at most 1", second.body);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_masked_addresses_max\":0}").status);
         Assert.AreEqual(403, Send("GET", "/api/v1/me/masked-addresses", me, null).status);
      }

      // "" when accepted; the refusal otherwise.
      private static string SendAs(string user, string from)
      {
         try
         {
            new SmtpClientSimulator().Send(false, user, Password, from, user, "Sent as a masked address", "Body.", out string ignored);
            return "";
         }
         catch (DeliveryFailedException ex)
         {
            return ex.Message;
         }
      }

      // "" when mail from outside to the address is taken; the refusal otherwise.
      private static string Refusal(string to)
      {
         try
         {
            new SmtpClientSimulator().Send("shop@dummy-example.com", to, "Another offer", "Body.");
            return "";
         }
         catch (DeliveryFailedException ex)
         {
            return ex.Message;
         }
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
