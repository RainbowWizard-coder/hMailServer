// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A message with an HTML part and no text part is read for its text where the
   ///    webmail lists it (the snippet of GET /api/v1/me/folders/{id}/messages), where
   ///    an IMAP client asks for its PREVIEW and where a message that expires is read:
   ///    tags dropped, then the entities decoded with String::Replace. Replace replaced
   ///    each occurrence in place and moved the whole rest of the string along every
   ///    time, so a sender's HTML part of a great many entities held the REST worker
   ///    that listed the inbox for a time that grew with the square of their number -
   ///    no click needed, the listing is the first thing the webmail asks for.
   ///
   ///    Replace (StdString.h) now writes a string of another length in one pass, and
   ///    Remove takes every character it removes in one pass too; what they make is
   ///    as it was. The test fails against 6.3.5's server on the time the listing
   ///    takes.
   /// </summary>
   [TestFixture]
   public class RestApiHtmlOnlyText : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the suite's
      // own where RestListener finds one already on.
      private static int RestPort = 9575;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Original-Passw0rd!";

      private string Address
      {
         get { return "htmlonly@" + _domain.Name; }
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

         (int status, string body) probe = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/status", BasicHeader("Administrator", AdminPassword), null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("An HTML-only message of half a million entities is listed, its snippet decoded, in seconds")]
      public void AnHtmlOnlyMessageOfEntitiesIsListedInSeconds()
      {
         const int Entities = 500000;
         var html = new StringBuilder(Entities * 4 + 256);
         html.Append("<p>Opening words</p>\r\n<p>");
         for (int i = 0; i < Entities; i++)
         {
            html.Append("&lt;");
            if (i % 19 == 18)
               html.Append("\r\n");
         }
         html.Append("</p>\r\n<p>Closing words</p>\r\n");

         var client = new SmtpClientSimulator();
         client.SendRaw("sender@example.com", Address,
            "From: sender@example.com\r\nTo: " + Address + "\r\nSubject: Entities\r\nMIME-Version: 1.0\r\n" +
            "Content-Type: text/html; charset=utf-8\r\n\r\n" + html);
         Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);

         (int status, string body) tree = Me("GET", "/api/v1/me/folders");
         Assert.AreEqual(200, tree.status, "Body: " + Truncated(tree.body));
         long inboxId = IdBefore(tree.body, "\"path\":\"INBOX\"");

         // 6.3.5's server moved the rest of the text along at each of the half
         // million: some 500 billion characters, and its time grows with the square
         // of the count - 300,000 of them took it 15 seconds to list in the CI image.
         // One pass takes milliseconds; ten seconds leave room for a slower runner
         // and none for the square.
         var clock = Stopwatch.StartNew();
         (int status, string body) page = Me("GET", "/api/v1/me/folders/" + inboxId + "/messages");
         clock.Stop();
         TestContext.Progress.WriteLine("Listed an HTML-only message of " + Entities + " entities in " + clock.ElapsedMilliseconds + " ms.");
         Assert.AreEqual(200, page.status, "Body: " + Truncated(page.body));
         Assert.Less(clock.Elapsed.TotalSeconds, 10.0, "The listing must read the message's text in one pass, not one pass an entity (" + clock.ElapsedMilliseconds + " ms).");

         // The snippet is the text as before: the first paragraph, then the
         // entities decoded.
         int snippetAt = page.body.IndexOf("\"snippet\":\"", StringComparison.Ordinal);
         Assert.IsTrue(snippetAt >= 0, "No snippet in: " + Truncated(page.body));
         string snippet = page.body.Substring(snippetAt + 11, Math.Min(200, page.body.Length - snippetAt - 11));
         StringAssert.StartsWith("Opening words", snippet, "The snippet starts with the text: " + snippet);
         StringAssert.DoesNotContain("&lt;", snippet, "The entities are decoded: " + snippet);
         Assert.IsTrue(snippet.Contains("<<<<") || snippet.Contains("\\u003c\\u003c"), "The entities are decoded to the character: " + snippet);
      }

      // The id of the object a marker sits in: the last "id" before it.
      private static long IdBefore(string body, string marker)
      {
         int at = body.IndexOf(marker, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "Missing " + marker + " in: " + Truncated(body));
         int idAt = body.LastIndexOf("\"id\":", at, StringComparison.Ordinal);
         Assert.IsTrue(idAt >= 0, "No id before " + marker + " in: " + Truncated(body));
         int end = idAt + 5;
         while (end < body.Length && char.IsDigit(body[end]))
            end++;
         return long.Parse(body.Substring(idAt + 5, end - idAt - 5));
      }

      private static string Truncated(string value)
      {
         return value.Length <= 2000 ? value : value.Substring(0, 2000) + "... (" + value.Length + " characters)";
      }

      private (int status, string body) Me(string method, string path)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, BasicHeader(Address, UserPassword), null);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }
   }
}
