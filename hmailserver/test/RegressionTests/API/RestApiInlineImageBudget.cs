// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The webmail's view of a message's HTML (GET /api/v1/me/messages/{id}/html)
   ///    puts each image the message embeds back in place as a data: URL. The images
   ///    were held to 4 MB each and 12 MB together, but counted once a part however
   ///    many times the HTML named it, and every reference was replaced in place, the
   ///    rest of the HTML moved along each time: a sender naming one small image a
   ///    great many times made the route build a document many times larger than the
   ///    message, in a time that grew with the square of the count, on one of the REST
   ///    listener's four workers.
   ///
   ///    The route now reads the HTML once to learn what it names and once more as it
   ///    writes it, and the data: URLs it writes come to 24 MB at most, every
   ///    reference counted; a reference past that is left as the cid: URL it was.
   ///    Each test fails against 6.3.5's route: the first on the size of what it
   ///    writes, the second on the time it takes as well.
   /// </summary>
   [TestFixture]
   public class RestApiInlineImageBudget : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the suite's
      // own where RestListener finds one already on.
      private static int RestPort = 9573;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Original-Passw0rd!";

      // What the route writes of data: URLs at most, every reference counted
      // (RestApiServer.cpp, HandleMeMessageHtml_, InlineWritten).
      private const long InlineWritten = 24L * 1024 * 1024;

      private string Address
      {
         get { return "inline@" + _domain.Name; }
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
      [Description("One image named twenty thousand times is written as a data: URL until 24 MB of them are written, and the references after that stay cid: URLs")]
      public void AnImageNamedOverAndOverIsWrittenUntil24MBAndNoFurther()
      {
         const int References = 20000;
         byte[] image = ImageBytes(1500);
         string dataUrl = "data:image/png;base64," + Convert.ToBase64String(image);

         // 6.3.5's route wrote every one: some 40 MB from a message of under 1 MB.
         Assert.Greater((long) References * dataUrl.Length, InlineWritten + InlineWritten / 2);

         long id = Deliver("Named over and over", image, References);
         var clock = Stopwatch.StartNew();
         (int status, string body) shown = Me("GET", "/api/v1/me/messages/" + id + "/html");
         clock.Stop();
         Assert.AreEqual(200, shown.status, "Body: " + Truncated(shown.body));

         int written = Occurrences(shown.body, dataUrl);
         int left = Occurrences(shown.body, "src=\"cid:p@i\"");
         int expected = (int) (InlineWritten / dataUrl.Length);
         Assert.AreEqual(expected, written,
            "The data: URLs written must stop at 24 MB, every reference counted (" + shown.body.Length + " characters answered in " + clock.ElapsedMilliseconds + " ms).");
         Assert.AreEqual(References - expected, left, "A reference past the 24 MB must stay the cid: URL it was.");
         Assert.Less(shown.body.Length, InlineWritten + References * 40L, "The document is the HTML and 24 MB of data: URLs at most.");
         StringAssert.Contains("<p>First</p>", shown.body, "The HTML before the images is all there.");
         StringAssert.Contains("<p>Last</p>", shown.body, "And so is the HTML after them.");
      }

      [Test]
      [Description("A message naming one small image half a million times is answered in seconds, in one pass over its HTML, and still within the 24 MB of data: URLs")]
      public void HalfAMillionReferencesAreAnsweredInSeconds()
      {
         const int References = 500000;
         byte[] image = ImageBytes(30);
         string dataUrl = "data:image/png;base64," + Convert.ToBase64String(image);

         long id = Deliver("Half a million", image, References);
         var clock = Stopwatch.StartNew();
         (int status, string body) shown = Me("GET", "/api/v1/me/messages/" + id + "/html");
         clock.Stop();
         Assert.AreEqual(200, shown.status, "Body: " + Truncated(shown.body));

         // 6.3.5's route moved the rest of the HTML along at each of the half a
         // million references: its replace loop alone took 47 seconds over this
         // HTML on the laptop this was written on, and its time grows with the
         // square of the count; 6.3.5's server took 39 seconds to answer in the CI
         // image. One pass, the message parsed and 33 MB sent with it, took two to
         // three seconds there; twelve leave room for a slower runner and none for
         // the square.
         TestContext.Progress.WriteLine("Half a million references answered in " + clock.ElapsedMilliseconds + " ms, " + shown.body.Length + " characters.");
         Assert.Less(clock.Elapsed.TotalSeconds, 12.0, "Half a million references must be one pass over the HTML, not one pass each (" + clock.ElapsedMilliseconds + " ms).");

         int expected = (int) Math.Min(References, InlineWritten / dataUrl.Length);
         Assert.AreEqual(expected, Occurrences(shown.body, dataUrl), "The data: URLs written must stop at 24 MB.");
         Assert.AreEqual(References - expected, Occurrences(shown.body, "src=\"cid:p@i\""), "The rest stay cid: URLs.");
         StringAssert.Contains("<p>Last</p>", shown.body, "The HTML after the images is all there.");
      }

      // A message whose HTML names one inline image part `references` times, a
      // line each, between a first and a last paragraph; delivered, and its id
      // as the account's REST listing gives it.
      private long Deliver(string subject, byte[] image, int references)
      {
         var html = new StringBuilder(references * 21 + 64);
         html.Append("<p>First</p>\r\n");
         for (int i = 0; i < references; i++)
            html.Append("<img src=\"cid:p@i\">\r\n");
         html.Append("<p>Last</p>\r\n");

         string encoded = Regex.Replace(Convert.ToBase64String(image), "(.{76})", "$1\r\n");
         var client = new SmtpClientSimulator();
         client.SendRaw("sender@example.com", Address,
            "From: sender@example.com\r\nTo: " + Address + "\r\nSubject: " + subject + "\r\nMIME-Version: 1.0\r\n" +
            "Content-Type: multipart/related; boundary=\"rel\"\r\n\r\n--rel\r\nContent-Type: text/html; charset=utf-8\r\n\r\n" +
            html +
            "--rel\r\nContent-Type: image/png\r\nContent-ID: <p@i>\r\nContent-Transfer-Encoding: base64\r\n\r\n" + encoded + "\r\n--rel--\r\n");
         Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);

         (int status, string body) tree = Me("GET", "/api/v1/me/folders");
         Assert.AreEqual(200, tree.status, "Body: " + Truncated(tree.body));
         long inboxId = IdBefore(tree.body, "\"path\":\"INBOX\"");
         (int status, string body) page = Me("GET", "/api/v1/me/folders/" + inboxId + "/messages");
         Assert.AreEqual(200, page.status, "Body: " + Truncated(page.body));
         return IdBefore(page.body, "\"subject\":\"" + subject + "\"");
      }

      // Bytes that start as a PNG does.
      private static byte[] ImageBytes(int length)
      {
         byte[] bytes = new byte[length];
         byte[] signature = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
         Array.Copy(signature, bytes, signature.Length);
         for (int i = signature.Length; i < length; i++)
            bytes[i] = (byte) (i % 251);
         return bytes;
      }

      private static int Occurrences(string text, string what)
      {
         int count = 0;
         for (int at = text.IndexOf(what, StringComparison.Ordinal); at >= 0; at = text.IndexOf(what, at + what.Length, StringComparison.Ordinal))
            count++;
         return count;
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
