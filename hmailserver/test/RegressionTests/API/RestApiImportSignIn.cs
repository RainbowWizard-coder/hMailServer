// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.ExternalAccounts;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    Importing from the old provider with a sign-in (ReaderOAuth.h): the reader
   ///    starts at /api/v1/me/fetch-accounts/import, the provider sends the browser
   ///    back to /portal/oauth/callback, and an IMAP fetch account is made that logs
   ///    in with XOAUTH2 and brings over the folders the reader picks. The provider is
   ///    a local token endpoint - the code exchange and the refresh are the server's
   ///    own requests, so that is where they are seen - and the other mail server is a
   ///    ScriptedImapServer the account is pointed at by the administrator's route.
   /// </summary>
   [TestFixture]
   public class RestApiImportSignIn : TestFixtureBase
   {
      private static int RestPort = 9615;
      private const int ImapPort = 9616;
      private const int TokenPort = 9617;
      private const string AdminPassword = "testar";
      private const string ReaderPassword = "Import-SignIn-Passw0rd!";
      private const string SignedInAddress = "reader@gmail.example";

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
            Admin("PUT", "/api/v1/settings",
               "{\"import_oauth_redirect_base_url\":\"\",\"import_oauth_google_client_id\":\"\",\"import_oauth_google_client_secret\":\"\"," +
               "\"import_oauth_google_authorize_url\":\"https://accounts.google.com/o/oauth2/v2/auth\",\"import_oauth_google_token_url\":\"https://oauth2.googleapis.com/token\"," +
               "\"import_oauth_microsoft_client_id\":\"\",\"import_oauth_microsoft_client_secret\":\"\"}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A provider is offered once it is registered; the start sends the browser to the provider with PKCE and a state, and gives it an HttpOnly cookie for the callback.")]
      public void AProviderIsOfferedOnceRegisteredAndTheStartCarriesPkce()
      {
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "import-offer@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);

         Assert.AreEqual("{\"google\":false,\"microsoft\":false}", Send("GET", "/api/v1/me/fetch-accounts/import", me, null).body);
         Assert.AreEqual(403, Send("POST", "/api/v1/me/fetch-accounts/import", me, "{\"provider\":\"google\"}").status, "Not set up, not offered.");

         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"import_oauth_google_token_url\":\"http://mail.example.test/token\"}").status,
            "An endpoint in the clear to another machine is refused.");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings",
            "{\"import_oauth_redirect_base_url\":\"https://mail.example.test/\",\"import_oauth_google_client_id\":\"client-g\",\"import_oauth_google_client_secret\":\"secret-g\"," +
            "\"import_oauth_microsoft_client_id\":\"client-m\"}").status);
         StringAssert.DoesNotContain("secret-g", Admin("GET", "/api/v1/settings", null).body, "The secret is never emitted.");

         Assert.AreEqual("{\"google\":true,\"microsoft\":true}", Send("GET", "/api/v1/me/fetch-accounts/import", me, null).body);
         Assert.AreEqual(400, Send("POST", "/api/v1/me/fetch-accounts/import", me, "{\"provider\":\"yahoo\"}").status);
         Assert.AreEqual(400, Send("POST", "/api/v1/me/fetch-accounts/import", me, "{\"provider\":\"google\",\"server_address\":\"evil.example\"}").status);

         Response started = Raw("POST", "/api/v1/me/fetch-accounts/import", me, "{\"provider\":\"google\"}", null);
         Assert.AreEqual(200, started.Status, started.Body);
         string url = Regex.Unescape(Regex.Match(started.Body, "\"url\":\"([^\"]+)\"").Groups[1].Value);
         StringAssert.StartsWith("https://accounts.google.com/o/oauth2/v2/auth?response_type=code&client_id=client-g&", url);
         StringAssert.Contains("&redirect_uri=https%3A%2F%2Fmail.example.test%2Fportal%2Foauth%2Fcallback&", url, "The base URL's trailing slash is not doubled.");
         StringAssert.Contains("&scope=https%3A%2F%2Fmail.google.com%2F%20openid%20email&", url);
         StringAssert.Contains("&code_challenge_method=S256", url);
         StringAssert.Contains("&access_type=offline&prompt=consent", url);
         Assert.AreEqual(43, Parameter(url, "state").Length, "32 random bytes, base64url.");
         Assert.AreEqual(43, Parameter(url, "code_challenge").Length, "SHA-256, base64url.");

         string cookie = HeaderLine(started.Headers, "Set-Cookie");
         StringAssert.StartsWith("hmimport=", cookie);
         StringAssert.Contains("Path=/portal/oauth", cookie);
         StringAssert.Contains("HttpOnly", cookie);
         StringAssert.Contains("SameSite=Lax", cookie);

         Response microsoft = Raw("POST", "/api/v1/me/fetch-accounts/import", me, "{\"provider\":\"microsoft\",\"mirror_folders\":false}", null);
         Assert.AreEqual(200, microsoft.Status, microsoft.Body);
         StringAssert.Contains("IMAP.AccessAsUser.All", Regex.Unescape(microsoft.Body));
         StringAssert.Contains("offline_access", Regex.Unescape(microsoft.Body));
      }

      [Test]
      [Description("The callback exchanges the code with the verifier, makes an IMAP account that logs in with XOAUTH2, lists the folders for the reader, brings the ones they keep, refreshes the token and keeps a rotated refresh token; a replayed state, another browser's cookie and a change of server are refused.")]
      public void ASignInMakesAnAccountThatCollectsTheFoldersTheReaderPicks()
      {
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "import-reader@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);

         using (var provider = new FakeHttpEndpoint(TokenPort, 200, TokenAnswer("access-1", "refresh-1", 3600), "application/json"))
         {
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings",
               "{\"import_oauth_redirect_base_url\":\"https://mail.example.test\",\"import_oauth_google_client_id\":\"client-g\",\"import_oauth_google_client_secret\":\"secret-g\"," +
               "\"import_oauth_google_token_url\":\"" + provider.UrlFor("/token") + "\"}").status);

            // Signed in.
            var first = Start(me);
            Response back = Raw("GET", "/portal/oauth/callback?state=" + first.state + "&code=code-one", null, null, "hmimport=" + first.nonce);
            Assert.AreEqual(302, back.Status, back.Headers + back.Body);
            string location = HeaderLine(back.Headers, "Location");
            StringAssert.StartsWith("/portal#/accounts?imported=", location);
            StringAssert.Contains("hmimport=;", HeaderLine(back.Headers, "Set-Cookie"), "The cookie is cleared.");
            string id = location.Substring("/portal#/accounts?imported=".Length);

            string exchange = Encoding.ASCII.GetString(provider.RequestBodies.Last());
            StringAssert.Contains("grant_type=authorization_code", exchange);
            StringAssert.Contains("code=code-one", exchange);
            StringAssert.Contains("client_secret=secret-g", exchange);
            StringAssert.Contains("redirect_uri=https%3A%2F%2Fmail.example.test%2Fportal%2Foauth%2Fcallback", exchange);
            string verifier = Uri.UnescapeDataString(Regex.Match(exchange, "code_verifier=([^&]+)").Groups[1].Value);
            Assert.AreEqual(first.challenge, Base64Url(SHA256.Create().ComputeHash(Encoding.ASCII.GetBytes(verifier))),
               "The verifier sent with the code is the one the challenge was made from.");

            (int status, string body) made = Send("GET", "/api/v1/me/fetch-accounts/" + id, me, null);
            Assert.AreEqual(200, made.status, made.body);
            StringAssert.Contains("\"oauth_provider\":\"google\"", made.body);
            // The reader's grant, never this server's token; and the provider's own
            // server, not a host of the reader's choosing.
            StringAssert.Contains("\"server_token_withheld\":true", made.body);
            StringAssert.Contains("\"reader_created\":false", made.body);
            StringAssert.Contains("\"server_address\":\"imap.gmail.com\",\"port\":993,\"server_type\":\"imap\"", made.body);
            StringAssert.Contains("\"username\":\"" + SignedInAddress + "\"", made.body);
            StringAssert.Contains("\"connection_security\":\"tls\"", made.body);
            StringAssert.Contains("\"use_antispam\":true", made.body);
            StringAssert.Contains("\"mirror_folders\":true", made.body);
            StringAssert.Contains("\"mirror_pending\":true", made.body);
            StringAssert.DoesNotContain("refresh-1", made.body, "The refresh token is never emitted.");

            // The same state twice, and another browser.
            Response replay = Raw("GET", "/portal/oauth/callback?state=" + first.state + "&code=code-one", null, null, "hmimport=" + first.nonce);
            StringAssert.Contains("import_error=", HeaderLine(replay.Headers, "Location"), "A state works once.");
            var second = Start(me);
            Response stranger = Raw("GET", "/portal/oauth/callback?state=" + second.state + "&code=code-two", null, null, "hmimport=someone-elses");
            StringAssert.Contains("import_error=", HeaderLine(stranger.Headers, "Location"), "A callback without the browser's own cookie attaches nothing.");
            Assert.AreEqual(1, reader.FetchAccounts.Count);

            // The reader cannot move the grant to another server; the administrator
            // points it at the scripted one for the test.
            Assert.AreEqual(400, Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"server_address\":\"imap.elsewhere.example\"}").status);
            Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + reader.Address + "/fetch-accounts/" + id,
               "{\"server_address\":\"localhost\",\"port\":" + ImapPort + ",\"connection_security\":\"tls\"}").status);

            // A token that lives a second, and a provider that rotates the refresh token.
            provider.SetResponse(200, TokenAnswer("access-2", "refresh-2", 1));

            var inbox = new ScriptedImapServer.RemoteMailbox("INBOX");
            inbox.Add(new ScriptedImapServer.RemoteMessage(101, "From: old@gmail.example\r\nSubject: From the inbox there\r\n\r\nKept.\r\n"));
            var archive = new ScriptedImapServer.RemoteMailbox("Archive");
            archive.Add(new ScriptedImapServer.RemoteMessage(201, "From: old@gmail.example\r\nSubject: From the archive there\r\n\r\nLeft.\r\n"));
            var mailboxes = new List<ScriptedImapServer.RemoteMailbox> { inbox, archive };

            FetchAccount fetchAccount = reader.FetchAccounts.get_ItemByDBID(int.Parse(id));

            // First collection: the folders are listed for the reader, nothing is brought.
            var listing = new ScriptedImapServer(ImapPort, mailboxes) { AcceptXOAuth2 = true, ImplicitTls = true };
            Poll(listing, fetchAccount);
            Assert.AreEqual("user=" + SignedInAddress + "auth=Bearer access-2",
               Encoding.UTF8.GetString(Convert.FromBase64String(listing.XOAuth2Blob ?? "")), "The bearer the refresh gave, for the address that signed in.");
            StringAssert.Contains("grant_type=refresh_token&refresh_token=refresh-1", Encoding.ASCII.GetString(provider.RequestBodies.Last()));
            Assert.AreEqual(0, listing.FetchedUids.Count, "Nothing is brought before the reader chooses.");

            (int status, string body) listed = Send("GET", "/api/v1/me/fetch-accounts/" + id, me, null);
            StringAssert.Contains("\"mirror_seen\":[\"INBOX\",\"Archive\"]", listed.body);

            // The reader keeps the inbox and leaves the archive.
            (int status, string body) chosen = Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"mirror_skip\":[\"Archive\"]}");
            Assert.AreEqual(200, chosen.status, chosen.body);
            StringAssert.Contains("\"mirror_pending\":false", chosen.body);
            StringAssert.Contains("\"mirror_skip\":[\"Archive\"]", chosen.body);

            var collecting = new ScriptedImapServer(ImapPort, mailboxes) { AcceptXOAuth2 = true, ImplicitTls = true };
            Poll(collecting, fetchAccount);
            CollectionAssert.AreEqual(new[] { 101 }, collecting.FetchedUids, "The inbox is brought and the archive is not.");
            StringAssert.Contains("grant_type=refresh_token&refresh_token=refresh-2", Encoding.ASCII.GetString(provider.RequestBodies.Last()),
               "The rotated refresh token was kept and is the one used next.");
            StringAssert.Contains("\"collected_messages\":1", Send("GET", "/api/v1/me/fetch-accounts/" + id, me, null).body);

            // Signed in again as the same address: the same account gets the grant.
            provider.SetResponse(200, TokenAnswer("access-3", "refresh-3", 3600));
            var again = Start(me);
            Response signedInAgain = Raw("GET", "/portal/oauth/callback?state=" + again.state + "&code=code-three", null, null, "hmimport=" + again.nonce);
            Assert.AreEqual("/portal#/accounts?imported=" + id, HeaderLine(signedInAgain.Headers, "Location"));
            Assert.AreEqual(1, reader.FetchAccounts.Count, "Nothing is added for an address already here.");
         }
      }

      // ------------------------------------------------------------- helpers

      private (string state, string nonce, string challenge) Start(string authorization)
      {
         Response started = Raw("POST", "/api/v1/me/fetch-accounts/import", authorization, "{\"provider\":\"google\"}", null);
         Assert.AreEqual(200, started.Status, started.Body);
         string url = Regex.Unescape(Regex.Match(started.Body, "\"url\":\"([^\"]+)\"").Groups[1].Value);
         string cookie = HeaderLine(started.Headers, "Set-Cookie");
         string nonce = Regex.Match(cookie, "^hmimport=([^;]+)").Groups[1].Value;
         return (Parameter(url, "state"), nonce, Parameter(url, "code_challenge"));
      }

      private static void Poll(ScriptedImapServer server, FetchAccount fetchAccount)
      {
         server.StartListen();
         fetchAccount.DownloadNow();
         DateTime deadline = DateTime.Now.AddSeconds(60);
         Thread.Sleep(500);
         while (DateTime.Now < deadline && fetchAccount.IsLocked)
            Thread.Sleep(100);
         server.WaitForCompletion();
      }

      private static string TokenAnswer(string access, string refresh, int expiresIn)
      {
         string claims = "{\"sub\":\"1\",\"email\":\"" + SignedInAddress + "\"}";
         string idToken = "eyJhbGciOiJub25lIn0." + Base64Url(Encoding.UTF8.GetBytes(claims)) + ".sig";
         return "{\"access_token\":\"" + access + "\",\"token_type\":\"Bearer\",\"expires_in\":" + expiresIn +
                ",\"refresh_token\":\"" + refresh + "\",\"id_token\":\"" + idToken + "\"}";
      }

      private static string Base64Url(byte[] bytes)
      {
         return Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
      }

      private static string Parameter(string url, string name)
      {
         return Regex.Match(url, "[?&]" + name + "=([^&]*)").Groups[1].Value;
      }

      private static string HeaderLine(string headers, string name)
      {
         Match match = Regex.Match(headers, "^" + name + ":\\s*(.*)$", RegexOptions.Multiline | RegexOptions.IgnoreCase);
         return match.Success ? match.Groups[1].Value.TrimEnd('\r') : "";
      }

      private sealed class Response
      {
         public int Status;
         public string Headers = "";
         public string Body = "";
      }

      // A raw exchange, for the headers RestRequest does not return and the
      // cookie it does not send.
      private static Response Raw(string method, string path, string authorization, string body, string cookie)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);
            using (NetworkStream stream = client.GetStream())
            {
               byte[] payload = Encoding.UTF8.GetBytes(body ?? "");
               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (cookie != null)
                  head.Append("Cookie: " + cookie + "\r\n");
               if (body != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + payload.Length + "\r\n");
               }
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (body != null)
                  stream.Write(payload, 0, payload.Length);

               stream.ReadTimeout = 30000;
               var memory = new MemoryStream();
               stream.CopyTo(memory);
               string text = Encoding.UTF8.GetString(memory.ToArray());

               int split = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               var response = new Response();
               string headers = split >= 0 ? text.Substring(0, split) : text;
               response.Status = int.Parse(Regex.Match(headers, "^HTTP/1\\.[01] (\\d{3})").Groups[1].Value);
               response.Headers = headers;
               response.Body = split >= 0 ? text.Substring(split + 4) : "";
               return response;
            }
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
