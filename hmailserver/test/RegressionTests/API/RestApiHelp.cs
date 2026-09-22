// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Help where the question is: the documentation the server carries,
   ///    served at GET /api/v1/help and /api/v1/help/{page} for the two browser
   ///    pages' help panes, and the wiki base URL the panes link to.
   ///
   ///    The text is the repository's own hmailserver/docs, embedded by
   ///    build/generate-help-pages.py, so this fixture reads the files from the
   ///    tree it is built in and holds the routes to them: every document is
   ///    listed with the headings the file has, and every page is answered byte
   ///    for byte as the file is (its CRLF made LF, as the generator makes it).
   ///    Nothing here needs a credential, because the text is public; the
   ///    setting round-trips over REST and COM, an empty value is allowed on both
   ///    doors, and a value that is not a URL is refused on both with nothing
   ///    changed.
   /// </summary>
   [TestFixture]
   public class RestApiHelp : TestFixtureBase
   {
      private static int RestPort = 9826;
      private const string AdminPassword = "testar";

      private string _wikiBefore;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         _wikiBefore = _settings.HelpWikiBaseUrl;

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            _settings.HelpWikiBaseUrl = _wikiBefore;
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("GET /api/v1/help lists every document of hmailserver/docs but README.md and DeckParity.md, by its name, with its first heading as its title, its size in bytes and its level-one and level-two headings with their lines - as the files in the tree have them - the wiki setting's value, and a build tag; and no credential is asked.")]
      public void TheListAnswersEveryDocumentWithItsHeadings()
      {
         Response listed = Raw("GET", "/api/v1/help", null, null);
         Assert.AreEqual(200, listed.Status, listed.Body);
         StringAssert.StartsWith("application/json", listed.Header("Content-Type"));

         Dictionary<string, object> answer = MiniJson.Object(listed.Body);
         Assert.AreEqual(_settings.HelpWikiBaseUrl, (string) answer["wiki"]);
         StringAssert.IsMatch("^[0-9a-f]{16}$", (string) answer["tag"], "The build tag is sixteen hex digits.");

         var pages = (List<object>) answer["pages"];
         var listedNames = new List<string>();
         foreach (object item in pages)
         {
            var page = (Dictionary<string, object>) item;
            string name = (string) page["name"];
            listedNames.Add(name);

            string text = DocumentText(name);
            List<Heading> expected = Headings(text);

            Assert.AreEqual(expected.Count > 0 && expected[0].Level == 1 ? expected[0].Text : name, (string) page["title"], name + ": the title is the first heading");
            Assert.AreEqual(Encoding.UTF8.GetByteCount(text), Convert.ToInt64(page["bytes"]), name + ": the byte length is the file's");

            var headings = (List<object>) page["headings"];
            Assert.AreEqual(expected.Count, headings.Count, name + ": the number of headings");
            for (int i = 0; i < expected.Count; i++)
            {
               var heading = (Dictionary<string, object>) headings[i];
               Assert.AreEqual(expected[i].Text, (string) heading["text"], name + ": heading " + i);
               Assert.AreEqual(expected[i].Level, Convert.ToInt32(heading["level"]), name + ": level of " + expected[i].Text);
               Assert.AreEqual(expected[i].Line, Convert.ToInt32(heading["line"]), name + ": line of " + expected[i].Text);
            }
         }

         CollectionAssert.AreEqual(DocumentNames(), listedNames, "Every document in the tree, in file-name order, and nothing else.");
      }

      [Test]
      [Description("GET /api/v1/help/{page} answers the document's markdown byte for byte as the file in the tree (CRLF made LF), as text/markdown with a day's Cache-Control, nosniff and an ETag of the build tag and the page name; a request whose If-None-Match names that tag is answered 304 with no body; the name is matched without regard to case; and no credential is asked.")]
      public void APageAnswersItsMarkdownByteForByte()
      {
         string tag = (string) MiniJson.Object(Http("GET", "/api/v1/help", null, authorization: null).body)["tag"];

         foreach (string name in DocumentNames())
         {
            Response page = Raw("GET", "/api/v1/help/" + name, null, null);
            Assert.AreEqual(200, page.Status, name + ": " + page.Body);
            Assert.AreEqual("text/markdown; charset=utf-8", page.Header("Content-Type"), name);
            Assert.AreEqual("public, max-age=86400", page.Header("Cache-Control"), name);
            Assert.AreEqual("nosniff", page.Header("X-Content-Type-Options"), name);
            Assert.AreEqual("\"" + tag + "-" + name + "\"", page.Header("ETag"), name);
            CollectionAssert.AreEqual(Encoding.UTF8.GetBytes(DocumentText(name)), page.BodyBytes, name + ": the bytes are the file's");
         }

         Response backup = Raw("GET", "/api/v1/help/Backup", null, null);
         Response notModified = Raw("GET", "/api/v1/help/Backup", null, null, "If-None-Match: " + backup.Header("ETag") + "\r\n");
         Assert.AreEqual(304, notModified.Status);
         Assert.AreEqual(0, notModified.BodyBytes.Length, "A 304 carries no body.");
         Assert.AreEqual(backup.Header("ETag"), notModified.Header("ETag"));

         Response other = Raw("GET", "/api/v1/help/Backup", null, null, "If-None-Match: \"" + tag + "-Upgrading\"\r\n");
         Assert.AreEqual(200, other.Status, "Another page's tag is not this page's.");

         Response lowered = Raw("GET", "/api/v1/help/backup", null, null);
         Assert.AreEqual(200, lowered.Status);
         CollectionAssert.AreEqual(backup.BodyBytes, lowered.BodyBytes, "The name is matched without regard to case.");
         Assert.AreEqual(backup.Header("ETag"), lowered.Header("ETag"), "And the ETag is the page's, as spelled.");
      }

      [Test]
      [Description("A name with a path, a dot-dot, an unknown name and a name with an extension are one and the same 404, so a request that tries the path for a file learns nothing.")]
      public void AnythingButABarePageNameIsNotFound()
      {
         foreach (string path in new[] { "/api/v1/help/sub/Backup", "/api/v1/help/../Backup", "/api/v1/help/NoSuchPage", "/api/v1/help/Backup.md", "/api/v1/help/Backup%2F", "/api/v1/help/" })
         {
            (int status, string body) answer = Http("GET", path, null, authorization: null);
            Assert.AreEqual(404, answer.status, path + ": " + answer.body);
            StringAssert.Contains("\"error\":\"no such page\"", answer.body, path);
         }
      }

      [Test]
      [Description("Neither route asks for a credential: the text is the repository's public documentation, and the Deck's sign-in card offers help before there is a session.")]
      public void NoCredentialIsAsked()
      {
         Assert.AreEqual(200, Http("GET", "/api/v1/help", null, authorization: null).status);
         Assert.AreEqual(200, Http("GET", "/api/v1/help/Backup", null, authorization: null).status);
         Assert.AreEqual(200, Http("GET", "/api/v1/help/Backup", null, authorization: "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:not-the-password"))).status,
            "A wrong credential on a public route is not a refusal either; the route reads none.");
      }

      [Test]
      [Description("The wiki base URL round-trips over REST (help_wiki_base_url on GET and PUT /api/v1/settings, and the wiki field of GET /api/v1/help) and over COM (Settings.HelpWikiBaseUrl); it is trimmed; empty is allowed on both doors and hides the link; and a value that is not an absolute http or https URL is refused on both doors in the same words with nothing changed.")]
      public void TheWikiSettingRoundTripsOverRestAndCom()
      {
         (int status, string body) saved = Http("PUT", "/api/v1/settings", "{\"help_wiki_base_url\":\"  https://wiki.example.test/pages/  \"}");
         Assert.AreEqual(200, saved.status, saved.body);
         Assert.AreEqual("https://wiki.example.test/pages/", _settings.HelpWikiBaseUrl, "Trimmed, and what COM reads.");
         StringAssert.Contains("\"help_wiki_base_url\":\"https://wiki.example.test/pages/\"", Http("GET", "/api/v1/settings", null).body);
         Assert.AreEqual("https://wiki.example.test/pages/", (string) MiniJson.Object(Http("GET", "/api/v1/help", null, authorization: null).body)["wiki"]);

         _settings.HelpWikiBaseUrl = " http://other.example.test/w/ ";
         Assert.AreEqual("http://other.example.test/w/", _settings.HelpWikiBaseUrl, "COM trims too.");
         Assert.AreEqual("http://other.example.test/w/", (string) MiniJson.Object(Http("GET", "/api/v1/help", null, authorization: null).body)["wiki"]);

         Assert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"help_wiki_base_url\":\"\"}").status);
         Assert.AreEqual("", _settings.HelpWikiBaseUrl, "Empty is a value: no link.");
         Assert.AreEqual("", (string) MiniJson.Object(Http("GET", "/api/v1/help", null, authorization: null).body)["wiki"]);

         _settings.HelpWikiBaseUrl = "https://wiki.example.test/";
         _settings.HelpWikiBaseUrl = "";
         Assert.AreEqual("", _settings.HelpWikiBaseUrl, "Empty over COM as well.");

         _settings.HelpWikiBaseUrl = "https://wiki.example.test/";
         (int status, string body) refused = Http("PUT", "/api/v1/settings", "{\"help_wiki_base_url\":\"wiki.example.test\"}");
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("begins with http:// or https://", refused.body);
         Assert.AreEqual("https://wiki.example.test/", _settings.HelpWikiBaseUrl, "A refused value changes nothing.");

         COMException viaCom = Assert.Throws<COMException>(() => _settings.HelpWikiBaseUrl = "ftp://wiki.example.test/");
         StringAssert.Contains("begins with http:// or https://", viaCom.Message);
         Assert.AreEqual("https://wiki.example.test/", _settings.HelpWikiBaseUrl, "A refused value changes nothing over COM either.");
      }

      // ---- the documents in the tree ----------------------------------------

      private static string RepositoryRoot
      {
         get { return Path.GetFullPath(Paths.Combine(TestContext.CurrentContext.TestDirectory, @"..\..\..\..\..\..")); }
      }

      private static string DocsDirectory
      {
         get { return Paths.Combine(RepositoryRoot, @"hmailserver\docs"); }
      }

      /// <summary>The documents the generator embeds, in file-name order: every .md but the index and the census.</summary>
      private static List<string> DocumentNames()
      {
         Assert.IsTrue(Directory.Exists(DocsDirectory), "Missing directory: " + DocsDirectory);
         return Directory.GetFiles(DocsDirectory, "*.md")
            .Select(Path.GetFileName)
            .Where(f => f != "README.md" && f != "DeckParity.md")
            .Select(f => f.Substring(0, f.Length - 3))
            .OrderBy(n => n, StringComparer.Ordinal)
            .ToList();
      }

      /// <summary>The file's text as the generator embeds it: UTF-8, CRLF made LF.</summary>
      private static string DocumentText(string name)
      {
         string path = Paths.Combine(DocsDirectory, name + ".md");
         Assert.IsTrue(File.Exists(path), "Missing file: " + path);
         return File.ReadAllText(path, Encoding.UTF8).Replace("\r\n", "\n");
      }

      private sealed class Heading
      {
         public string Text;
         public int Level;
         public int Line;
      }

      private static readonly Regex Atx = new Regex(@"^(#{1,6})[ \t]+(.*?)[ \t]*#*[ \t]*$");
      private static readonly Regex SetextOne = new Regex(@"^=+[ \t]*$");
      private static readonly Regex SetextTwo = new Regex(@"^-+[ \t]*$");
      private static readonly Regex Fence = new Regex(@"^[ \t]{0,3}(```|~~~)");
      private static readonly Regex ListItem = new Regex(@"^[ \t]*(?:[-*+]|\d+[.)])[ \t]+");

      /// <summary>
      ///    The level-one and level-two headings, ATX or setext, outside fenced
      ///    code, as build/generate-help-pages.py lists them: the same rules,
      ///    written a second time so that the server's list is held to the file
      ///    and not to the generator's own reading of it.
      /// </summary>
      private static List<Heading> Headings(string text)
      {
         var found = new List<Heading>();
         string[] lines = text.Split('\n');
         string inFence = null;
         for (int index = 0; index < lines.Length; index++)
         {
            string line = lines[index];
            Match fence = Fence.Match(line);
            if (fence.Success)
            {
               if (inFence == null)
                  inFence = fence.Groups[1].Value;
               else if (fence.Groups[1].Value == inFence)
                  inFence = null;
               continue;
            }
            if (inFence != null)
               continue;

            Match atx = Atx.Match(line);
            if (atx.Success)
            {
               if (atx.Groups[1].Value.Length <= 2 && atx.Groups[2].Value.Length > 0)
                  found.Add(new Heading { Text = atx.Groups[2].Value, Level = atx.Groups[1].Value.Length, Line = index });
               continue;
            }
            if (index == 0)
               continue;

            int underline = SetextOne.IsMatch(line) ? 1 : SetextTwo.IsMatch(line) ? 2 : 0;
            if (underline == 0)
               continue;

            string above = lines[index - 1];
            if (above.Trim().Length == 0 || ListItem.IsMatch(above) || above.TrimStart().StartsWith("|") || Atx.IsMatch(above))
               continue;
            if (SetextOne.IsMatch(above) || SetextTwo.IsMatch(above) || Fence.IsMatch(above))
               continue;

            found.Add(new Heading { Text = above.Trim(), Level = underline, Line = index - 1 });
         }
         return found;
      }

      // ---- HTTP ----------------------------------------------------------------

      private static (int status, string body) Http(string method, string path, string requestBody)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) Http(string method, string path, string requestBody, string authorization)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public byte[] BodyBytes = new byte[0];
         public readonly Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : "";
         }
      }

      /// <summary>
      ///    One request, with the head parsed and the body kept as bytes, so that
      ///    a page can be held to its file byte for byte and a 304 to having none.
      /// </summary>
      private static Response Raw(string method, string path, string authorization, string requestBody, string extraHeaders = null)
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
               if (extraHeaders != null)
                  headers.Append(extraHeaders);

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

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               byte[] all = memory.ToArray();
               var response = new Response();

               int separator = IndexOfHeadEnd(all);
               string head = Encoding.ASCII.GetString(all, 0, separator >= 0 ? separator : all.Length);

               if (separator >= 0)
               {
                  response.BodyBytes = new byte[all.Length - separator - 4];
                  Array.Copy(all, separator + 4, response.BodyBytes, 0, response.BodyBytes.Length);
                  response.Body = Encoding.UTF8.GetString(response.BodyBytes);
               }

               string[] lines = head.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out response.Status);
               }

               for (int i = 1; i < lines.Length; i++)
               {
                  int colon = lines[i].IndexOf(':');
                  if (colon > 0)
                     response.Headers[lines[i].Substring(0, colon).Trim()] = lines[i].Substring(colon + 1).Trim();
               }

               return response;
            }
         }
      }

      private static int IndexOfHeadEnd(byte[] bytes)
      {
         for (int i = 0; i + 3 < bytes.Length; i++)
         {
            if (bytes[i] == 13 && bytes[i + 1] == 10 && bytes[i + 2] == 13 && bytes[i + 3] == 10)
               return i;
         }
         return -1;
      }
   }
}
