// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The global rule write routes: POST /api/v1/rules, PUT and DELETE /api/v1/rules/&lt;id&gt;.
   ///
   ///    Every assertion reads the rule back through COM (app.Rules, the
   ///    collection the Control Panel edits) and never only from the response,
   ///    and the round trip goes on to prove the rule acts on mail: a message
   ///    sent over SMTP is or is not in the folder IMAP shows, without a
   ///    restart, exactly as a rule saved in the Control Panel would act. The
   ///    refusals are checked to have changed nothing, through the same
   ///    collection, and a rule that belongs to an account is shown to be out
   ///    of reach of the global routes.
   ///
   ///    Every rule this fixture makes is named with a prefix TearDown removes,
   ///    because a global rule outlives the domain the suite recreates for
   ///    each test.
   /// </summary>
   [TestFixture]
   public class RestApiRules : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9121;
      private const string AdminPassword = "testar";
      private const string RulePrefix = "restrule-";
      private const string RouteDomain = "restrule-route.test";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         // The rules first, over COM: a global rule is not in any domain, so
         // the per-test domain reset does not take it with it, and a rule
         // left behind by a failed assertion would act on the next fixture's
         // mail. Then the route one test makes for send_using_route.
         try
         {
            hMailServer.Rules rules = _application.Rules;
            for (int i = rules.Count - 1; i >= 0; i--)
            {
               Rule rule = rules[i];
               if (rule.Name.StartsWith(RulePrefix, StringComparison.OrdinalIgnoreCase))
                  rule.Delete();
            }

            hMailServer.Routes routes = _settings.Routes;
            for (int i = routes.Count - 1; i >= 0; i--)
            {
               Route route = routes[i];
               if (string.Equals(route.DomainName, RouteDomain, StringComparison.OrdinalIgnoreCase))
                  route.Delete();
            }
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // The global rule through COM, or null. Asked of app.Rules - a fresh
      // read of hm_rules each time, which is what the Control Panel shows -
      // so the API's answer is checked against something that did not
      // produce it.
      private Rule RuleOverCom(int id)
      {
         try
         {
            return _application.Rules.get_ItemByDBID(id);
         }
         catch (System.Runtime.InteropServices.COMException)
         {
            return null;
         }
      }

      private static string RuleBody(string name, string criteria, string actions, bool active = true, bool allCriteria = true)
      {
         return "{\"name\":\"" + name + "\",\"active\":" + (active ? "true" : "false") +
                ",\"all_criteria\":" + (allCriteria ? "true" : "false") +
                ",\"criteria\":[" + criteria + "],\"actions\":[" + actions + "]}";
      }

      [Test]
      [Description("A rule is created, listed, read back through COM and acts on the next message; replaced by PUT with a header criterion and a move, and acts differently; deleted, and no longer acts. A second delete and a PUT on the gone id are 404.")]
      public void RuleRoundTripActsOnDelivery()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruleuser@example.test", "test");

         string name = RulePrefix + "drop";
         (int status, string body) created = Http("POST", "/api/v1/rules",
            RuleBody(name,
               "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"restrule-drop\"}",
               "{\"type\":\"delete\"}"));
         Assert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("\"name\":\"" + name + "\"", created.body);
         StringAssert.Contains("\"active\":true", created.body);
         StringAssert.Contains("\"all_criteria\":true", created.body);
         StringAssert.Contains("{\"field\":\"subject\",\"header\":\"\",\"match\":\"contains\",\"value\":\"restrule-drop\"}", created.body);
         StringAssert.Contains("{\"type\":\"delete\",\"value\":\"\"}", created.body);
         int id = ExtractId(created.body);
         Assert.Greater(id, 0, "The created rule carries its id.");

         // The listing shows it, as the same entry the create answered with.
         (int listStatus, string list) = Http("GET", "/api/v1/rules");
         Assert.AreEqual(200, listStatus, list);
         StringAssert.Contains("\"id\":" + id + ",\"name\":\"" + name + "\",\"active\":true,\"all_criteria\":true", list);

         // COM sees what the Control Panel would show.
         Rule rule = RuleOverCom(id);
         Assert.IsNotNull(rule, "The rule must exist through COM after POST /api/v1/rules.");
         Assert.AreEqual(name, rule.Name);
         Assert.IsTrue(rule.Active);
         Assert.IsTrue(rule.UseAND);
         Assert.AreEqual(0, rule.AccountID, "A rule created here is global.");
         Assert.AreEqual(1, rule.Criterias.Count);
         RuleCriteria criterion = rule.Criterias[0];
         Assert.IsTrue(criterion.UsePredefined);
         Assert.AreEqual(eRulePredefinedField.eFTSubject, criterion.PredefinedField);
         Assert.AreEqual(eRuleMatchType.eMTContains, criterion.MatchType);
         Assert.AreEqual("restrule-drop", criterion.MatchValue);
         Assert.AreEqual(1, rule.Actions.Count);
         Assert.AreEqual(eRuleActionType.eRADeleteEmail, rule.Actions[0].Type);
         int firstCriterionId = criterion.ID;
         int firstActionId = rule.Actions[0].ID;

         // It acts on the next message, without a restart: the one whose
         // subject matches is dropped, the other arrives.
         var smtp = new SmtpClientSimulator();
         smtp.Send("sender@example.test", account.Address, "about restrule-drop today", "dropped");
         smtp.Send("sender@example.test", account.Address, "kept", "kept");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "INBOX", 1);

         // Replaced whole: a header criterion and a move. The old criterion
         // and action are gone (new ids), and the running server moves the
         // next matching message.
         string movedName = RulePrefix + "move";
         (int putStatus, string putBody) = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(movedName,
               "{\"field\":\"header\",\"header\":\"Subject\",\"match\":\"equals\",\"value\":\"restrule-move\"}",
               "{\"type\":\"move_to_folder\",\"folder\":\"RestRuleFolder\"}"));
         Assert.AreEqual(200, putStatus, putBody);
         StringAssert.Contains("\"id\":" + id + ",\"name\":\"" + movedName + "\"", putBody);
         StringAssert.Contains("{\"field\":\"header\",\"header\":\"Subject\",\"match\":\"equals\",\"value\":\"restrule-move\"}", putBody);
         StringAssert.Contains("{\"type\":\"move_to_folder\",\"value\":\"RestRuleFolder\",\"folder\":\"RestRuleFolder\"}", putBody);

         rule = RuleOverCom(id);
         Assert.IsNotNull(rule);
         Assert.AreEqual(movedName, rule.Name);
         Assert.AreEqual(1, rule.Criterias.Count, "The old criterion was replaced, not added to.");
         criterion = rule.Criterias[0];
         Assert.IsFalse(criterion.UsePredefined);
         Assert.AreEqual("Subject", criterion.HeaderField);
         Assert.AreEqual(eRuleMatchType.eMTEquals, criterion.MatchType);
         Assert.AreEqual("restrule-move", criterion.MatchValue);
         Assert.AreNotEqual(firstCriterionId, criterion.ID, "PUT deletes the old criteria and creates the new.");
         Assert.AreEqual(1, rule.Actions.Count);
         Assert.AreEqual(eRuleActionType.eRAMoveToImapFolder, rule.Actions[0].Type);
         Assert.AreEqual("RestRuleFolder", rule.Actions[0].IMAPFolder);
         Assert.AreNotEqual(firstActionId, rule.Actions[0].ID, "PUT deletes the old actions and creates the new.");

         smtp.Send("sender@example.test", account.Address, "restrule-move", "moved");
         smtp.Send("sender@example.test", account.Address, "about restrule-drop today", "no longer dropped");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "RestRuleFolder", 1);
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "INBOX", 2);

         // Deleted: COM does not find it, the listing does not show it, and
         // the next matching message lands in the INBOX.
         (int deleteStatus, string deleteBody) = Http("DELETE", "/api/v1/rules/" + id);
         Assert.AreEqual(200, deleteStatus, deleteBody);
         StringAssert.Contains("\"deleted\":true", deleteBody);
         Assert.IsNull(RuleOverCom(id), "The rule must be gone through COM after DELETE.");
         StringAssert.DoesNotContain("\"id\":" + id + ",", Http("GET", "/api/v1/rules").body);

         smtp.Send("sender@example.test", account.Address, "restrule-move", "not moved any more");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "INBOX", 3);
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "RestRuleFolder", 1);

         Assert.AreEqual(404, Http("DELETE", "/api/v1/rules/" + id).status);
         Assert.AreEqual(404, Http("PUT", "/api/v1/rules/" + id, RuleBody(name, "", "")).status);
      }

      [Test]
      [Description("Every action type, with the parameters it takes, is stored as COM stores it and read back field by field; the actions keep the order of the array.")]
      public void EveryActionTypeRoundTripsThroughCom()
      {
         Route route = _settings.Routes.Add();
         route.DomainName = RouteDomain;
         route.TargetSMTPHost = "192.0.2.10";
         route.TargetSMTPPort = 25;
         route.NumberOfTries = 1;
         route.MinutesBetweenTry = 5;
         route.Save();

         string name = RulePrefix + "every-action";
         string actions =
            "{\"type\":\"forward\",\"to\":\"forwardee@example.test\"}," +
            "{\"type\":\"reply\",\"from_name\":\"Auto Reply\",\"from_address\":\"noreply@example.test\",\"subject\":\"Re: yours\",\"body\":\"Thank you.\\nWe will answer.\"}," +
            "{\"type\":\"move_to_folder\",\"folder\":\"Filed.Sub\"}," +
            "{\"type\":\"script_function\",\"script_function\":\"OnRule\"}," +
            "{\"type\":\"set_header\",\"header\":\"X-Rest-Rule\",\"value\":\"applied\"}," +
            "{\"type\":\"send_using_route\",\"route_id\":" + route.ID + "}," +
            "{\"type\":\"copy\"}," +
            "{\"type\":\"bind_to_address\",\"value\":\"127.0.0.1\"}," +
            "{\"type\":\"stop\"}," +
            "{\"type\":\"delete\"}";

         // Inactive on purpose: this rule is about storage, and a forward and
         // a reply that fired on the suite's mail would be a nuisance.
         (int status, string body) created = Http("POST", "/api/v1/rules",
            RuleBody(name, "{\"field\":\"from\",\"match\":\"wildcard\",\"value\":\"*@example.test\"}", actions, active: false, allCriteria: false));
         Assert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("\"active\":false,\"all_criteria\":false", created.body);
         StringAssert.Contains("{\"type\":\"forward\",\"value\":\"forwardee@example.test\",\"to\":\"forwardee@example.test\",\"abort_spam_flagged\":false}", created.body);
         StringAssert.Contains("{\"type\":\"reply\",\"value\":\"\",\"from_name\":\"Auto Reply\",\"from_address\":\"noreply@example.test\",\"subject\":\"Re: yours\",\"body\":\"Thank you.\\nWe will answer.\",\"abort_spam_flagged\":false}", created.body);
         StringAssert.Contains("{\"type\":\"set_header\",\"value\":\"applied\",\"header\":\"X-Rest-Rule\"}", created.body);
         StringAssert.Contains("{\"type\":\"send_using_route\",\"value\":\"\",\"route_id\":" + route.ID + "}", created.body);
         StringAssert.Contains("{\"type\":\"bind_to_address\",\"value\":\"127.0.0.1\"}", created.body);
         int id = ExtractId(created.body);

         Rule rule = RuleOverCom(id);
         Assert.IsNotNull(rule);
         Assert.IsFalse(rule.Active);
         Assert.IsFalse(rule.UseAND);
         Assert.AreEqual(1, rule.Criterias.Count);
         Assert.AreEqual(eRulePredefinedField.eFTFrom, rule.Criterias[0].PredefinedField);
         Assert.AreEqual(eRuleMatchType.eMTWildcard, rule.Criterias[0].MatchType);
         Assert.AreEqual("*@example.test", rule.Criterias[0].MatchValue);

         Assert.AreEqual(10, rule.Actions.Count);

         Assert.AreEqual(eRuleActionType.eRAForwardEmail, rule.Actions[0].Type);
         Assert.AreEqual("forwardee@example.test", rule.Actions[0].To);

         Assert.AreEqual(eRuleActionType.eRAReply, rule.Actions[1].Type);
         Assert.AreEqual("Auto Reply", rule.Actions[1].FromName);
         Assert.AreEqual("noreply@example.test", rule.Actions[1].FromAddress);
         Assert.AreEqual("Re: yours", rule.Actions[1].Subject);
         Assert.AreEqual("Thank you.\nWe will answer.", rule.Actions[1].Body);

         Assert.AreEqual(eRuleActionType.eRAMoveToImapFolder, rule.Actions[2].Type);
         Assert.AreEqual("Filed.Sub", rule.Actions[2].IMAPFolder);

         Assert.AreEqual(eRuleActionType.eRARunScriptFunction, rule.Actions[3].Type);
         Assert.AreEqual("OnRule", rule.Actions[3].ScriptFunction);

         Assert.AreEqual(eRuleActionType.eRASetHeaderValue, rule.Actions[4].Type);
         Assert.AreEqual("X-Rest-Rule", rule.Actions[4].HeaderName);
         Assert.AreEqual("applied", rule.Actions[4].Value);

         Assert.AreEqual(eRuleActionType.eRASendUsingRoute, rule.Actions[5].Type);
         Assert.AreEqual(route.ID, rule.Actions[5].RouteID);

         Assert.AreEqual(eRuleActionType.eRACreateCopy, rule.Actions[6].Type);

         Assert.AreEqual(eRuleActionType.eRABindToAddress, rule.Actions[7].Type);
         Assert.AreEqual("127.0.0.1", rule.Actions[7].Value);

         Assert.AreEqual(eRuleActionType.eRAStopRuleProcessing, rule.Actions[8].Type);
         Assert.AreEqual(eRuleActionType.eRADeleteEmail, rule.Actions[9].Type);

         // The listing's generic "value" is accepted as the parameter it
         // stands for, so a client can send back what it read.
         (int putStatus, string putBody) = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(name, "{\"field\":\"to\",\"match\":\"not_contains\",\"value\":\"x\"}",
               "{\"type\":\"forward\",\"value\":\"other@example.test\"},{\"type\":\"script_function\",\"value\":\"Other\"}", active: false));
         Assert.AreEqual(200, putStatus, putBody);
         rule = RuleOverCom(id);
         Assert.AreEqual(2, rule.Actions.Count);
         Assert.AreEqual("other@example.test", rule.Actions[0].To);
         Assert.AreEqual("Other", rule.Actions[1].ScriptFunction);
         Assert.AreEqual(eRuleMatchType.eMTNotContains, rule.Criterias[0].MatchType);
      }

      [Test]
      [Description("A malformed body, a missing name, an unknown word, an unknown key, a parameter the type does not take, one it needs and lacks, a regex that does not compile and a value over its column are each 400 naming the problem, and none of them creates or changes a rule.")]
      public void RefusalsChangeNothing()
      {
         int before = _application.Rules.Count;

         void Refused(string body, string expectedWords)
         {
            (int status, string answer) = Http("POST", "/api/v1/rules", body);
            Assert.AreEqual(400, status, "Expected 400 for " + body + " but got " + status + ": " + answer);
            StringAssert.Contains(expectedWords, answer);
         }

         string criterion = "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}";

         Refused("{\"active\":true}", "name is required");
         Refused(RuleBody("", criterion, ""), "name is required");
         Refused(RuleBody(RulePrefix + new string('n', 101), criterion, ""), "name is longer than 100 characters");
         Refused("not json", "not JSON");
         Refused("[1,2]", "must be a JSON object");
         Refused("{\"name\":\"" + RulePrefix + "x\",\"criteria\":{}}", "criteria must be an array");
         Refused("{\"name\":\"" + RulePrefix + "x\",\"active\":\"yes\"}", "active must be true or false");

         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"sujbect\",\"match\":\"contains\",\"value\":\"x\"}", ""), "unknown field 'sujbect'");
         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"subject\",\"match\":\"includes\",\"value\":\"x\"}", ""), "unknown match type 'includes'");
         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"subject\",\"match\":\"contains\",\"vlaue\":\"x\"}", ""), "unknown key 'vlaue'");
         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"subject\",\"match\":\"contains\"}", ""), "value is required");
         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"header\",\"match\":\"contains\",\"value\":\"x\"}", ""), "header is required when field is header");
         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"subject\",\"header\":\"X-Y\",\"match\":\"contains\",\"value\":\"x\"}", ""), "header is only used when field is header");
         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"subject\",\"match\":\"regex\",\"value\":\"[unclosed\"}", ""), "does not compile");
         Refused(RuleBody(RulePrefix + "x", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"" + new string('v', 2001) + "\"}", ""), "2000 characters");

         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"explode\"}"), "unknown action type 'explode'");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"delete\",\"colour\":\"red\"}"), "unknown key 'colour'");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"delete\",\"to\":\"a@example.test\"}"), "to is not a parameter of a delete action");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"forward\"}"), "to is required for a forward action");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"move_to_folder\"}"), "folder is required for a move_to_folder action");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"script_function\"}"), "script_function is required");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"set_header\",\"value\":\"v\"}"), "header is required for a set_header action");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"send_using_route\"}"), "route_id is required");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"send_using_route\",\"route_id\":\"7\"}"), "route_id must be a number");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"bind_to_address\"}"), "value is required for a bind_to_address action");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"stop\",\"value\":\"x\"}"), "a stop action has no value");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"forward\",\"to\":\"a@example.test\",\"value\":\"b@example.test\"}"), "to and value disagree");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"set_header\",\"header\":\"" + new string('h', 81) + "\",\"value\":\"v\"}"), "header is longer than 80 characters");
         Refused(RuleBody(RulePrefix + "x", criterion, "{\"type\":\"forward\",\"to\":\"" + new string('t', 256) + "\"}"), "to is longer than 255 characters");
         Refused(RuleBody(RulePrefix + "x", criterion, "\"delete\""), "actions[0]: must be an object");

         Assert.AreEqual(before, _application.Rules.Count, "No refused POST may have created a rule.");

         // A refused PUT leaves the rule exactly as it was.
         string name = RulePrefix + "kept";
         (int status, string created) = Http("POST", "/api/v1/rules", RuleBody(name, criterion, "{\"type\":\"delete\"}", active: false));
         Assert.AreEqual(201, status, created);
         int id = ExtractId(created);

         (int putStatus, string putBody) = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(RulePrefix + "changed", "{\"field\":\"subject\",\"match\":\"regex\",\"value\":\"(\"}", ""));
         Assert.AreEqual(400, putStatus, putBody);
         StringAssert.Contains("does not compile", putBody);

         Rule rule = RuleOverCom(id);
         Assert.AreEqual(name, rule.Name);
         Assert.AreEqual(1, rule.Criterias.Count);
         Assert.AreEqual("x", rule.Criterias[0].MatchValue);
         Assert.AreEqual(1, rule.Actions.Count);
         Assert.AreEqual(eRuleActionType.eRADeleteEmail, rule.Actions[0].Type);

         // Unknown ids, and ids that are not numbers, are 404.
         Assert.AreEqual(404, Http("PUT", "/api/v1/rules/999999999", RuleBody(name, criterion, "")).status);
         Assert.AreEqual(404, Http("DELETE", "/api/v1/rules/999999999").status);
         Assert.AreEqual(404, Http("DELETE", "/api/v1/rules/abc").status);
         Assert.AreEqual(404, Http("DELETE", "/api/v1/rules/" + id + "/more").status);
      }

      [Test]
      [Description("An account's rule is not a global rule: PUT and DELETE on its id are 404 and leave it as it was, and the listing does not show it.")]
      public void AccountRuleIsNotReachable()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruleowner@example.test", "test");

         Rule accountRule = account.Rules.Add();
         accountRule.Name = RulePrefix + "account-owned";
         accountRule.Active = true;
         RuleCriteria criterion = accountRule.Criterias.Add();
         criterion.UsePredefined = true;
         criterion.PredefinedField = eRulePredefinedField.eFTSubject;
         criterion.MatchType = eRuleMatchType.eMTContains;
         criterion.MatchValue = "owned";
         criterion.Save();
         RuleAction action = accountRule.Actions.Add();
         action.Type = eRuleActionType.eRADeleteEmail;
         action.Save();
         accountRule.Save();
         int id = accountRule.ID;
         Assert.Greater(id, 0);

         Assert.AreEqual(404, Http("PUT", "/api/v1/rules/" + id,
            RuleBody(RulePrefix + "taken-over", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}", "")).status);
         Assert.AreEqual(404, Http("DELETE", "/api/v1/rules/" + id).status);
         StringAssert.DoesNotContain("\"id\":" + id + ",", Http("GET", "/api/v1/rules").body);

         Rule again = account.Rules.get_ItemByDBID(id);
         Assert.IsNotNull(again);
         Assert.AreEqual(RulePrefix + "account-owned", again.Name);
         Assert.AreEqual(1, again.Criterias.Count);
         Assert.AreEqual("owned", again.Criterias[0].MatchValue);
         Assert.AreEqual(1, again.Actions.Count);
      }

      [Test]
      [Description("The outbound content policy's words round-trip through the listing: the four conditions and the warn_sender action with its text, and an unknown detector value is still just a value")]
      public void TheContentPolicyWordsRoundTrip()
      {
         string name = RulePrefix + "content-policy";
         string criteria =
            "{\"field\":\"sensitive_content\",\"match\":\"contains\",\"value\":\"card_number\"}," +
            "{\"field\":\"attachment_type\",\"match\":\"not_contains\",\"value\":\"executable\"}," +
            "{\"field\":\"external_recipient\",\"match\":\"contains\",\"value\":\"@\"}," +
            "{\"field\":\"content\",\"match\":\"regex\",\"value\":\"(?s).*PRJ-[0-9]{4}.*\"}";
         string actions = "{\"type\":\"warn_sender\",\"value\":\"Use the portal.\"},{\"type\":\"quarantine\"}";

         (int status, string body) created = Http("POST", "/api/v1/rules", RuleBody(name, criteria, actions, active: false, allCriteria: true));
         Assert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("{\"field\":\"sensitive_content\",\"header\":\"\",\"match\":\"contains\",\"value\":\"card_number\"}", created.body);
         StringAssert.Contains("{\"field\":\"attachment_type\",\"header\":\"\",\"match\":\"not_contains\",\"value\":\"executable\"}", created.body);
         StringAssert.Contains("{\"field\":\"external_recipient\",\"header\":\"\",\"match\":\"contains\",\"value\":\"@\"}", created.body);
         StringAssert.Contains("{\"field\":\"content\",\"header\":\"\",\"match\":\"regex\",\"value\":\"(?s).*PRJ-[0-9]{4}.*\"}", created.body);
         StringAssert.Contains("{\"type\":\"warn_sender\",\"value\":\"Use the portal.\"}", created.body);
         int id = ExtractId(created.body);

         // What COM reads back is the numbers the server stored: the fields
         // 10 to 13 and the action 15 of the IDL.
         Rule rule = RuleOverCom(id);
         Assert.IsNotNull(rule, "The rule exists in the collection the Control Panel edits.");
         Assert.AreEqual(4, rule.Criterias.Count);
         Assert.AreEqual(10, (int) rule.Criterias[0].PredefinedField);
         Assert.AreEqual(11, (int) rule.Criterias[1].PredefinedField);
         Assert.AreEqual(12, (int) rule.Criterias[2].PredefinedField);
         Assert.AreEqual(13, (int) rule.Criterias[3].PredefinedField);
         Assert.AreEqual(15, (int) rule.Actions[0].Type);
         Assert.AreEqual("Use the portal.", rule.Actions[0].Value);

         // A warn_sender with no text is allowed: the notice names the rule anyway.
         (int status, string body) replaced = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(name, "{\"field\":\"external_recipient\",\"match\":\"equals\",\"value\":\"\"}", "{\"type\":\"warn_sender\"}", active: false, allCriteria: true));
         Assert.AreEqual(200, replaced.status, replaced.body);
         StringAssert.Contains("{\"type\":\"warn_sender\",\"value\":\"\"}", replaced.body);

         // A parameter the action does not take is refused, as for every other type.
         (int status, string body) refused = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(name, "{\"field\":\"content\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"warn_sender\",\"to\":\"someone@example.test\"}", active: false, allCriteria: true));
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("to is not a parameter of a warn_sender action", refused.body);

         // A line break in a name or a reject text is refused: the name goes
         // into a 550 line and the notice's headers, the text is the 550 line.
         // A warn_sender text is a body, and may have paragraphs.
         (int status, string body) splitName = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(name + "\\r\\nX-Injected: yes", "{\"field\":\"content\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"warn_sender\"}", active: false, allCriteria: true));
         Assert.AreEqual(400, splitName.status, splitName.body);
         StringAssert.Contains("name must not contain control characters", splitName.body);

         (int status, string body) splitReply = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(name, "{\"field\":\"content\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"reject\",\"value\":\"No.\\r\\n250 OK\"}", active: false, allCriteria: true));
         Assert.AreEqual(400, splitReply.status, splitReply.body);
         StringAssert.Contains("value must not contain control characters", splitReply.body);

         (int status, string body) paragraphs = Http("PUT", "/api/v1/rules/" + id,
            RuleBody(name, "{\"field\":\"content\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"warn_sender\",\"value\":\"Use the portal.\\nThank you.\"}", active: false, allCriteria: true));
         Assert.AreEqual(200, paragraphs.status, paragraphs.body);
         StringAssert.Contains("{\"type\":\"warn_sender\",\"value\":\"Use the portal.\\nThank you.\"}", paragraphs.body);

         Assert.AreEqual(200, Http("DELETE", "/api/v1/rules/" + id).status);
      }

      [Test]
      [Description("A domain's rules are its own over REST: created under the domain with the new condition and the four new actions, listed there and not among the server's, replaced and deleted there")]
      public void ADomainsRulesAreItsOwnOverRest()
      {
         string name = RulePrefix + "domain-flow";
         string actions =
            "{\"type\":\"add_disclaimer\",\"body\":\"Confidential.\"}," +
            "{\"type\":\"reject\",\"value\":\"Not here.\"}," +
            "{\"type\":\"require_tls\"}," +
            "{\"type\":\"quarantine\"}";
         (int status, string body) created = Http("POST", "/api/v1/domains/" + _domain.Name + "/rules",
            RuleBody(name, "{\"field\":\"attachment_name\",\"match\":\"wildcard\",\"value\":\"*.zip\"}", actions, active: false, allCriteria: true));
         Assert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("\"field\":\"attachment_name\"", created.body);
         StringAssert.Contains("{\"type\":\"add_disclaimer\",\"value\":\"Confidential.\",\"body\":\"Confidential.\"}", created.body);
         StringAssert.Contains("{\"type\":\"reject\",\"value\":\"Not here.\"}", created.body);
         StringAssert.Contains("{\"type\":\"require_tls\",\"value\":\"\"}", created.body);
         StringAssert.Contains("{\"type\":\"quarantine\",\"value\":\"\"}", created.body);
         int id = ExtractId(created.body);

         StringAssert.Contains("\"name\":\"" + name + "\"", Http("GET", "/api/v1/domains/" + _domain.Name + "/rules").body, "Listed under the domain.");
         StringAssert.DoesNotContain(name, Http("GET", "/api/v1/rules").body, "And not among the server's.");
         Assert.AreEqual(404, Http("PUT", "/api/v1/rules/" + id, RuleBody(name, "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"delete\"}", active: false, allCriteria: true)).status,
            "The server's route does not reach a domain's rule.");

         (int status, string body) replaced = Http("PUT", "/api/v1/domains/" + _domain.Name + "/rules/" + id,
            RuleBody(name + "-2", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"delete\"}", active: false, allCriteria: true));
         Assert.AreEqual(200, replaced.status, replaced.body);
         StringAssert.Contains("\"name\":\"" + name + "-2\"", replaced.body);

         Assert.AreEqual(400, Http("POST", "/api/v1/domains/" + _domain.Name + "/rules",
            RuleBody(name + "-3", "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}", "{\"type\":\"add_disclaimer\"}", active: false, allCriteria: true)).status,
            "A disclaimer needs its text.");
         Assert.AreEqual(404, Http("GET", "/api/v1/domains/not-hosted.example/rules").status);

         Assert.AreEqual(200, Http("DELETE", "/api/v1/domains/" + _domain.Name + "/rules/" + id).status);
         Assert.AreEqual(404, Http("DELETE", "/api/v1/domains/" + _domain.Name + "/rules/" + id).status);

         // A key restricted to the domain reaches the domain's rules and no other domain's.
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("other-flow.test");
         (string scopedId, string scopedKey) = CreateKey("restrule - domain scoped", "full", _domain.Name);
         try
         {
            string simple = "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"x\"}";
            (int status, string body) mine = Bearer("POST", "/api/v1/domains/" + _domain.Name + "/rules", scopedKey, RuleBody(name + "-mine", simple, "{\"type\":\"delete\"}", active: false));
            Assert.AreEqual(201, mine.status, mine.body);
            Assert.AreEqual(200, Bearer("GET", "/api/v1/domains/" + _domain.Name + "/rules", scopedKey).status);
            Assert.AreEqual(403, Bearer("GET", "/api/v1/domains/" + other.Name + "/rules", scopedKey).status, "Another domain's rules are not the key's to read.");
            Assert.AreEqual(403, Bearer("POST", "/api/v1/domains/" + other.Name + "/rules", scopedKey, RuleBody(name + "-theirs", simple, "{\"type\":\"delete\"}", active: false)).status);
            Assert.AreEqual(403, Bearer("POST", "/api/v1/rules", scopedKey, RuleBody(name + "-global", simple, "{\"type\":\"delete\"}", active: false)).status, "Nor are the server's.");
            Assert.AreEqual(200, Bearer("DELETE", "/api/v1/domains/" + _domain.Name + "/rules/" + ExtractId(mine.body), scopedKey).status);
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + scopedId);
         }
      }

      [Test]
      [Description("A key restricted to named domains cannot write a global rule whatever its scope; a read-only key cannot; an unrestricted full key can; no credential is 401.")]
      public void ScopedAndReadOnlyKeysAreRefused()
      {
         string name = RulePrefix + "keys";
         string criterion = "{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"keys\"}";

         (string scopedId, string scopedKey) = CreateKey("restrule - scoped", "full", "example.test");
         (string readOnlyId, string readOnlyKey) = CreateKey("restrule - readonly", "readonly", null);
         (string fullId, string fullKey) = CreateKey("restrule - full", "full", null);

         try
         {
            int before = _application.Rules.Count;

            Assert.AreEqual(403, Bearer("POST", "/api/v1/rules", scopedKey, RuleBody(name, criterion, "")).status,
               "A domain-scoped key must not create a global rule.");
            Assert.AreEqual(403, Bearer("POST", "/api/v1/rules", readOnlyKey, RuleBody(name, criterion, "")).status,
               "A read-only key must not create a global rule.");
            Assert.AreEqual(401, Http("POST", "/api/v1/rules", null, RuleBody(name, criterion, "")).status);
            Assert.AreEqual(before, _application.Rules.Count, "No refused POST may have created a rule.");

            (int status, string body) created = Bearer("POST", "/api/v1/rules", fullKey, RuleBody(name, criterion, "{\"type\":\"delete\"}", active: false));
            Assert.AreEqual(201, created.status, created.body);
            int id = ExtractId(created.body);
            Assert.IsNotNull(RuleOverCom(id));

            Assert.AreEqual(403, Bearer("PUT", "/api/v1/rules/" + id, scopedKey, RuleBody(RulePrefix + "changed", criterion, "")).status);
            Assert.AreEqual(403, Bearer("DELETE", "/api/v1/rules/" + id, scopedKey).status);
            Assert.AreEqual(403, Bearer("PUT", "/api/v1/rules/" + id, readOnlyKey, RuleBody(RulePrefix + "changed", criterion, "")).status);
            Assert.AreEqual(403, Bearer("DELETE", "/api/v1/rules/" + id, readOnlyKey).status);
            Assert.AreEqual(name, RuleOverCom(id).Name, "The refused PUTs must not have changed the rule.");

            Assert.AreEqual(200, Bearer("PUT", "/api/v1/rules/" + id, fullKey, RuleBody(RulePrefix + "changed", criterion, "", active: false)).status);
            Assert.AreEqual(RulePrefix + "changed", RuleOverCom(id).Name);
            Assert.AreEqual(200, Bearer("DELETE", "/api/v1/rules/" + id, fullKey).status);
            Assert.IsNull(RuleOverCom(id));
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + scopedId);
            Http("DELETE", "/api/v1/apikeys/" + readOnlyId);
            Http("DELETE", "/api/v1/apikeys/" + fullId);
         }
      }

      private static (string id, string key) CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";
         (int status, string created) = Http("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + created);
         return (Extract(created, "id"), Extract(created, "key"));
      }

      // The string value of a top-level JSON property, enough for the bodies this API returns.
      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
         Assert.IsTrue(match.Success, "No '" + key + "' in: " + json);
         return match.Groups[1].Value;
      }

      // The rule's own id: the first "id" in the body, which is the rule's
      // because the entry starts with it.
      private static int ExtractId(string json)
      {
         Match match = Regex.Match(json, "^\\{\"id\":(\\d+),");
         Assert.IsTrue(match.Success, "No leading id in: " + json);
         return int.Parse(match.Groups[1].Value);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return Http(method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) Bearer(string method, string path, string token, string requestBody = null)
      {
         return Http(method, path, "Bearer " + token, requestBody);
      }

      // Issues one HTTP/1.0 request against the REST listener and returns the
      // parsed status code and body. authorization is the complete header value
      // or null to send none. The connect is retried briefly to absorb the
      // listener bind race right after a reinitialize.
      private static (int status, string body) Http(string method, string path, string authorization, string requestBody)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int statusCode = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               return (statusCode, body);
            }
         }
      }
   }
}
