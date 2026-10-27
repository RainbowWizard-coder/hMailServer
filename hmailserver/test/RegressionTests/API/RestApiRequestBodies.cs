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

namespace RegressionTests.API
{
   /// <summary>
   ///    A request body is read once, as one JSON document. A body meant to be JSON
   ///    that is not one JSON object is answered 400 before any route reads a member
   ///    of it, and so is one that names a member twice in an object or carries an
   ///    escaped U+0000. Every member reader reads the one parse. Until 6.3.7 a body
   ///    the parse refused was searched for its keys, and so read anyway. Several
   ///    readers searched every body, one that parsed included: GetJsonBoolValue_,
   ///    the digit readers, the object and attachment readers and the "does the body
   ///    name this key" tests. So a key spelt inside a value, or a member of a nested
   ///    object or of an array, was read as the member it is not.
   ///
   ///    The refused shapes go to a route of every reader and to every route that
   ///    needs no credential: text after the value, a trailing comma, an escaped NUL
   ///    and a name given twice. Each must be answered 400 with the dispatcher's
   ///    sentence and leave nothing made. The bodies that parse must be read as the
   ///    parse reads them. Each fails against a server that searched the text, for
   ///    the reason its message names.
   /// </summary>
   [TestFixture]
   public class RestApiRequestBodies : TestFixtureBase
   {
      private static int RestPort = 9661;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Bodies-Passw0rd!";
      private const string Refusal = "the body must be a JSON object";

      // An escaped U+0000, as JSON writes it, put together so that no tool on the
      // way reads it as the character.
      private static readonly string EscapedNul = "\\" + "u0000";

      private string Address
      {
         get { return "bodies@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            RemoveWhatTheControlsMayHaveMade();
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------
      // The refused shapes.

      private sealed class Target
      {
         public string Name;
         public string Method;
         public string Path;
         public string Credential;     // admin, user or none
         public string ExtraHeader;
         public string FirstKey;       // the first member, a string, where the NUL goes
         public string FirstValue;
         public string Rest;           // the other members, written out
         public Func<RestApiRequestBodies, string, string> Made;   // what the request made, or null
      }

      private List<Target> Targets(string tag)
      {
         string token = new string('0', 64);
         string domain = _domain.Name;

         return new List<Target>
         {
            // Administrator routes, one per reader.
            new Target { Name = "POST /api/v1/domains (JsonUtf8Value_, GetJsonBoolValue_)", Method = "POST", Path = "/api/v1/domains", Credential = "admin",
                         FirstKey = "name", FirstValue = "rbr-" + tag + ".test", Rest = "\"active\":true",
                         Made = (t, v) => t.Admin("GET", "/api/v1/domains/rbr-" + tag + ".test", null).status == 200 ? "the domain rbr-" + tag + ".test" : null },
            new Target { Name = "POST /api/v1/apikeys (GetJsonStringValue_, JsonWholeNumber_)", Method = "POST", Path = "/api/v1/apikeys", Credential = "admin",
                         FirstKey = "label", FirstValue = "rbr-" + tag, Rest = "\"scope\":\"readonly\",\"send_per_day\":0",
                         Made = (t, v) => t.Admin("GET", "/api/v1/apikeys", null).body.Contains("\"label\":\"rbr-" + tag + "\"") ? "an API key labelled rbr-" + tag : null },
            new Target { Name = "POST /api/v1/ipranges (GetJsonBoolValue_, the priority)", Method = "POST", Path = "/api/v1/ipranges", Credential = "admin",
                         FirstKey = "name", FirstValue = "rbr-" + tag, Rest = "\"lower\":\"192.0.2.81\",\"upper\":\"192.0.2.81\",\"priority\":5,\"allow_pop3\":false",
                         Made = (t, v) => t.Admin("GET", "/api/v1/ipranges", null).body.Contains("\"name\":\"rbr-" + tag + "\"") ? "an IP range named rbr-" + tag : null },
            new Target { Name = "POST /api/v1/ports (JsonInteger_)", Method = "POST", Path = "/api/v1/ports", Credential = "admin",
                         FirstKey = "protocol", FirstValue = "pop3", Rest = "\"port\":70000" },
            new Target { Name = "POST /api/v1/roles (GetJsonStringValue_)", Method = "POST", Path = "/api/v1/roles", Credential = "admin",
                         FirstKey = "name", FirstValue = "rbr-" + tag, Rest = "\"description\":\"x\",\"permissions\":\"view_configuration\"",
                         Made = (t, v) => t.Admin("GET", "/api/v1/roles/rbr-" + tag, null).status == 200 ? "a role named rbr-" + tag : null },

            // The account's routes.
            new Target { Name = "POST /api/v1/me/address-books (JsonUtf8Value_)", Method = "POST", Path = "/api/v1/me/address-books", Credential = "user",
                         FirstKey = "name", FirstValue = "rbr-" + tag, Rest = "",
                         Made = (t, v) => t.User("GET", "/api/v1/me/address-books", null).body.Contains("\"name\":\"rbr-" + tag + "\"") ? "an address book named rbr-" + tag : null },
            new Target { Name = "PUT /api/v1/me/vacation (a presence test, GetJsonBoolValue_)", Method = "PUT", Path = "/api/v1/me/vacation", Credential = "user",
                         FirstKey = "subject", FirstValue = "rbr-" + tag, Rest = "\"enabled\":false,\"message\":\"m\"",
                         Made = (t, v) => t.User("GET", "/api/v1/me", null).body.Contains("rbr-" + tag) ? "an automatic reply with the subject rbr-" + tag : null },
            new Target { Name = "PUT /api/v1/me/settings (the object readers)", Method = "PUT", Path = "/api/v1/me/settings", Credential = "user",
                         FirstKey = "note", FirstValue = "rbr", Rest = "\"signature\":{\"enabled\":false,\"text\":\"rbr-" + tag + "\"}",
                         Made = (t, v) => t.User("GET", "/api/v1/me/settings", null).body.Contains("rbr-" + tag) ? "a signature of rbr-" + tag : null },
            new Target { Name = "POST /api/v1/me/contacts (JsonWholeNumber_, presence tests)", Method = "POST", Path = "/api/v1/me/contacts", Credential = "user",
                         FirstKey = "address", FirstValue = "rbr-" + tag + "@example.org", Rest = "\"book\":0",
                         Made = (t, v) => t.User("GET", "/api/v1/me/contacts", null).body.Contains("rbr-" + tag + "@example.org") ? "a contact rbr-" + tag + "@example.org" : null },
            new Target { Name = "POST /api/v1/me/drafts (the digit and attachment readers)", Method = "POST", Path = "/api/v1/me/drafts", Credential = "user",
                         FirstKey = "subject", FirstValue = "rbr-" + tag, Rest = "\"text\":\"t\",\"replace_id\":0,\"attachments\":[]" },

            // The routes that need no credential.
            new Target { Name = "POST /api/v1/password-reset", Method = "POST", Path = "/api/v1/password-reset", Credential = "none",
                         FirstKey = "address", FirstValue = Address, Rest = "" },
            new Target { Name = "POST /api/v1/password-reset/complete", Method = "POST", Path = "/api/v1/password-reset/complete", Credential = "none",
                         FirstKey = "token", FirstValue = token, Rest = "\"password\":\"Rbr-Not-Used-1\"" },
            new Target { Name = "POST /api/v1/passkeys/challenge", Method = "POST", Path = "/api/v1/passkeys/challenge", Credential = "none",
                         ExtraHeader = "X-Requested-With: hMailServer", FirstKey = "purpose", FirstValue = "sign-in", Rest = "" },
            new Target { Name = "POST /api/v1/session (the passkey and single sign-on readers in Authenticate_)", Method = "POST", Path = "/api/v1/session", Credential = "none",
                         ExtraHeader = "X-Requested-With: hMailServer", FirstKey = "sso_ticket", FirstValue = "rbr-" + tag, Rest = "" },
            new Target { Name = "POST /api/v1/protected/{token}/reply", Method = "POST", Path = "/api/v1/protected/" + token + "/reply", Credential = "none",
                         FirstKey = "message", FirstValue = "x", Rest = "" },
         };
      }

      private static string Members(Target target, string firstValue)
      {
         string first = "\"" + target.FirstKey + "\":\"" + firstValue + "\"";
         return string.IsNullOrEmpty(target.Rest) ? first : first + "," + target.Rest;
      }

      // Each body sent to each route, every answer judged before the assertion,
      // so that a failure names every route that read the body.
      private void EachIsRefused(string tag, Func<Target, string> body, string because)
      {
         var wrong = new StringBuilder();

         foreach (Target target in Targets(tag))
         {
            string sent = body(target);
            (int status, string answer) = Send(target.Credential, target.Method, target.Path, sent, target.ExtraHeader);

            if (status != 400 || !answer.Contains(Refusal) || !answer.Contains(because))
               wrong.Append("\r\n  " + target.Name + ": " + status + " " + Shorten(answer));

            string made = target.Made == null ? null : target.Made(this, sent);
            if (made != null)
               wrong.Append("\r\n  " + target.Name + " made " + made);
         }

         ClassicAssert.IsTrue(wrong.Length == 0,
            "A body that is not one JSON object (" + tag + ") was read, where it must be answered 400 with '" + Refusal + "' and '" + because + "' before anything reads it:" + wrong);
      }

      [Test]
      [Description("A body with text after its value is answered 400 before any route reads it, on a route of every reader and every route that needs no credential, and nothing is made.")]
      public void TextAfterTheValueIsRefused()
      {
         EachIsRefused("after", t => "{" + Members(t, t.FirstValue) + "} and more", "not JSON");
      }

      [Test]
      [Description("A body with a trailing comma is answered 400 before any route reads it, on a route of every reader and every route that needs no credential, and nothing is made.")]
      public void ATrailingCommaIsRefused()
      {
         EachIsRefused("comma", t => "{" + Members(t, t.FirstValue) + ",}", "not JSON");
      }

      [Test]
      [Description("A body whose string carries an escaped U+0000 is answered 400 before any route reads it, on a route of every reader and every route that needs no credential, and nothing is made - the readers cut the value at it where a document kept it whole.")]
      public void AnEscapedNulIsRefused()
      {
         EachIsRefused("nul", t => "{" + Members(t, t.FirstValue + EscapedNul + "z") + "}", "U+0000");
      }

      [Test]
      [Description("A body that names one member twice is answered 400 before any route reads it, on a route of every reader and every route that needs no credential, and nothing is made - readers of JSON keep the first or the last.")]
      public void ANameGivenTwiceIsRefused()
      {
         EachIsRefused("twice", t => "{" + Members(t, t.FirstValue) + ",\"" + t.FirstKey + "\":\"" + t.FirstValue + "-again\"}", "names one member twice");
      }

      [Test]
      [Description("The same refusal on a SCIM path is RFC 7644's error document, scimType invalidSyntax.")]
      public void AScimBodyThatIsNotJsonIsRefusedInScimsShape()
      {
         (int status, string body) answer = Send("admin", "POST", "/scim/v2/Users", "{\"userName\":\"rbr\"} and more", null);
         ClassicAssert.AreEqual(400, answer.status, answer.body);
         StringAssert.Contains("\"scimType\":\"invalidSyntax\"", answer.body);
         StringAssert.Contains(Refusal, answer.body);
      }

      // ------------------------------------------------------------------
      // Bodies that parse, read as the parse reads them.

      [Test]
      [Description("A port whose address is spelt \"port\" has no port: the number after the next colon is not read as its port number.")]
      public void APortsAddressSpeltPortIsNotItsPort()
      {
         (int status, string body) answer = Admin("POST", "/api/v1/ports", "{\"protocol\":\"smtp\",\"address\":\"port\",\"connection_security\":\"70000\"}");
         ClassicAssert.AreEqual(400, answer.status, answer.body);
         StringAssert.Contains("port is required", answer.body, "The digits after another member's colon were read as the port number.");
      }

      [Test]
      [Description("An IP range named deliver_remote_to_remote is not an open relay: the next boolean in the body is not read as that flag.")]
      public void AnIpRangeNamedAfterAFlagDoesNotSetIt()
      {
         (int status, string body) made = Admin("POST", "/api/v1/ipranges",
            "{\"name\":\"deliver_remote_to_remote\",\"allow_pop3\":true,\"lower\":\"192.0.2.82\",\"upper\":\"192.0.2.82\"}");
         ClassicAssert.AreEqual(201, made.status, made.body);

         string range = RangeEntry("deliver_remote_to_remote");
         ClassicAssert.IsNotNull(range, "The range was not listed.");
         StringAssert.Contains("\"deliver_remote_to_remote\":false", range,
            "The range's name was read as its deliver_remote_to_remote flag, and the next boolean in the body as its value: " + range);
      }

      [Test]
      [Description("A contact renamed \"address\" is renamed, not refused for an address the body never gave; renamed \"book\" it stays in its book.")]
      public void AContactNamedAfterAMemberIsRenamed()
      {
         (int status, string body) made = User("POST", "/api/v1/me/contacts", "{\"address\":\"rbr-friend@example.org\",\"name\":\"Friend\"}");
         ClassicAssert.AreEqual(201, made.status, made.body);
         string id = FirstNumber(made.body, "id");

         (int status, string body) renamed = User("PUT", "/api/v1/me/contacts/" + id, "{\"name\":\"address\"}");
         ClassicAssert.AreEqual(200, renamed.status, "A name of 'address' was read as the address member: " + renamed.body);
         StringAssert.Contains("\"name\":\"address\"", renamed.body);
         StringAssert.Contains("rbr-friend@example.org", renamed.body, "The contact's address changed.");

         (int status, string body) source = User("PUT", "/api/v1/me/contacts/" + id, "{\"name\":\"source\"}");
         ClassicAssert.AreEqual(200, source.status, "A name of 'source' was read as the source member: " + source.body);
      }

      [Test]
      [Description("The account's settings read each object the body names, not the next object in the text after a value that spells its name.")]
      public void ASettingsValueSpeltLikeAnObjectIsAValue()
      {
         (int status, string body) answer = User("PUT", "/api/v1/me/settings",
            "{\"name\":{\"first\":\"forwarding\",\"last\":\"Reader\"},\"signature\":{\"enabled\":true,\"text\":\"rbr-sig\"}}");
         ClassicAssert.AreEqual(200, answer.status,
            "A first name of 'forwarding' made the signature's object the forwarding: " + answer.body);
         StringAssert.Contains("forwarding", answer.body);

         (int status, string body) settings = User("GET", "/api/v1/me/settings", null);
         StringAssert.Contains("rbr-sig", settings.body);
      }

      [Test]
      [Description("An automatic reply whose subject is \"enabled\" does not name enabled.")]
      public void AVacationSubjectSpeltEnabledDoesNotNameEnabled()
      {
         (int status, string body) answer = User("PUT", "/api/v1/me/vacation", "{\"subject\":\"enabled\",\"message\":\"m\"}");
         ClassicAssert.AreEqual(400, answer.status, "A subject of 'enabled' was taken for the enabled member: " + answer.body);
         StringAssert.Contains("enabled is required", answer.body);
      }

      [Test]
      [Description("A draft whose subject is \"replace_id\" replaces no draft: the digits of the next member are not read as the id of one to delete.")]
      public void ADraftSubjectSpeltReplaceIdDeletesNoDraft()
      {
         (int status, string body) first = User("POST", "/api/v1/me/drafts", "{\"subject\":\"rbr first\",\"text\":\"a\"}");
         ClassicAssert.AreEqual(201, first.status, first.body);
         string firstId = FirstNumber(first.body, "id");

         (int status, string body) second = User("POST", "/api/v1/me/drafts",
            "{\"subject\":\"replace_id\",\"to\":\"" + firstId + "@example.org\",\"text\":\"b\"}");
         ClassicAssert.AreEqual(201, second.status, second.body);

         (int status, string body) kept = User("GET", "/api/v1/me/messages/" + firstId, null);
         ClassicAssert.AreEqual(200, kept.status, "The first draft was deleted as the one the second replaced: " + kept.body);
      }

      [Test]
      [Description("A draft whose text is \"attachments\" carries no attachment from another array in the body.")]
      public void ADraftTextSpeltAttachmentsCarriesNoAttachment()
      {
         (int status, string body) made = User("POST", "/api/v1/me/drafts",
            "{\"subject\":\"rbr attach\",\"text\":\"attachments\",\"files\":[{\"name\":\"rbr.txt\",\"type\":\"text/plain\",\"data\":\"cmJy\"}]}");
         ClassicAssert.AreEqual(201, made.status, made.body);
         string id = FirstNumber(made.body, "id");

         (int status, string body) read = User("GET", "/api/v1/me/messages/" + id, null);
         ClassicAssert.AreEqual(200, read.status, read.body);
         StringAssert.DoesNotContain("rbr.txt", read.body, "An object of another array was attached: " + Shorten(read.body));
      }

      [Test]
      [Description("A keyword named deleted sets no \\Deleted: a flag is named by its own member, not by a string in the keyword array.")]
      public void AKeywordSpeltLikeAFlagSetsNoFlag()
      {
         (int status, string body) made = User("POST", "/api/v1/me/drafts", "{\"subject\":\"rbr flags\",\"text\":\"f\"}");
         ClassicAssert.AreEqual(201, made.status, made.body);
         string id = FirstNumber(made.body, "id");

         (int status, string body) flagged = User("PUT", "/api/v1/me/messages/" + id + "/flags", "{\"keywords_add\":[\"deleted\"],\"flagged\":true}");
         ClassicAssert.AreEqual(200, flagged.status, flagged.body);
         StringAssert.Contains("\"flagged\":true", flagged.body);
         StringAssert.Contains("\"deleted\":false", flagged.body, "The keyword 'deleted' was read as the deleted flag, from the next boolean: " + flagged.body);
      }

      [Test]
      [Description("An API key's domains or allowed_from written as an array is refused, not read as no restriction.")]
      public void AnApiKeyRestrictionAsAnArrayIsRefused()
      {
         (int status, string body) domains = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rbr-array\",\"scope\":\"readonly\",\"domains\":[\"" + _domain.Name + "\"]}");
         ClassicAssert.AreEqual(400, domains.status, "A key asked for with domains as an array was made, with no domain restriction: " + domains.body);
         StringAssert.Contains("domains must be", domains.body);

         (int status, string body) from = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rbr-array\",\"scope\":\"readonly\",\"allowed_from\":[\"127.0.0.1\"]}");
         ClassicAssert.AreEqual(400, from.status, "A key asked for with allowed_from as an array was made, with no source restriction: " + from.body);
         StringAssert.Contains("allowed_from must be", from.body);

         StringAssert.DoesNotContain("\"label\":\"rbr-array\"", Admin("GET", "/api/v1/apikeys", null).body);
      }

      [Test]
      [Description("A role assignment's domains written as an array is refused, not read as every domain.")]
      public void ARoleAssignmentsDomainsAsAnArrayIsRefused()
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/role-assignments/" + Address, "{\"role\":\"auditor\",\"domains\":[\"" + _domain.Name + "\"]}");
         ClassicAssert.AreEqual(400, answer.status, "A role assigned with domains as an array was assigned in every domain: " + answer.body);
         StringAssert.Contains("domains must be", answer.body);
      }

      // ------------------------------------------------------------------
      // What is not JSON, and what parses, keeps working.

      [Test]
      [Description("A route whose body is a message keeps taking one, and a body that parses is read as before.")]
      public void WhatIsNotJsonAndWhatParsesAreTakenAsBefore()
      {
         string inbox = FolderId("INBOX");
         ClassicAssert.IsNotNull(inbox, "The account has no INBOX listed.");

         string message = "From: rbr@example.org\r\nTo: " + Address + "\r\nSubject: rbr import\r\n\r\nA message, not JSON.\r\n";
         (int status, string body) imported = User("POST", "/api/v1/me/folders/" + inbox + "/messages", message);
         ClassicAssert.AreEqual(201, imported.status, "The message route refused a message: " + imported.body);

         (int status, string body) book = User("POST", "/api/v1/me/address-books", "{\"name\":\"rbr parses\"}");
         ClassicAssert.AreEqual(201, book.status, book.body);

         (int status, string body) blank = User("PUT", "/api/v1/me/vacation", "");
         ClassicAssert.AreEqual(400, blank.status, blank.body);
         StringAssert.Contains("enabled is required", blank.body, "An empty body is no body, not a body that is not JSON.");
      }

      // ------------------------------------------------------------------

      private void RemoveWhatTheControlsMayHaveMade()
      {
         foreach (string tag in new[] { "after", "comma", "nul", "twice" })
         {
            Admin("DELETE", "/api/v1/domains/rbr-" + tag + ".test", null);
            Admin("DELETE", "/api/v1/roles/rbr-" + tag, null);
         }

         string keys = Admin("GET", "/api/v1/apikeys", null).body;
         foreach (Match key in Regex.Matches(keys, "\"id\":\"([^\"]+)\",\"label\":\"rbr-[a-z]+\""))
            Admin("DELETE", "/api/v1/apikeys/" + key.Groups[1].Value, null);

         Admin("DELETE", "/api/v1/role-assignments/" + Address, null);
      }

      private string RangeEntry(string name)
      {
         string list = Admin("GET", "/api/v1/ipranges", null).body;
         Match entry = Regex.Match(list, "\\{\"id\":\\d+,\"name\":\"" + Regex.Escape(name) + "\"[^}]*\\}");
         return entry.Success ? entry.Value : null;
      }

      private string FolderId(string name)
      {
         string folders = User("GET", "/api/v1/me/folders", null).body;
         Match folder = Regex.Match(folders, "\"id\":(\\d+),\"account_id\":\\d+,\"name\":\"" + Regex.Escape(name) + "\"");
         return folder.Success ? folder.Groups[1].Value : null;
      }

      private static string FirstNumber(string json, string key)
      {
         Match found = Regex.Match(json, "\"" + key + "\":(\\d+)");
         ClassicAssert.IsTrue(found.Success, "No " + key + " in " + json);
         return found.Groups[1].Value;
      }

      private static string Shorten(string text)
      {
         return text.Length > 220 ? text.Substring(0, 220) + "..." : text;
      }

      private (int status, string body) Send(string credential, string method, string path, string body, string extraHeader)
      {
         if (credential == "admin")
            return Admin(method, path, body, extraHeader);
         if (credential == "user")
            return User(method, path, body, extraHeader);
         return RestRequest.Send("127.0.0.1", RestPort, method, path, null, body, extraHeader);
      }

      private (int status, string body) Admin(string method, string path, string body, string extraHeader = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), body, extraHeader);
      }

      private (int status, string body) User(string method, string path, string body, string extraHeader = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic(Address, UserPassword), body, extraHeader);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }
   }
}
