// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using System.Text.Json;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The sample domain: POST, GET and DELETE /api/v1/setup/sample-domain, and the
   ///    same three documents over COM as Utilities.CreateSampleDomain,
   ///    .GetSampleDomain and .RemoveSampleDomain.
   ///
   ///    What is under test is that the sample is REAL - the three accounts exist,
   ///    receive mail and sign in, the distribution list expands, the rule fires and
   ///    creates the folder it names - and that the removal takes exactly what the
   ///    sample made and reports it.
   ///
   ///    THE ONE AWKWARDNESS, and the reason for the deletes below: the sample domain
   ///    is example.test, which is also this suite's own domain, made fresh by
   ///    PerformBasicSetup before every test. So a test that wants the sample must
   ///    first delete the suite's domain - and the test that proves the refusal wants
   ///    the suite's domain exactly where it is, because a domain of that name that
   ///    this server did not make is the case the marker exists for.
   ///
   ///    THE PASSWORD is not a constant here, because it is not a constant there: the
   ///    server mints one per creation and says it once, in the answer to the call
   ///    that made the sample. Every test that signs a mailbox in takes it from that
   ///    answer, and one of them proves that no later read gives it back - which is
   ///    the whole point of minting it.
   /// </summary>
   [TestFixture]
   public class RestApiSampleDomain : TestFixtureBase
   {
      private static int RestPort = 9696;
      private const string AdminPassword = "testar";
      private const string Path = "/api/v1/setup/sample-domain";

      private const string Domain = "example.test";
      private const string Alice = "alice@example.test";
      private const string Bob = "bob@example.test";
      private const string Carol = "carol@example.test";
      private const string List = "everyone@example.test";

      // The eight sample messages, expanded: alice 4 in the INBOX and one filed by
      // her own rule, bob 5, carol 4.
      private const int AliceInbox = 4;
      private const int BobInbox = 5;
      private const int CarolInbox = 4;
      private const int Deliveries = 14;

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
         try
         {
            // The queue first, always. A sample deleted while its own mail is still
            // queued leaves messages whose recipient account has gone, and
            // LocalDelivery correctly reports HM5165 against whatever test runs
            // next - the shape TestFixtureBase has a paragraph about.
            Drain_();

            // Whatever the test did, this server must not be left holding a sample -
            // nor a marker naming a domain the next test's ClearDomains removed. The
            // refusals are expected and say nothing here.
            Http("DELETE", Path);
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("The sample is made whole: the domain, three accounts that receive mail and sign in, a list of the three that expands, and a rule that files the invoice into a folder it creates itself.")]
      public void MakesADomainWithMailInIt()
      {
         MakeRoom_();

         (int status, string body) made = Http("POST", Path);
         ClassicAssert.AreEqual(201, made.status, made.body);

         using JsonDocument report = JsonDocument.Parse(made.body);
         JsonElement root = report.RootElement;
         ClassicAssert.IsTrue(root.GetProperty("created").GetBoolean());
         ClassicAssert.AreEqual(Domain, root.GetProperty("domain").GetString());
         ClassicAssert.Greater(root.GetProperty("domain_id").GetInt64(), 0);

         // The password the server minted: twenty Crockford base32 characters in
         // groups of five, as AppPassword::GenerateSecret makes one. It is asserted
         // by shape rather than by value, because there is no value to know.
         string password = root.GetProperty("password").GetString();
         StringAssert.IsMatch("^[2-9A-HJ-NP-TV-Z]{5}-[2-9A-HJ-NP-TV-Z]{5}-[2-9A-HJ-NP-TV-Z]{5}-[2-9A-HJ-NP-TV-Z]{5}$", password);
         StringAssert.Contains(password, root.GetProperty("message").GetString(),
            "The sentence carries the password, because it is what every command line and dialog prints.");

         ClassicAssert.AreEqual(3, root.GetProperty("accounts_created").GetInt32());
         ClassicAssert.AreEqual(3, root.GetProperty("list_members").GetInt32());
         ClassicAssert.AreEqual(1, root.GetProperty("rules").GetInt32());
         ClassicAssert.AreEqual(8, root.GetProperty("messages").GetInt32());
         ClassicAssert.AreEqual(Deliveries, root.GetProperty("deliveries").GetInt32(),
            "The list expands into the envelope, which is what makes the sample's list visibly a list.");
         StringAssert.Contains(Domain, root.GetProperty("message").GetString());

         // The domain over COM, as an administrator would find it. get_ItemByName
         // rather than the ItemByName[...] parameterized property: both are the same
         // call over COM, and only this spelling exists in the Linux shims, which
         // compile this same file.
         Domain sample = _application.Domains.get_ItemByName(Domain);
         ClassicAssert.IsNotNull(sample);
         ClassicAssert.IsTrue(sample.Active);
         ClassicAssert.AreEqual(3, sample.Accounts.Count);
         ClassicAssert.AreEqual(Alice, sample.Postmaster);

         // The three mailboxes sign in with the one password the creation answered
         // with, and have the mail the sample sent them. This is the whole point of
         // the row: every page has something in it.
         Pop3ClientSimulator.AssertMessageCount(Alice, password, AliceInbox);
         Pop3ClientSimulator.AssertMessageCount(Bob, password, BobInbox);
         Pop3ClientSimulator.AssertMessageCount(Carol, password, CarolInbox);

         // The rule, and the folder it made when it fired. The folder is not created
         // by the sample: if it is there, the rule ran.
         Account alice = sample.Accounts.get_ItemByAddress(Alice);
         ClassicAssert.AreEqual(1, alice.Rules.Count);
         Rule rule = alice.Rules[0];
         ClassicAssert.IsTrue(rule.Active);
         ClassicAssert.AreEqual(1, rule.Criterias.Count);
         ClassicAssert.AreEqual("Invoice", rule.Criterias[0].MatchValue);
         ClassicAssert.AreEqual(1, rule.Actions.Count);
         ClassicAssert.AreEqual("Invoices", rule.Actions[0].IMAPFolder);

         ImapClientSimulator.AssertMessageCount(Alice, password, "Invoices", 1);

         // Nothing left in the queue: the eight messages were all delivered, which
         // is what the three counts above and this together say.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // The list, and the three members the deliveries above already proved it
         // expands to.
         DistributionList list = sample.DistributionLists.get_ItemByAddress(List);
         ClassicAssert.IsNotNull(list);
         ClassicAssert.IsTrue(list.Active);
         ClassicAssert.AreEqual(3, list.Recipients.Count);

         // The state route now says the sample is ours, with the id the report gave -
         // and does NOT say the password. That is the assertion the row's security
         // finding turns on: GET needs only view_configuration, and three mailboxes
         // that may relay must not have their credential readable with it.
         using JsonDocument state = JsonDocument.Parse(Get(Path));
         ClassicAssert.AreEqual("present", state.RootElement.GetProperty("state").GetString());
         ClassicAssert.AreEqual(root.GetProperty("domain_id").GetInt64(), state.RootElement.GetProperty("domain_id").GetInt64());
         ClassicAssert.IsTrue(state.RootElement.GetProperty("exists").GetBoolean());
         ClassicAssert.IsFalse(state.RootElement.TryGetProperty("password", out _),
            "The state document must never carry the password: it is shown once, by the creation.");
         ClassicAssert.IsTrue(state.RootElement.GetProperty("password_shown_once").GetBoolean());
         StringAssert.DoesNotContain(password, Get(Path),
            "Nothing in the state document may be the password, under any name.");
      }

      [Test]
      [Description("An example.test this server did not make - the suite's own domain - is never created over and never removed: both answer 409, the state is foreign, and the domain is untouched.")]
      public void RefusesAnExampleTestItDidNotMake()
      {
         // The suite's own domain is here, made by PerformBasicSetup. It is exactly
         // the case the marker exists for.
         int accountsBefore = _domain.Accounts.Count;

         using (JsonDocument state = JsonDocument.Parse(Get(Path)))
         {
            ClassicAssert.AreEqual("foreign", state.RootElement.GetProperty("state").GetString());
            ClassicAssert.IsTrue(state.RootElement.GetProperty("exists").GetBoolean());
            ClassicAssert.AreEqual(0, state.RootElement.GetProperty("domain_id").GetInt64(),
               "A domain the sample did not make has no sample id, whatever it is called.");
         }

         (int status, string body) refusedCreate = Http("POST", Path);
         ClassicAssert.AreEqual(409, refusedCreate.status, refusedCreate.body);
         StringAssert.Contains("did not make it", refusedCreate.body);

         (int status, string body) refusedRemove = Http("DELETE", Path);
         ClassicAssert.AreEqual(409, refusedRemove.status, refusedRemove.body);
         StringAssert.Contains("was not made by the sample", refusedRemove.body);

         // Untouched, which is the assertion that matters: a refusal that deleted
         // somebody's domain would answer 409 just the same.
         Domain still = _application.Domains.get_ItemByName(Domain);
         ClassicAssert.IsNotNull(still);
         ClassicAssert.AreEqual(accountsBefore, still.Accounts.Count);
      }

      [Test]
      [Description("The removal counts what went - mailboxes, lists, rules and messages - takes the domain with everything in it, and a second removal is refused because there is nothing left.")]
      public void RemoveReportsWhatWentAndLeavesNothing()
      {
         MakeRoom_();

         (int status, string body) made = Http("POST", Path);
         ClassicAssert.AreEqual(201, made.status, made.body);

         string password;
         using (JsonDocument report = JsonDocument.Parse(made.body))
            password = report.RootElement.GetProperty("password").GetString();

         // Wait for all of the mail before removing it, so the count is the whole
         // sample rather than whatever had been delivered when the DELETE arrived.
         // The deliverer takes the queue by size rather than in order, so one
         // mailbox being full says nothing about another.
         Pop3ClientSimulator.AssertMessageCount(Alice, password, AliceInbox);
         Pop3ClientSimulator.AssertMessageCount(Bob, password, BobInbox);
         Pop3ClientSimulator.AssertMessageCount(Carol, password, CarolInbox);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         (int status, string body) removed = Http("DELETE", Path);
         ClassicAssert.AreEqual(200, removed.status, removed.body);

         using (JsonDocument report = JsonDocument.Parse(removed.body))
         {
            JsonElement root = report.RootElement;
            ClassicAssert.IsTrue(root.GetProperty("removed").GetBoolean());
            ClassicAssert.AreEqual(Domain, root.GetProperty("domain").GetString());
            ClassicAssert.AreEqual(3, root.GetProperty("accounts").GetInt64());
            ClassicAssert.AreEqual(1, root.GetProperty("lists").GetInt64());
            ClassicAssert.AreEqual(1, root.GetProperty("rules").GetInt64());
            ClassicAssert.AreEqual(Deliveries, root.GetProperty("messages").GetInt64(),
               "Every delivered copy is counted, including the one the rule filed.");

            // Nothing was left in the queue to drop, because the test waited for
            // the mail above. The count is still reported, and is what a removal
            // that came straight after the creation would have to fill in.
            ClassicAssert.AreEqual(0, root.GetProperty("queued").GetInt64(),
               "The queue was empty before the DELETE, so there was nothing in it to drop.");
            StringAssert.Contains("went with it", root.GetProperty("message").GetString());
            StringAssert.DoesNotContain("delivery queue", root.GetProperty("message").GetString(),
               "The queue clause is said only when there was something in it.");
         }

         ClassicAssert.AreEqual(0, _application.Domains.Count, "The sample was the only domain, and it has gone.");

         using (JsonDocument state = JsonDocument.Parse(Get(Path)))
         {
            ClassicAssert.AreEqual("absent", state.RootElement.GetProperty("state").GetString());
            ClassicAssert.IsFalse(state.RootElement.GetProperty("exists").GetBoolean());
         }

         (int status, string body) again = Http("DELETE", Path);
         ClassicAssert.AreEqual(409, again.status, again.body);
         StringAssert.Contains("nothing was removed", again.body);
      }

      [Test]
      [Description("The first-hour checklist does not count the sample as the administrator's first domain or its mailboxes as their first accounts, and says why.")]
      public void TheChecklistDoesNotCountTheSample()
      {
         MakeRoom_();
         ClassicAssert.AreEqual(201, Http("POST", Path).status);

         using JsonDocument checklist = JsonDocument.Parse(Get("/api/v1/setup/checklist?dns=0"));
         JsonElement root = checklist.RootElement;

         JsonElement domainItem = Item_(root, "domain");
         ClassicAssert.AreEqual("action_needed", domainItem.GetProperty("state").GetString(),
            "Three sample mailboxes are not a server that has had a domain added to it.");

         bool sameDomainNone = false;
         bool saidWhy = false;
         foreach (JsonElement finding in domainItem.GetProperty("findings").EnumerateArray())
         {
            string code = finding.GetProperty("code").GetString();
            if (code == "domain_none")
               sameDomainNone = true;
            if (code == "sample_domain")
            {
               saidWhy = true;
               ClassicAssert.AreEqual("not_needed", finding.GetProperty("state").GetString(),
                  "The sample finding says why the count is what it is; it cannot change the item's state.");
               ClassicAssert.AreEqual(Domain, finding.GetProperty("args")[0].GetString());
            }
         }

         ClassicAssert.IsTrue(sameDomainNone, domainItem.ToString());
         ClassicAssert.IsTrue(saidWhy, domainItem.ToString());

         ClassicAssert.AreEqual("account_none", FirstCode_(root, "account"),
            "The sample's three mailboxes are not the administrator's first account either.");

         Drain_();
      }

      [Test]
      [Description("The same three documents over COM as over REST, and all three refused to an ordinary account's credentials.")]
      public void TheSameOverComAndRefusedToAnAccount()
      {
         MakeRoom_();

         using (JsonDocument fromCom = JsonDocument.Parse(ComState_()))
         {
            ClassicAssert.AreEqual("absent", fromCom.RootElement.GetProperty("state").GetString());
            ClassicAssert.IsFalse(fromCom.RootElement.TryGetProperty("password", out _),
               "GetSampleDomain answers the same document the route does, and it carries no password.");
         }

         string password;
         using (JsonDocument made = JsonDocument.Parse(ComCreate_()))
         {
            ClassicAssert.IsTrue(made.RootElement.GetProperty("created").GetBoolean());
            ClassicAssert.AreEqual(3, made.RootElement.GetProperty("accounts_created").GetInt32());
            password = made.RootElement.GetProperty("password").GetString();
            StringAssert.IsMatch("^[2-9A-HJ-NP-TV-Z]{5}(-[2-9A-HJ-NP-TV-Z]{5}){3}$", password);
         }

         // The password the COM call answered with really is the mailboxes': a
         // report that carried a plausible-looking string nothing was hashed from
         // would pass the shape assertion above and fail here.
         Pop3ClientSimulator.AssertMessageCount(Bob, password, BobInbox);

         using (JsonDocument fromRest = JsonDocument.Parse(Get(Path)))
         using (JsonDocument fromCom = JsonDocument.Parse(ComState_()))
         {
            ClassicAssert.AreEqual(fromRest.RootElement.GetProperty("state").GetString(),
               fromCom.RootElement.GetProperty("state").GetString());
            ClassicAssert.AreEqual(fromRest.RootElement.GetProperty("domain_id").GetInt64(),
               fromCom.RootElement.GetProperty("domain_id").GetInt64());
         }

         // A second creation is refused, and over COM a refusal is an error carrying
         // the server's own sentence.
         Exception refused = Assert.Catch(() => ComCreate_());
         StringAssert.Contains("already", refused.Message);

         // One of the sample's own mailboxes is an ordinary account, and the sample's
         // routes administer the server.
         string mailbox = Convert.ToBase64String(Encoding.ASCII.GetBytes(Alice + ":" + password));
         ClassicAssert.AreEqual(403, RestRequest.Send("127.0.0.1", RestPort, "GET", Path, "Basic " + mailbox, null).status);
         ClassicAssert.AreEqual(403, RestRequest.Send("127.0.0.1", RestPort, "POST", Path, "Basic " + mailbox, null).status);
         ClassicAssert.AreEqual(403, RestRequest.Send("127.0.0.1", RestPort, "DELETE", Path, "Basic " + mailbox, null).status);

         Drain_();

         using (JsonDocument gone = JsonDocument.Parse(ComRemove_()))
         {
            ClassicAssert.IsTrue(gone.RootElement.GetProperty("removed").GetBoolean());
            ClassicAssert.AreEqual(3, gone.RootElement.GetProperty("accounts").GetInt64());
         }
      }

      // ----------------------------------------------------------------- helpers

      // The three sample-domain members of Utilities are newer than the type
      // library build-tests.ps1 compiles against until an elevated
      // build/register-com.ps1 has run, so they bind late, as the reputation
      // watch's members and the settings defaults do; a refusal still arrives as
      // the COMException the test expects. The Linux shims answer the same three
      // names, so the file compiles and runs there through the REST API.
      private string ComState_() => (string) ((dynamic) _application.Utilities).GetSampleDomain();

      private string ComCreate_() => (string) ((dynamic) _application.Utilities).CreateSampleDomain();

      private string ComRemove_() => (string) ((dynamic) _application.Utilities).RemoveSampleDomain();

      /// <summary>
      ///    Takes the suite's own example.test away, because the sample's is the same
      ///    name and the server refuses to create over a domain it did not make. The
      ///    next test's PerformBasicSetup puts the suite's domain back.
      /// </summary>
      private void MakeRoom_()
      {
         _domain.Delete();
         _domain = null;
         ClassicAssert.AreEqual(0, _application.Domains.Count);
      }

      /// <summary>
      ///    Waits for the delivery queue to empty, and says nothing when it does not:
      ///    this is cleanup rather than an assertion, and a queue that will not drain
      ///    is a failure the tests above make in their own words.
      /// </summary>
      private static void Drain_()
      {
         try
         {
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
         }
      }

      private static string Get(string path)
      {
         (int status, string body) answer = Http("GET", path);
         ClassicAssert.AreEqual(200, answer.status, "GET " + path + " answered " + answer.status + ": " + answer.body);
         return answer.body;
      }

      private static JsonElement Item_(JsonElement root, string key)
      {
         foreach (JsonElement item in root.GetProperty("items").EnumerateArray())
         {
            if (item.GetProperty("key").GetString() == key)
               return item;
         }

         Assert.Fail("No item " + key + " in " + root);
         return default;
      }

      private static string FirstCode_(JsonElement root, string key)
      {
         foreach (JsonElement finding in Item_(root, key).GetProperty("findings").EnumerateArray())
            return finding.GetProperty("code").GetString();

         Assert.Fail("No finding on " + key);
         return null;
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
