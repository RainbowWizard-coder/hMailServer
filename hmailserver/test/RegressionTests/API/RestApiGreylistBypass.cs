// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The greylisting bypass list over REST (/api/v1/greylisting-bypass): the
   ///    four verbs, the entry's shape, the refusals the persistence layer makes -
   ///    an empty pattern, one that is not a domain, an entry requiring nothing, a
   ///    match word outside the three - and the three hardened-greylisting rows of
   ///    the anti-spam settings group with their bounds. What the entries DO to a
   ///    message is AntiSpam/GreyListingHardened's; this is the resource.
   ///
   ///    Everything here is made over the API and torn down over it, so nothing
   ///    depends on COM and the Linux suite links this fixture.
   /// </summary>
   [TestFixture]
   public class RestApiGreylistBypass : TestFixtureBase
   {
      private static int RestPort = 9677;
      private const string AdminPassword = "testar";

      private readonly List<long> made_ = new List<long>();
      private string settingsBefore_;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         Dictionary<string, object> settings = AntiSpamSettings();
         settingsBefore_ = "{\"greylisting_slide_on_early_retry\":" + (MiniJson.Flag(settings, "greylisting_slide_on_early_retry") ? "true" : "false") +
                           ",\"greylisting_abuse_threshold\":" + MiniJson.Number(settings, "greylisting_abuse_threshold") +
                           ",\"greylisting_abuse_lock_minutes\":" + MiniJson.Number(settings, "greylisting_abuse_lock_minutes") + "}";
      }

      [TearDown]
      public void StopRestApi()
      {
         // The status of every delete is read. An entry left behind switches
         // greylisting off for its domain in every fixture that runs after this
         // one, and a DELETE nobody checked is how that happens quietly.
         var notRemoved = new List<string>();

         try
         {
            foreach (long id in made_)
            {
               (int status, string body) removed = Http("DELETE", "/api/v1/greylisting-bypass/" + id);
               if (removed.status != 200)
                  notRemoved.Add(id + " -> " + removed.status + " " + removed.body);
            }

            made_.Clear();

            Http("PUT", "/api/v1/settings/antispam", settingsBefore_);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }

         ClassicAssert.AreEqual(0, notRemoved.Count,
            "A greylisting bypass entry this fixture made was not removed, so greylisting is off for its domain in every " +
            "fixture after this one: " + string.Join("; ", notRemoved));
      }

      [Test]
      [Description("An entry is created with its pattern lower-cased and its flags as given, listed, changed by id with the fields left out kept, and removed.")]
      public void AnEntryIsCreatedListedChangedAndRemoved()
      {
         (int status, string body) created = Http("POST", "/api/v1/greylisting-bypass",
            "{\"pattern\":\"Partner.Example\",\"match\":\"both\",\"require_spf\":true,\"require_dkim\":true,\"note\":\"their gateway retries on a timer\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);

         Dictionary<string, object> entry = MiniJson.Object(created.body);
         long id = MiniJson.Number(entry, "id");
         made_.Add(id);

         ClassicAssert.IsTrue(id > 0, created.body);
         ClassicAssert.AreEqual("partner.example", MiniJson.Text(entry, "pattern"), created.body);
         ClassicAssert.AreEqual("both", MiniJson.Text(entry, "match"), created.body);
         ClassicAssert.IsTrue(MiniJson.Flag(entry, "require_spf"), created.body);
         ClassicAssert.IsTrue(MiniJson.Flag(entry, "require_dkim"), created.body);
         ClassicAssert.IsFalse(MiniJson.Flag(entry, "require_dmarc"), created.body);
         ClassicAssert.AreEqual("their gateway retries on a timer", MiniJson.Text(entry, "note"), created.body);

         (int status, string body) listed = Http("GET", "/api/v1/greylisting-bypass");
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         ClassicAssert.IsTrue(listed.body.Contains("\"id\":" + id + ","), listed.body);
         ClassicAssert.IsTrue(listed.body.Contains("\"pattern\":\"partner.example\""), listed.body);

         // A change of one field keeps the rest.
         (int status, string body) changed = Http("PUT", "/api/v1/greylisting-bypass/" + id, "{\"require_dmarc\":true,\"match\":\"helo\"}");
         ClassicAssert.AreEqual(200, changed.status, changed.body);

         Dictionary<string, object> after = MiniJson.Object(changed.body);
         ClassicAssert.AreEqual("partner.example", MiniJson.Text(after, "pattern"), changed.body);
         ClassicAssert.AreEqual("helo", MiniJson.Text(after, "match"), changed.body);
         ClassicAssert.IsTrue(MiniJson.Flag(after, "require_spf"), changed.body);
         ClassicAssert.IsTrue(MiniJson.Flag(after, "require_dkim"), changed.body);
         ClassicAssert.IsTrue(MiniJson.Flag(after, "require_dmarc"), changed.body);
         ClassicAssert.AreEqual("their gateway retries on a timer", MiniJson.Text(after, "note"), changed.body);

         (int status, string body) removed = Http("DELETE", "/api/v1/greylisting-bypass/" + id);
         ClassicAssert.AreEqual(200, removed.status, removed.body);
         made_.Remove(id);

         (int status, string body) gone = Http("DELETE", "/api/v1/greylisting-bypass/" + id);
         ClassicAssert.AreEqual(404, gone.status, gone.body);

         (int status, string body) unknown = Http("PUT", "/api/v1/greylisting-bypass/" + id, "{\"note\":\"x\"}");
         ClassicAssert.AreEqual(404, unknown.status, unknown.body);
      }

      [Test]
      [Description("A suffix with a leading dot is kept as one; a bare domain is kept as one; both are lower-cased and trimmed.")]
      public void ASuffixKeepsItsLeadingDot()
      {
         (int status, string body) suffix = Http("POST", "/api/v1/greylisting-bypass", "{\"pattern\":\"  .Mail.Partner.Example \",\"require_spf\":true}");
         ClassicAssert.AreEqual(201, suffix.status, suffix.body);
         made_.Add(MiniJson.Number(MiniJson.Object(suffix.body), "id"));
         ClassicAssert.AreEqual(".mail.partner.example", MiniJson.Text(MiniJson.Object(suffix.body), "pattern"), suffix.body);
         ClassicAssert.AreEqual("envelope", MiniJson.Text(MiniJson.Object(suffix.body), "match"), "envelope is the default match: " + suffix.body);
      }

      [Test]
      [Description("The refusals, each a 400 naming the reason and none of them writing a row: an empty pattern, an address and an IP " +
                   "literal as a pattern, an entry requiring nothing, a match word outside the three, a note longer than the column, " +
                   "a flag that is not a boolean, an unknown field and a body that is not an object.")]
      public void TheRefusals()
      {
         int before = Count();

         // The note is refused rather than cut. The reply to a create echoes the
         // entry, so a silent truncation answers with text the row does not hold.
         Refused("{\"pattern\":\"quiet.example\",\"require_spf\":true,\"note\":\"" + new string('n', 256) + "\"}",
            "The note is at most 255 characters");

         Refused("{\"pattern\":\"\",\"require_spf\":true}", "must not be empty");
         Refused("{\"pattern\":\"user@example.com\",\"require_spf\":true}", "domain name");
         Refused("{\"pattern\":\"[192.0.2.1]\",\"require_spf\":true}", "domain name");
         Refused("{\"pattern\":\"-bad.example\",\"require_spf\":true}", "domain name");
         Refused("{\"pattern\":\"quiet.example\"}", "At least one of SPF, DKIM and DMARC");
         Refused("{\"pattern\":\"quiet.example\",\"require_spf\":false,\"require_dkim\":false,\"require_dmarc\":false}", "At least one of SPF, DKIM and DMARC");
         Refused("{\"pattern\":\"quiet.example\",\"match\":\"sender\",\"require_spf\":true}", "match must be envelope, helo or both");
         Refused("{\"pattern\":\"quiet.example\",\"require_spf\":\"yes\"}", "require_spf must be true or false");
         Refused("{\"pattern\":\"quiet.example\",\"require_spf\":true,\"colour\":\"blue\"}", "unknown field: colour");
         Refused("[]", "the body must be a JSON object");

         ClassicAssert.AreEqual(before, Count(), "a refused create wrote nothing");
      }

      [Test]
      [Description("The three settings of the sliding wait and the abuse refusal are in the anti-spam group, off, 0 and 60 by " +
                   "default on this bench, and bounded: a threshold over 100000 and a lock of 0 or over a week are refused with the " +
                   "same sentence the COM properties refuse with.")]
      public void TheThreeSettingsAreInTheAntiSpamGroupAndBounded()
      {
         Dictionary<string, object> settings = AntiSpamSettings();
         ClassicAssert.IsTrue(settings.ContainsKey("greylisting_slide_on_early_retry"), "greylisting_slide_on_early_retry is in the group");
         ClassicAssert.IsTrue(settings.ContainsKey("greylisting_abuse_threshold"), "greylisting_abuse_threshold is in the group");
         ClassicAssert.IsTrue(settings.ContainsKey("greylisting_abuse_lock_minutes"), "greylisting_abuse_lock_minutes is in the group");

         (int status, string body) set = Http("PUT", "/api/v1/settings/antispam",
            "{\"greylisting_slide_on_early_retry\":true,\"greylisting_abuse_threshold\":20,\"greylisting_abuse_lock_minutes\":90}");
         ClassicAssert.AreEqual(200, set.status, set.body);

         settings = AntiSpamSettings();
         ClassicAssert.IsTrue(MiniJson.Flag(settings, "greylisting_slide_on_early_retry"));
         ClassicAssert.AreEqual(20, MiniJson.Number(settings, "greylisting_abuse_threshold"));
         ClassicAssert.AreEqual(90, MiniJson.Number(settings, "greylisting_abuse_lock_minutes"));

         (int status, string body) tooMany = Http("PUT", "/api/v1/settings/antispam", "{\"greylisting_abuse_threshold\":100001}");
         ClassicAssert.AreEqual(400, tooMany.status, tooMany.body);
         ClassicAssert.IsTrue(tooMany.body.Contains("GreyListingAbuseThreshold is between 0 and 100000"), tooMany.body);

         (int status, string body) noLock = Http("PUT", "/api/v1/settings/antispam", "{\"greylisting_abuse_lock_minutes\":0}");
         ClassicAssert.AreEqual(400, noLock.status, noLock.body);
         ClassicAssert.IsTrue(noLock.body.Contains("GreyListingAbuseLockMinutes is between 1 and 10080"), noLock.body);

         (int status, string body) tooLong = Http("PUT", "/api/v1/settings/antispam", "{\"greylisting_abuse_lock_minutes\":10081}");
         ClassicAssert.AreEqual(400, tooLong.status, tooLong.body);

         // A refused write changed nothing.
         settings = AntiSpamSettings();
         ClassicAssert.AreEqual(20, MiniJson.Number(settings, "greylisting_abuse_threshold"));
         ClassicAssert.AreEqual(90, MiniJson.Number(settings, "greylisting_abuse_lock_minutes"));
      }

      [Test]
      [Description("Who may reach the four routes. The list is server-wide - it decides whether mail from a named domain is " +
                   "challenged at all - so a key issued for one domain is refused on every one of them, listing included, and so " +
                   "is an ordinary account's own password; a read-only key may list it and not change it; and none of the refusals " +
                   "writes or removes anything.")]
      public void OnlyAnUnrestrictedAdministratorReachesTheList()
      {
         (int status, string body) created = Http("POST", "/api/v1/greylisting-bypass",
            "{\"pattern\":\"authz.example\",\"require_spf\":true,\"note\":\"the entry the refusals must not touch\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);

         long id = MiniJson.Number(MiniJson.Object(created.body), "id");
         made_.Add(id);

         const string AccountAddress = "greybypass-user@example.test";
         const string AccountPassword = "secret1234";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, AccountAddress, AccountPassword);

         (string id, string token) scoped = CreateKey("regression - greylist bypass, one domain", "full", _domain.Name);
         (string id, string token) readOnly = CreateKey("regression - greylist bypass, read only", "readonly", null);

         string account = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes(AccountAddress + ":" + AccountPassword));

         try
         {
            foreach (string credential in new[] { "Bearer " + scoped.token, account })
            {
               string who = credential.StartsWith("Bearer") ? "a key issued for one domain" : "an ordinary account";

               ClassicAssert.AreEqual(403, Send("GET", "/api/v1/greylisting-bypass", credential).status,
                  who + " listed the bypass list");
               ClassicAssert.AreEqual(403, Send("POST", "/api/v1/greylisting-bypass", credential,
                  "{\"pattern\":\"planted.example\",\"require_spf\":true}").status, who + " added an entry");
               ClassicAssert.AreEqual(403, Send("PUT", "/api/v1/greylisting-bypass/" + id, credential,
                  "{\"note\":\"planted\"}").status, who + " changed an entry");
               ClassicAssert.AreEqual(403, Send("DELETE", "/api/v1/greylisting-bypass/" + id, credential).status,
                  who + " removed an entry");
            }

            ClassicAssert.AreEqual(200, Send("GET", "/api/v1/greylisting-bypass", "Bearer " + readOnly.token).status,
               "a read-only key reads the list");
            ClassicAssert.AreEqual(403, Send("POST", "/api/v1/greylisting-bypass", "Bearer " + readOnly.token,
               "{\"pattern\":\"planted.example\",\"require_spf\":true}").status, "a read-only key added an entry");
            ClassicAssert.AreEqual(403, Send("DELETE", "/api/v1/greylisting-bypass/" + id, "Bearer " + readOnly.token).status,
               "a read-only key removed an entry");

            (int status, string body) listed = Http("GET", "/api/v1/greylisting-bypass");
            ClassicAssert.AreEqual(200, listed.status, listed.body);
            ClassicAssert.IsFalse(listed.body.Contains("planted.example"), "a refused create wrote a row: " + listed.body);
            ClassicAssert.IsTrue(listed.body.Contains("\"id\":" + id + ","), "a refused delete removed the entry: " + listed.body);
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + scoped.id);
            Http("DELETE", "/api/v1/apikeys/" + readOnly.id);
         }
      }

      [Test]
      [Description("The OpenAPI document describes the resource, its enumeration and its flags, which is what the Deck builds its editor from.")]
      public void TheResourceIsInTheOpenApiDocument()
      {
         (int status, string body) spec = Http("GET", "/api/v1/openapi.json");
         ClassicAssert.AreEqual(200, spec.status, spec.body.Length > 400 ? spec.body.Substring(0, 400) : spec.body);

         ClassicAssert.IsTrue(spec.body.Contains("\"/api/v1/greylisting-bypass\":{"), "the collection path");
         ClassicAssert.IsTrue(spec.body.Contains("\"/api/v1/greylisting-bypass/{id}\":{"), "the entry path");
         ClassicAssert.IsTrue(spec.body.Contains("\"match\":{\"type\":\"string\",\"enum\":[\"envelope\",\"helo\",\"both\"]}"), "the match enumeration");
         ClassicAssert.IsTrue(spec.body.Contains("\"require_dmarc\":{\"type\":\"boolean\"}"), "the flags are booleans");
      }

      // ------------------------------------------------------------------ helpers

      private void Refused(string json, string reason)
      {
         (int status, string body) answer = Http("POST", "/api/v1/greylisting-bypass", json);
         ClassicAssert.AreEqual(400, answer.status, json + " -> " + answer.body);
         ClassicAssert.IsTrue(answer.body.IndexOf(reason, StringComparison.Ordinal) >= 0, json + " -> " + answer.body);
      }

      private int Count()
      {
         (int status, string body) listed = Http("GET", "/api/v1/greylisting-bypass");
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         return MiniJson.List(MiniJson.Object("{\"items\":" + listed.body + "}"), "items").Count;
      }

      private static Dictionary<string, object> AntiSpamSettings()
      {
         (int status, string body) = Http("GET", "/api/v1/settings/antispam");
         ClassicAssert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }

      /// <summary>The same request under somebody else's credential.</summary>
      private static (int status, string body) Send(string method, string path, string authorization, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static (string id, string token) CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";

         (int status, string answer) = Http("POST", "/api/v1/apikeys", body);
         ClassicAssert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + answer);

         Dictionary<string, object> key = MiniJson.Object(answer);
         return (MiniJson.Text(key, "id"), MiniJson.Text(key, "key"));
      }
   }
}
