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
   ///
   ///    And what reading a body costs: a route that needs a credential judges the
   ///    body only once the credential is, so a request with none is answered 401
   ///    for a body of the large cap without its being parsed, and a body is parsed
   ///    within bounds of values and depth, refused at the first value past them -
   ///    on the protected-message reply too, which takes no credential and the
   ///    large cap. And a member whose absence is the wider setting, written as
   ///    another type than the route reads, is refused, not read as absent.
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

      // A byte order mark, as a spreadsheet's "CSV UTF-8" starts with one.
      private static readonly string ByteOrderMark = ((char) 0xFEFF).ToString();

      // The port numbers the port controls ask for, removed in the teardown.
      private static readonly int[] ControlPorts = { 2596, 2597, 2598, 2599 };

      private string Address
      {
         get { return "bodies@" + _domain.Name; }
      }

      private string ReaderAddress
      {
         get { return "bodies-reader@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, ReaderAddress, UserPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         // A password-protected message to reply to, as RestApiProtectedMessages sets
         // the server up for one.
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"http://127.0.0.1:" + RestPort + "\"}", null).status);
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}", null).status);
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(Address, UserPassword));
         ClassicAssert.IsTrue(imap.CreateFolder("Sent"));
         imap.Disconnect();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            RemoveWhatTheControlsMayHaveMade();
            Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\"}", null);
            Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}", null);
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

         // The reply route judges its body only once the token names a message
         // that takes a reply, so it is sent to one, with the ciphertext a reply
         // carries as the member the shapes break.
         string protectedToken = MakeProtectedMessage();

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
            new Target { Name = "POST /api/v1/protected/{token}/reply", Method = "POST", Path = "/api/v1/protected/" + protectedToken + "/reply", Credential = "none",
                         FirstKey = "message", FirstValue = JsonText(RestApiProtectedMessages.Vector), Rest = "",
                         Made = (t, v) => t.User("GET", "/api/v1/me/protected", null).body.Contains("\"reply_to\":\"" + protectedToken + "\"") ? "a reply to the protected message" : null },
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
      [Description("A name given twice inside a nested object, and an escaped U+0000 in a nested string, in an array's item and in a member name, are refused as at the top level, and nothing is changed - the readers of a nested object took the first of a name given twice, and cut a value at the NUL.")]
      public void ANameGivenTwiceOrANulBelowTheTopLevelIsRefused()
      {
         (int status, string body) draft = User("POST", "/api/v1/me/drafts", "{\"subject\":\"rbr nested\",\"text\":\"n\"}");
         ClassicAssert.AreEqual(201, draft.status, draft.body);
         string id = FirstNumber(draft.body, "id");

         string[][] cases =
         {
            new[] { "names one member twice", "PUT", "/api/v1/me/settings", "{\"signature\":{\"enabled\":true,\"text\":\"rbr-twice-a\",\"text\":\"rbr-twice-b\"}}" },
            new[] { "U+0000", "PUT", "/api/v1/me/settings", "{\"signature\":{\"enabled\":true,\"text\":\"rbr-nul-string" + EscapedNul + "z\"}}" },
            new[] { "U+0000", "PUT", "/api/v1/me/settings", "{\"signature\":{\"enabled\":true,\"text\":\"rbr-nul-name\",\"no" + EscapedNul + "te\":\"x\"}}" },
            new[] { "U+0000", "PUT", "/api/v1/me/messages/" + id + "/flags", "{\"keywords_add\":[\"rbrnul" + EscapedNul + "z\"]}" },
         };

         var wrong = new StringBuilder();
         foreach (string[] item in cases)
         {
            (int status, string body) answer = User(item[1], item[2], item[3]);
            if (answer.status != 400 || !answer.body.Contains(Refusal) || !answer.body.Contains(item[0]))
               wrong.Append("\r\n  " + item[1] + " " + item[2] + " " + item[3] + ": " + answer.status + " " + Shorten(answer.body));
         }

         string settings = User("GET", "/api/v1/me/settings", null).body;
         foreach (string text in new[] { "rbr-twice", "rbr-nul-string", "rbr-nul-name" })
         {
            if (settings.Contains(text))
               wrong.Append("\r\n  the signature became " + text + ": " + Shorten(settings));
         }
         string message = User("GET", "/api/v1/me/messages/" + id, null).body;
         if (message.Contains("rbrnul"))
            wrong.Append("\r\n  the keyword rbrnul was set: " + Shorten(message));

         ClassicAssert.IsTrue(wrong.Length == 0, "A nested object's name given twice, or a U+0000 below the top level, was read:" + wrong);
      }

      [Test]
      [Description("The same refusal on a SCIM path, to the API key SCIM answers to, is RFC 7644's error document, scimType invalidSyntax; without a credential the same body is SCIM's 401, the body not judged.")]
      public void AScimBodyThatIsNotJsonIsRefusedInScimsShape()
      {
         (int status, string body) key = Admin("POST", "/api/v1/apikeys", "{\"label\":\"rbr-scim\",\"scope\":\"full\"}", null);
         ClassicAssert.AreEqual(201, key.status, key.body);
         string bearer = "Bearer " + Regex.Match(key.body, "\"key\":\"([^\"]+)\"").Groups[1].Value;

         (int status, string body) answer = RestRequest.Send("127.0.0.1", RestPort, "POST", "/scim/v2/Users", bearer, "{\"userName\":\"rbr\"} and more");
         ClassicAssert.AreEqual(400, answer.status, answer.body);
         StringAssert.Contains("\"scimType\":\"invalidSyntax\"", answer.body);
         StringAssert.Contains(Refusal, answer.body);

         (int status, string body) nobody = RestRequest.Send("127.0.0.1", RestPort, "POST", "/scim/v2/Users", null, "{\"userName\":\"rbr\"} and more");
         ClassicAssert.AreEqual(401, nobody.status, "A SCIM body was judged before the credential: " + nobody.body);
         StringAssert.DoesNotContain(Refusal, nobody.body);
      }

      // ------------------------------------------------------------------
      // What a body costs.

      [Test]
      [Description("A body of the large cap sent with no credential to a route that needs one is answered 401 without being parsed: at once, and with the server's peak memory grown by little more than the body's own bytes. Sixteen megabytes of zeros in an array, flat and sixty-two arrays deep, to POST /api/v1/me/messages, which the listener grants the large cap before any credential is read.")]
      public void ABodyIsNotParsedBeforeTheCredentialIsJudged()
      {
         var wrong = new StringBuilder();
         foreach (KeyValuePair<string, string> shape in LargeShapes())
         {
            ServerMemory memory = ServerMemory.Start();
            var watch = System.Diagnostics.Stopwatch.StartNew();
            (int status, string answer) = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/me/messages", null, shape.Value);
            double seconds = watch.Elapsed.TotalSeconds;
            string grew = memory.Grew(out long grownBytes);

            if (status != 401)
               wrong.Append("\r\n  " + shape.Key + ": " + status + " " + Shorten(answer));
            if (seconds > SecondsForALargeBody)
               wrong.Append("\r\n  " + shape.Key + ": answered after " + seconds.ToString("0.00") + " s");
            if (grownBytes > BytesALargeBodyMayCost)
               wrong.Append("\r\n  " + shape.Key + ": the server's peak memory grew " + grew);
            TestContext.Progress.WriteLine("RestApiRequestBodies: no credential, " + shape.Key + ": " + status + " in " + seconds.ToString("0.00") + " s, peak memory grew " + grew);
         }

         ClassicAssert.IsTrue(wrong.Length == 0, "A body was parsed before its request's credential was judged:" + wrong);
      }

      [Test]
      [Description("A body past the parse's bounds - more than 100000 values, or more than 32 levels deep - is answered 400 at the first value past them, to a credential the route takes: sixteen megabytes of zeros in an array, flat and sixty-two arrays deep, to POST /api/v1/me/messages, each refused at once and with the server's peak memory grown by little more than the body's own bytes; and nothing is sent.")]
      public void ABodyPastTheParsesBoundsIsRefusedAtOnce()
      {
         var wrong = new StringBuilder();
         foreach (KeyValuePair<string, string> shape in LargeShapes())
         {
            ServerMemory memory = ServerMemory.Start();
            var watch = System.Diagnostics.Stopwatch.StartNew();
            (int status, string answer) = User("POST", "/api/v1/me/messages", shape.Value);
            double seconds = watch.Elapsed.TotalSeconds;
            string grew = memory.Grew(out long grownBytes);

            if (status != 400 || !answer.Contains(Refusal) || !answer.Contains("more than 100000 values"))
               wrong.Append("\r\n  " + shape.Key + ": " + status + " " + Shorten(answer));
            if (seconds > SecondsForALargeBody)
               wrong.Append("\r\n  " + shape.Key + ": answered after " + seconds.ToString("0.00") + " s");
            if (grownBytes > BytesALargeBodyMayCost)
               wrong.Append("\r\n  " + shape.Key + ": the server's peak memory grew " + grew);
            TestContext.Progress.WriteLine("RestApiRequestBodies: the account, " + shape.Key + ": " + status + " in " + seconds.ToString("0.00") + " s, peak memory grew " + grew);
         }

         ClassicAssert.IsTrue(wrong.Length == 0, "A body past the parse's bounds was read, or read slowly:" + wrong);

         // Within the bounds a body past the ordinary cap is read as before: one
         // string of four megabytes, the shape of a message's attachment, is one
         // value.
         string text = new string('a', 4 * 1024 * 1024);
         (int status, string body) draft = User("POST", "/api/v1/me/drafts", "{\"subject\":\"rbr large\",\"text\":\"" + text + "\"}");
         ClassicAssert.AreEqual(201, draft.status, "A draft of one four-megabyte string was refused: " + Shorten(draft.body));
      }

      [Test]
      [Description("The reply to a password-protected message takes no credential, only the token of its link. It is granted more than the ordinary 64 KB but held to 2 MB, room for its one armoured message and nothing like the 16 MB the attachment routes take: a body larger than that is refused by the listener before anything reads it, so a message's link alone never reaches a large parse (a 16 MB body is covered by ABodyIsNotParsedBeforeTheCredentialIsJudged's counterpart on the credentialed route). A body within the 2 MB ceiling but past the parse's bounds - about 1.9 MB of zeros in an array, flat and sixty-two arrays deep, with no credential - is answered 400 at the first value past the bounds, at once and with the server's peak memory grown by little more than the body's own bytes; and no reply is made.")]
      public void AProtectedMessagesReplyIsParsedWithinTheBounds()
      {
         string token = MakeProtectedMessage();
         var wrong = new StringBuilder();
         foreach (KeyValuePair<string, string> shape in WithinCeilingShapes())
         {
            ServerMemory memory = ServerMemory.Start();
            var watch = System.Diagnostics.Stopwatch.StartNew();
            (int status, string answer) = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/protected/" + token + "/reply", null, shape.Value);
            double seconds = watch.Elapsed.TotalSeconds;
            string grew = memory.Grew(out long grownBytes);

            if (status != 400 || !answer.Contains(Refusal) || !answer.Contains("more than 100000 values"))
               wrong.Append("\r\n  " + shape.Key + ": " + status + " " + Shorten(answer));
            if (seconds > SecondsForALargeBody)
               wrong.Append("\r\n  " + shape.Key + ": answered after " + seconds.ToString("0.00") + " s");
            if (grownBytes > BytesALargeBodyMayCost)
               wrong.Append("\r\n  " + shape.Key + ": the server's peak memory grew " + grew);
            TestContext.Progress.WriteLine("RestApiRequestBodies: the protected-message reply, no credential, " + shape.Key + ": " + status + " in " + seconds.ToString("0.00") + " s, peak memory grew " + grew);
         }

         if (User("GET", "/api/v1/me/protected", null).body.Contains("\"reply_to\":\"" + token + "\""))
            wrong.Append("\r\n  a reply to the protected message was made");

         ClassicAssert.IsTrue(wrong.Length == 0, "A body within the reply's ceiling but past the parse's bounds was read past them, or read slowly:" + wrong);
      }

      // Two shapes just under the protected reply's 2 MB ceiling: an array of zeros,
      // and the same zeros sixty-two arrays deep. Each is a JSON object, each holds
      // far more than 100,000 values, so each is refused at the value bound.
      private static IEnumerable<KeyValuePair<string, string>> WithinCeilingShapes()
      {
         int cap = 2 * 1024 * 1024 - 64 * 1024;
         foreach (int depth in new[] { 1, 62 })
         {
            int zeros = (cap - 16 - 2 * depth) / 2;
            var text = new StringBuilder(cap);
            text.Append("{\"a\":");
            text.Append('[', depth);
            text.Append('0');
            for (int i = 1; i < zeros; i++)
               text.Append(",0");
            text.Append(']', depth);
            text.Append('}');
            yield return new KeyValuePair<string, string>(depth == 1 ? "an array of zeros" : "the zeros 62 arrays deep", text.ToString());
         }
      }

      [Test]
      [Description("An object of more members than a route takes - more than 10000 in one object, well within the value and depth bounds - is answered 400 before the handler reads it, with the account's password to POST /api/v1/me/messages and with no credential to the protected-message reply, each at once and within five seconds; and no reply is made. The bound follows the bytes and not the member names, so it holds whatever a caller chooses to name the members: it is the request body's guard against one huge object, the shape a duplicate-name check that walked its members would have been slowest on.")]
      public void AnObjectOfTooManyMembersIsRefused()
      {
         // Twenty thousand distinct members, each a short name and a zero: about
         // 230 KB, far within the value bound (100000) and one level deep, and
         // twice the 10000-member bound.
         var text = new StringBuilder(256 * 1024);
         text.Append('{');
         for (int i = 0; i < 20000; i++)
         {
            if (i != 0)
               text.Append(',');
            text.Append("\"m").Append(i).Append("\":0");
         }
         text.Append('}');
         string body = text.ToString();

         string token = MakeProtectedMessage();
         var cases = new[]
         {
            new KeyValuePair<string, (int, string)>("the account's password to POST /api/v1/me/messages", (0, "/api/v1/me/messages")),
            new KeyValuePair<string, (int, string)>("no credential to the protected-message reply", (1, "/api/v1/protected/" + token + "/reply")),
         };

         var wrong = new StringBuilder();
         foreach (KeyValuePair<string, (int who, string path)> one in cases)
         {
            var watch = System.Diagnostics.Stopwatch.StartNew();
            (int status, string answer) = one.Value.who == 0
               ? User("POST", one.Value.path, body)
               : RestRequest.Send("127.0.0.1", RestPort, "POST", one.Value.path, null, body);
            double seconds = watch.Elapsed.TotalSeconds;

            if (status != 400 || !answer.Contains(Refusal) || !answer.Contains("more than 10000 members"))
               wrong.Append("\r\n  " + one.Key + ": " + status + " " + Shorten(answer));
            if (seconds > SecondsForALargeBody)
               wrong.Append("\r\n  " + one.Key + ": answered after " + seconds.ToString("0.00") + " s");
            TestContext.Progress.WriteLine("RestApiRequestBodies: too many members, " + one.Key + ": " + status + " in " + seconds.ToString("0.00") + " s");
         }

         if (User("GET", "/api/v1/me/protected", null).body.Contains("\"reply_to\":\"" + token + "\""))
            wrong.Append("\r\n  a reply to the protected message was made");

         ClassicAssert.IsTrue(wrong.Length == 0, "An object of more than ten thousand members was read, or read slowly:" + wrong);

         // Ten thousand members exactly - the bound itself - is read: the member
         // of that name is found, so the bound refuses only past a count no route
         // takes, not at it.
         var atBound = new StringBuilder(160 * 1024);
         atBound.Append("{\"subject\":\"rbr members\",\"text\":\"m\"");
         for (int i = 0; i < 9998; i++)
            atBound.Append(",\"m").Append(i).Append("\":0");
         atBound.Append('}');
         (int status, string body) draft = User("POST", "/api/v1/me/drafts", atBound.ToString());
         ClassicAssert.AreEqual(201, draft.status, "A draft of ten thousand members (the bound) was refused: " + Shorten(draft.body));
      }

      // The two shapes, each just under the listener's 16 MB: an array of zeros, and
      // the same zeros sixty-two arrays deep. Each is a JSON object.
      private static IEnumerable<KeyValuePair<string, string>> LargeShapes()
      {
         int cap = 16 * 1024 * 1024 - 4096;
         foreach (int depth in new[] { 1, 62 })
         {
            int zeros = (cap - 16 - 2 * depth) / 2;
            var text = new StringBuilder(cap);
            text.Append("{\"a\":");
            text.Append('[', depth);
            text.Append('0');
            for (int i = 1; i < zeros; i++)
               text.Append(",0");
            text.Append(']', depth);
            text.Append('}');
            yield return new KeyValuePair<string, string>(depth == 1 ? "an array of zeros" : "the zeros 62 arrays deep", text.ToString());
         }
      }

      // What one such request may take. The body's own bytes are held a few times
      // over while it arrives and is answered - the listener's buffer, the request,
      // the body - which is how a request with no credential grows the peak by
      // about 60 MB on Linux; parsing either shape took more than 2 GB.
      private const double SecondsForALargeBody = 5.0;
      private const long BytesALargeBodyMayCost = 256L * 1024 * 1024;

      // The server's peak memory over one request: on Linux the process's VmHWM,
      // reset first where the kernel lets it be (clear_refs), and elsewhere its
      // peak working set, of which only growth past the earlier peak shows.
      private sealed class ServerMemory
      {
         private long _before;

         public static ServerMemory Start()
         {
            var memory = new ServerMemory();
            using (System.Diagnostics.Process server = Server())
            {
               if (server != null && System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(System.Runtime.InteropServices.OSPlatform.Linux))
               {
                  try
                  {
                     System.IO.File.WriteAllText("/proc/" + server.Id + "/clear_refs", "5");
                  }
                  catch (Exception)
                  {
                     // The peak since the process began, then.
                  }
               }
               memory._before = Peak(server);
            }
            return memory;
         }

         public string Grew(out long grownBytes)
         {
            using (System.Diagnostics.Process server = Server())
            {
               long after = Peak(server);
               if (_before < 0 || after < 0)
               {
                  grownBytes = 0;
                  return "(not measured: the server's process could not be read)";
               }
               grownBytes = after - _before;
               return (grownBytes / (1024 * 1024)) + " MB (" + (_before / (1024 * 1024)) + " MB to " + (after / (1024 * 1024)) + " MB)";
            }
         }

         private static System.Diagnostics.Process Server()
         {
            System.Diagnostics.Process[] found = System.Diagnostics.Process.GetProcessesByName("hmailserver");
            if (found.Length == 0)
               found = System.Diagnostics.Process.GetProcessesByName("hMailServer");
            for (int i = 1; i < found.Length; i++)
               found[i].Dispose();
            return found.Length > 0 ? found[0] : null;
         }

         private static long Peak(System.Diagnostics.Process server)
         {
            if (server == null)
               return -1;
            try
            {
               server.Refresh();
               return server.PeakWorkingSet64;
            }
            catch (Exception)
            {
               return -1;
            }
         }
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
      [Description("An IP range named deliver_remote_to_remote does not relay for its addresses: the value after the next colon in the body, when that is a boolean, is not read as that flag (which let the range relay for senders that authenticate, as require_auth_remote_to_remote defaults to).")]
      public void AnIpRangeNamedAfterAFlagDoesNotSetIt()
      {
         (int status, string body) made = Admin("POST", "/api/v1/ipranges",
            "{\"name\":\"deliver_remote_to_remote\",\"allow_pop3\":true,\"lower\":\"192.0.2.82\",\"upper\":\"192.0.2.82\"}");
         ClassicAssert.AreEqual(201, made.status, made.body);

         string range = RangeEntry("deliver_remote_to_remote");
         ClassicAssert.IsNotNull(range, "The range was not listed.");
         StringAssert.Contains("\"deliver_remote_to_remote\":false", range,
            "The range's name was read as its deliver_remote_to_remote flag, and the boolean after the next colon in the body as its value: " + range);
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
         StringAssert.Contains("\"deleted\":false", flagged.body, "The keyword 'deleted' was read as the deleted flag, from the boolean after the next colon: " + flagged.body);
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
      [Description("An API key's role or expires written as anything but a string is refused, and no key is made: a role as an array beside scope full is not read as no role, which would make the key the server administrator's, and an expiry as a number is not read as none, which would give the key the default's ninety days.")]
      public void AnApiKeysRoleOrExpiryAsAnotherTypeIsRefused()
      {
         var wrong = new StringBuilder();
         var made = new List<string>();
         string[][] cases =
         {
            new[] { "rbr-role", "{\"label\":\"rbr-role\",\"scope\":\"full\",\"role\":[\"helpdesk\"]}", "role must be the name of a role, as a string" },
            new[] { "rbr-expires", "{\"label\":\"rbr-expires\",\"scope\":\"readonly\",\"expires\":1735689600}", "expires must be a date in the form YYYY-MM-DD HH:MM:SS, as a string" },
         };

         foreach (string[] item in cases)
         {
            (int status, string body) answer = Admin("POST", "/api/v1/apikeys", item[1]);
            if (answer.status != 400 || !answer.body.Contains(item[2]))
               wrong.Append("\r\n  " + item[1] + ": " + answer.status + " " + Shorten(Regex.Replace(answer.body, "\"key\":\"[^\"]*\"", "\"key\":\"...\"")));

            Match id = Regex.Match(answer.body, "\"id\":\"([0-9a-f]+)\"");
            if (answer.status == 201 && id.Success)
               made.Add(id.Groups[1].Value);
         }

         string listed = Admin("GET", "/api/v1/apikeys", null).body;
         foreach (string[] item in cases)
         {
            if (listed.Contains("\"label\":\"" + item[0] + "\""))
               wrong.Append("\r\n  a key " + item[0] + " was made");
         }

         // A key made all the same is revoked, so that a failing run leaves no key behind.
         foreach (string id in made)
            Admin("DELETE", "/api/v1/apikeys/" + id, null);

         ClassicAssert.IsTrue(wrong.Length == 0, "An API key's role or expires written as another type than a string was read as absent:" + wrong);
      }

      [Test]
      [Description("A role assignment's domains written as an array is refused, not read as every domain.")]
      public void ARoleAssignmentsDomainsAsAnArrayIsRefused()
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/role-assignments/" + Address, "{\"role\":\"auditor\",\"domains\":[\"" + _domain.Name + "\"]}");
         ClassicAssert.AreEqual(400, answer.status, "A role assigned with domains as an array was assigned in every domain: " + answer.body);
         StringAssert.Contains("domains must be", answer.body);
      }

      [Test]
      [Description("A port's address, connection_security or client_certificate_policy written as anything but a string is refused, on POST and on PUT, not read as the widest default - every interface, no TLS, no client certificate - and nothing is made or changed.")]
      public void APortsWordsWrittenAsAnythingButAStringAreRefused()
      {
         var wrong = new StringBuilder();
         string[][] cases =
         {
            new[] { "address", "\"address\":[\"127.0.0.1\"]" },
            new[] { "connection_security", "\"connection_security\":1" },
            new[] { "client_certificate_policy", "\"client_certificate_policy\":[\"require\"]" },
         };

         for (int i = 0; i < cases.Length; i++)
         {
            int number = ControlPorts[i];
            (int status, string body) made = Admin("POST", "/api/v1/ports", "{\"protocol\":\"pop3\",\"port\":" + number + "," + cases[i][1] + "}", null);
            if (made.status != 400 || !made.body.Contains(cases[i][0] + " must be a string"))
               wrong.Append("\r\n  POST with " + cases[i][1] + ": " + made.status + " " + Shorten(made.body));
         }

         // One port made as it should be, and each word put to it as something else.
         int changed = ControlPorts[3];
         (int status, string body) port = Admin("POST", "/api/v1/ports", "{\"protocol\":\"pop3\",\"port\":" + changed + ",\"address\":\"127.0.0.1\"}", null);
         ClassicAssert.AreEqual(201, port.status, port.body);
         string id = FirstNumber(port.body, "id");
         foreach (string[] item in cases)
         {
            string address = item[0] == "address" ? "" : ",\"address\":\"127.0.0.1\"";
            (int status, string body) put = Admin("PUT", "/api/v1/ports/" + id, "{\"protocol\":\"pop3\",\"port\":" + changed + address + "," + item[1] + "}", null);
            if (put.status != 400 || !put.body.Contains(item[0] + " must be a string"))
               wrong.Append("\r\n  PUT with " + item[1] + ": " + put.status + " " + Shorten(put.body));
         }

         string ports = Admin("GET", "/api/v1/ports", null).body;
         for (int i = 0; i < cases.Length; i++)
         {
            if (ports.Contains("\"port\":" + ControlPorts[i] + ","))
               wrong.Append("\r\n  a port " + ControlPorts[i] + " was made");
         }
         if (!Regex.IsMatch(ports, "\"id\":" + id + ",[^}]*\"address\":\"127\\.0\\.0\\.1\""))
            wrong.Append("\r\n  the port made on 127.0.0.1 was changed: " + Shorten(ports));

         ClassicAssert.IsTrue(wrong.Length == 0, "A port's word written as something other than a string was read as absent:" + wrong);
      }

      [Test]
      [Description("A member whose absence is the wider setting, written as another type than the route reads, is refused rather than read as absent, and nothing is made or changed: a file link's password and a message's passcode as a number (no password, no passcode), the branding's and the files policy's domain as an array (the server's own in place of the domain's), an IP range's flags and a new domain's active as a string (allow_smtp \"false\" allowing SMTP, require_tls_for_auth \"true\" not requiring TLS, active \"false\" taking mail), and a mailbox move's keep_alias as a string (an alias kept where none was asked for).")]
      public void AMemberWhoseAbsenceIsWiderWrittenAsAnotherTypeIsRefused()
      {
         var wrong = new StringBuilder();
         string domain = _domain.Name;
         string mover = "bodies-move@" + domain;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, mover, UserPassword);

         string[][] cases =
         {
            new[] { "user", "POST", "/api/v1/me/files", "{\"name\":\"rbr-link.txt\",\"type\":\"text/plain\",\"size\":3,\"password\":123456}", "password is a string" },
            new[] { "user", "POST", "/api/v1/me/messages", "{\"to\":\"" + ReaderAddress + "\",\"subject\":\"rbr passcode\",\"text\":\"t\",\"expires_in\":3600,\"passcode\":123456}", "passcode must be a string" },
            new[] { "admin", "PUT", "/api/v1/portal/branding", "{\"domain\":[\"" + domain + "\"],\"announcement\":\"rbr-announcement\"}", "domain is a domain name, as a string" },
            new[] { "admin", "PUT", "/api/v1/portal/files", "{\"domain\":[\"" + domain + "\"],\"days\":3}", "domain is a domain name, as a string" },
            new[] { "admin", "POST", "/api/v1/ipranges", "{\"name\":\"rbr-smtp\",\"lower\":\"192.0.2.83\",\"upper\":\"192.0.2.83\",\"allow_smtp\":\"false\"}", "allow_smtp must be true or false" },
            new[] { "admin", "POST", "/api/v1/ipranges", "{\"name\":\"rbr-tls\",\"lower\":\"192.0.2.84\",\"upper\":\"192.0.2.84\",\"require_tls_for_auth\":\"true\"}", "require_tls_for_auth must be true or false" },
            new[] { "admin", "POST", "/api/v1/domains", "{\"name\":\"rbr-active.test\",\"active\":\"false\"}", "active must be true or false" },
            new[] { "admin", "POST", "/api/v1/accounts/" + mover + "/move", "{\"new_address\":\"bodies-moved@" + domain + "\",\"keep_alias\":\"false\"}", "keep_alias must be true or false" },
         };

         foreach (string[] item in cases)
         {
            (int status, string body) answer = Send(item[0], item[1], item[2], item[3], null);
            if (answer.status != 400 || !answer.body.Contains(item[4]))
               wrong.Append("\r\n  " + item[1] + " " + item[2] + " " + item[3] + ": " + answer.status + " " + Shorten(answer.body));
         }

         if (User("GET", "/api/v1/me/files", null).body.Contains("rbr-link.txt"))
            wrong.Append("\r\n  a file link was made");
         if (User("GET", "/api/v1/me/confidential", null).body.Contains("rbr passcode"))
            wrong.Append("\r\n  a message that expires was sent");
         if (Admin("GET", "/api/v1/portal/branding", null).body.Contains("rbr-announcement"))
            wrong.Append("\r\n  the server's own announcement was written");
         string files = Admin("GET", "/api/v1/portal/files", null).body;
         if (Regex.IsMatch(files, "\"days\":3[,}]"))
            wrong.Append("\r\n  the server's files policy was written: " + Shorten(files));
         string ranges = Admin("GET", "/api/v1/ipranges", null).body;
         foreach (string name in new[] { "rbr-smtp", "rbr-tls" })
         {
            if (ranges.Contains("\"name\":\"" + name + "\""))
               wrong.Append("\r\n  an IP range " + name + " was made");
         }
         if (Admin("GET", "/api/v1/domains/rbr-active.test", null).status == 200)
            wrong.Append("\r\n  the domain rbr-active.test was made");
         if (Admin("GET", "/api/v1/accounts/bodies-moved@" + domain, null).status == 200)
            wrong.Append("\r\n  the mailbox was moved");

         ClassicAssert.IsTrue(wrong.Length == 0, "A member whose absence is the wider setting, written as another type, was read as absent:" + wrong);
      }

      [Test]
      [Description("A migration's jobs given as a JSON object are held to what every JSON body is - a name given twice and an escaped U+0000 are refused - while CSV is taken as before.")]
      public void AMigrationsJobsAsJsonAreHeldToTheRule()
      {
         (int status, string body) migration = Admin("POST", "/api/v1/migrations", "{\"name\":\"rbr-migration\",\"kind\":\"imap\",\"host\":\"imap.example.net\"}", null);
         ClassicAssert.AreEqual(201, migration.status, migration.body);
         string jobs = "/api/v1/migrations/" + FirstNumber(migration.body, "id") + "/jobs";

         var wrong = new StringBuilder();
         string[][] refused =
         {
            new[] { "names one member twice", "{\"source_user\":\"rbr-one\",\"secret\":\"Source-1\",\"target\":\"" + Address + "\",\"source_user\":\"rbr-two\"}" },
            new[] { "U+0000", "{\"source_user\":\"rbr-three" + EscapedNul + "z\",\"secret\":\"Source-3\",\"target\":\"" + Address + "\"}" },
            new[] { "U+0000", ByteOrderMark + " {\"source_user\":\"rbr-four" + EscapedNul + "\",\"secret\":\"Source-4\",\"target\":\"" + Address + "\"}" },
         };
         foreach (string[] item in refused)
         {
            (int status, string body) answer = Admin("POST", jobs, item[1], null);
            if (answer.status != 400 || !answer.body.Contains(Refusal) || !answer.body.Contains(item[0]))
               wrong.Append("\r\n  " + item[1] + ": " + answer.status + " " + Shorten(answer.body));
         }

         string listed = Admin("GET", jobs, null).body;
         foreach (string user in new[] { "rbr-one", "rbr-two", "rbr-three", "rbr-four" })
         {
            if (listed.Contains(user))
               wrong.Append("\r\n  a job for " + user + " was made");
         }

         (int status, string body) csv = Admin("POST", jobs, "rbr-csv,Source-5," + Address, null);
         if (csv.status != 201)
            wrong.Append("\r\n  CSV: " + csv.status + " " + Shorten(csv.body));
         (int status, string body) json = Admin("POST", jobs, "{\"source_user\":\"rbr-json\",\"secret\":\"Source-6\",\"target\":\"" + Address + "\"}", null);
         if (json.status != 201)
            wrong.Append("\r\n  a JSON object: " + json.status + " " + Shorten(json.body));

         Admin("DELETE", "/api/v1/migrations/" + FirstNumber(migration.body, "id"), null);
         ClassicAssert.IsTrue(wrong.Length == 0, "A migration's jobs were read otherwise than every JSON body is:" + wrong);
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

         string ports = Admin("GET", "/api/v1/ports", null).body;
         foreach (int number in ControlPorts)
         {
            foreach (Match port in Regex.Matches(ports, "\\{\"id\":(\\d+),[^}]*\"port\":" + number + ","))
               Admin("DELETE", "/api/v1/ports/" + port.Groups[1].Value, null);
         }

         string migrations = Admin("GET", "/api/v1/migrations", null).body;
         foreach (Match migration in Regex.Matches(migrations, "\"id\":(\\d+),\"name\":\"rbr-migration\""))
            Admin("DELETE", "/api/v1/migrations/" + migration.Groups[1].Value, null);

         // What a server that read a member of another type as absent wrote in
         // the server's own name.
         Admin("DELETE", "/api/v1/domains/rbr-active.test", null);
         Admin("PUT", "/api/v1/portal/branding", "{\"announcement\":\"\"}", null);
         Admin("PUT", "/api/v1/portal/files", "{\"days\":null}", null);
      }

      // A password-protected message from the fixture's account to the reader,
      // complete, as the webmail makes one: its token.
      private string MakeProtectedMessage()
      {
         string vector = RestApiProtectedMessages.Vector;
         (int status, string body) made = User("POST", "/api/v1/me/protected",
            "{\"recipient\":\"" + ReaderAddress + "\",\"hint\":\"rbr\",\"size\":" + vector.Length + "}");
         ClassicAssert.AreEqual(201, made.status, made.body);
         string id = FirstNumber(made.body, "id");
         string token = Regex.Match(made.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;

         (int status, string body) content = User("PUT", "/api/v1/me/protected/" + id + "/content?offset=0", vector);
         ClassicAssert.AreEqual(200, content.status, content.body);
         StringAssert.Contains("\"complete\":true", content.body);
         return token;
      }

      // Text as a JSON string's contents.
      private static string JsonText(string text)
      {
         return text.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");
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
