// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using System.Text.Json;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    An account's own rules over REST: GET and POST /api/v1/accounts/{address}/rules,
   ///    PUT and DELETE /api/v1/accounts/{address}/rules/{id}. The same documents the
   ///    global rule routes take, on the collection Account.Rules holds over COM.
   ///
   ///    What matters is that a rule saved here is the account's and nobody else's:
   ///    it acts on the account's next message and on no other account's, it is not
   ///    in the global listing, and the global routes do not reach it - which the
   ///    RestApiRules fixture pins from the other side.
   /// </summary>
   [TestFixture]
   public class RestApiAccountRules : TestFixtureBase
   {
      private static int RestPort = 9594;
      private const string AdminPassword = "testar";

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

      private static string RuleBody(string name, string criteria, string actions, bool active = true)
      {
         return "{\"name\":\"" + name + "\",\"active\":" + (active ? "true" : "false") +
                ",\"all_criteria\":true,\"criteria\":[" + criteria + "],\"actions\":[" + actions + "]}";
      }

      private static string Path(Account account)
      {
         return "/api/v1/accounts/" + Uri.EscapeDataString(account.Address) + "/rules";
      }

      [Test]
      [Description("A rule created under the account is listed there and not globally, acts on the account's next message and on nobody else's, is replaced by PUT, and is gone after DELETE.")]
      public void AnAccountsRuleIsTheAccountsAlone()
      {
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rules-owner@example.test", "test");
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rules-other@example.test", "test");

         (int status, string body) created = Http("POST", Path(owner),
            RuleBody("drop-adverts", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"advert\"}", "{\"type\":\"delete\"}"));
         ClassicAssert.AreEqual(201, created.status, created.body);
         long id;
         using (JsonDocument document = JsonDocument.Parse(created.body))
         {
            id = document.RootElement.GetProperty("id").GetInt64();
            ClassicAssert.AreEqual("drop-adverts", document.RootElement.GetProperty("name").GetString());
         }
         ClassicAssert.Greater(id, 0);

         (int status, string body) listed = Http("GET", Path(owner));
         ClassicAssert.AreEqual(200, listed.status);
         StringAssert.Contains("\"id\":" + id + ",", listed.body, "The account's listing carries the rule.");
         StringAssert.DoesNotContain("\"id\":" + id + ",", Http("GET", "/api/v1/rules").body, "The global listing does not.");
         StringAssert.DoesNotContain("\"id\":" + id + ",", Http("GET", Path(other)).body, "Nor another account's.");

         // COM sees the same rule, under the account.
         Rule overCom = owner.Rules.get_ItemByDBID((int) id);
         ClassicAssert.IsNotNull(overCom);
         ClassicAssert.AreEqual("drop-adverts", overCom.Name);

         // In force for the next message, without a restart: the advert is
         // dropped for the owner and delivered to the other account.
         var smtp = new SmtpClientSimulator();
         smtp.Send("sender@example.test", owner.Address, "An advert for you", "Body");
         smtp.Send("sender@example.test", other.Address, "An advert for you", "Body");
         smtp.Send("sender@example.test", owner.Address, "Minutes", "Body");
         Pop3ClientSimulator.AssertMessageCount(other.Address, "test", 1);
         Pop3ClientSimulator.AssertMessageCount(owner.Address, "test", 1);
         StringAssert.Contains("Minutes", new Pop3ClientSimulator().GetFirstMessageText(owner.Address, "test"));

         // Replaced: the same id, a different criterion, and off - so the next
         // advert is kept.
         (int status, string body) replaced = Http("PUT", Path(owner) + "/" + id,
            RuleBody("drop-adverts-off", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"advert\"}", "{\"type\":\"delete\"}", false));
         ClassicAssert.AreEqual(200, replaced.status, replaced.body);
         StringAssert.Contains("\"name\":\"drop-adverts-off\"", replaced.body);
         StringAssert.Contains("\"active\":false", replaced.body);

         smtp.Send("sender@example.test", owner.Address, "Another advert", "Body");
         // One, not two: GetFirstMessageText above read "Minutes" with RETR and
         // then DELE, so the mailbox held nothing until this advert arrived.
         Pop3ClientSimulator.AssertMessageCount(owner.Address, "test", 1);
         StringAssert.Contains("Another advert", new Pop3ClientSimulator().GetFirstMessageText(owner.Address, "test"));

         // The other account's address does not reach it, and nor does the
         // global route.
         ClassicAssert.AreEqual(404, Http("PUT", Path(other) + "/" + id, RuleBody("taken", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"delete\"}")).status);
         ClassicAssert.AreEqual(404, Http("DELETE", "/api/v1/rules/" + id).status);
         ClassicAssert.AreEqual(404, Http("DELETE", Path(other) + "/" + id).status);

         ClassicAssert.AreEqual(200, Http("DELETE", Path(owner) + "/" + id).status);
         ClassicAssert.AreEqual(404, Http("DELETE", Path(owner) + "/" + id).status, "Gone.");
         StringAssert.DoesNotContain("\"id\":" + id + ",", Http("GET", Path(owner)).body);
         ClassicAssert.IsNull(owner.Rules.get_ItemByDBID((int) id));
      }

      [Test]
      [Description("An unknown account is 404 on every verb, and a body the global route would refuse is refused here with the same reason.")]
      public void UnknownAccountsAndBadBodiesAreRefused()
      {
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rules-refusals@example.test", "test");

         ClassicAssert.AreEqual(404, Http("GET", "/api/v1/accounts/nobody@example.test/rules").status);
         ClassicAssert.AreEqual(404, Http("POST", "/api/v1/accounts/nobody@example.test/rules",
            RuleBody("x", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"delete\"}")).status);
         ClassicAssert.AreEqual(404, Http("DELETE", "/api/v1/accounts/nobody@example.test/rules/1").status);

         (int status, string body) refused = Http("POST", Path(owner),
            RuleBody("no-recipient", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"forward\"}"));
         ClassicAssert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("error", refused.body);
         ClassicAssert.AreEqual(0, owner.Rules.Count, "A refused rule is not saved.");
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
