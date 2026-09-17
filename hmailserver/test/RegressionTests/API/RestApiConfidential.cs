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
   ///    A message that expires (ConfidentialMessages.h): sent with expires_in, the
   ///    message is kept on the server and the recipient gets a notice with a link and
   ///    not the text; the sender's Sent copy is the full message; the page refuses
   ///    a stranger, sends a one-time code to a recipient address, takes the passcode
   ///    the sender set, shows the text only once proved, records the first opening for
   ///    the sender, refuses after a revoke or the expiry, and the sweep deletes the
   ///    kept file. Without a base URL nothing can be sent with an expiry.
   /// </summary>
   [TestFixture]
   public class RestApiConfidential : TestFixtureBase
   {
      private static int RestPort = 9621;
      private const string AdminPassword = "testar";
      private const string Password = "Expires-Passw0rd!";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"http://127.0.0.1:" + RestPort + "\"}").status);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\"}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The recipient gets a notice with a link and not the text, the sender keeps the full message in Sent, the page asks a stranger for proof, a code sent to the recipient's address opens it, the sender sees when and by whom, and a revoke ends it and deletes the kept file.")]
      public void AMessageThatExpiresIsReadThroughTheLinkAndNotSent()
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "expires-sender@" + _domain.Name, Password);
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "expires-reader@" + _domain.Name, Password);
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "expires-other@" + _domain.Name, Password);
         string me = Basic(sender.Address, Password);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(sender.Address, Password));
         Assert.IsTrue(imap.CreateFolder("Sent"));
         imap.Disconnect();

         (int status, string body) sent = Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"The board pack\",\"text\":\"The figures are 42.\",\"expires_in\":3600}");
         Assert.AreEqual(201, sent.status, sent.body);
         StringAssert.Contains("\"confidential_id\":", sent.body);
         long sentId = long.Parse(Regex.Match(sent.body, "\"sent_id\":(\\d+)").Groups[1].Value);
         Assert.Greater(sentId, 0, "A copy is kept in Sent: " + sent.body);
         string id = Regex.Match(sent.body, "\"confidential_id\":(\\d+)").Groups[1].Value;

         // The notice, not the message, reaches the recipient. (Soft line breaks,
         // should the text part go quoted-printable, are joined before the link
         // is looked for: it is longer than a line.)
         string notice = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password).Replace("=\r\n", "");
         StringAssert.Contains("Subject: The board pack", notice);
         StringAssert.DoesNotContain("The figures are 42.", notice, "The text itself is not sent.");
         Match link = Regex.Match(notice, "http://127\\.0\\.0\\.1:" + RestPort + "/confidential/([0-9a-f]{64})");
         Assert.IsTrue(link.Success, "The notice carries the link: " + notice);
         string token = link.Groups[1].Value;

         // The sender's copy is the full message.
         imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(sender.Address, Password));
         Assert.AreEqual(1, imap.GetMessageCount("Sent"));
         Assert.IsTrue(imap.SelectFolder("Sent"));
         StringAssert.Contains("The figures are 42.", imap.Fetch("1 BODY.PEEK[TEXT]"), "The Sent copy is the message, not the notice.");
         imap.Disconnect();

         string kept = Path.Combine(_settings.Directories.DataDirectory, "Confidential", sender.ID.ToString(), token + ".eml");
         Assert.IsTrue(File.Exists(kept), "The message is kept at " + kept);

         // The page, to anyone with the link: proof first, and nothing of the text.
         Response page = Page("GET", token, null, null);
         Assert.AreEqual(200, page.Status, page.Body);
         StringAssert.Contains("Send me a code", page.Body);
         StringAssert.DoesNotContain("The figures are 42.", page.Body);
         StringAssert.Contains("Cache-Control: no-store", page.Headers);
         StringAssert.Contains("X-Frame-Options: DENY", page.Headers);
         StringAssert.Contains("Content-Security-Policy: default-src 'none'", page.Headers);
         StringAssert.Contains("Referrer-Policy: no-referrer", page.Headers);
         Assert.AreEqual(404, Page("GET", new string('0', 64), null, null).Status, "No such message.");

         // A code asked for an address the message did not go to is answered the
         // same way - the same status and the same page, on the first ask, on a
         // second inside a minute, and on a wrong code - so nothing the page
         // says tells a stranger who the recipients are.
         Response strangerAsked = Page("POST", token, null, "step=code&address=" + other.Address);
         Response readerAsked = Page("POST", token, null, "step=code&address=" + reader.Address);
         Assert.AreEqual(200, strangerAsked.Status, strangerAsked.Body);
         Assert.AreEqual(200, readerAsked.Status, readerAsked.Body);
         Assert.AreEqual(Neutral(strangerAsked.Body, other.Address), Neutral(readerAsked.Body, reader.Address), "The same page for a stranger's address and a recipient's.");
         Response strangerAgain = Page("POST", token, null, "step=code&address=" + other.Address);
         Response readerAgain = Page("POST", token, null, "step=code&address=" + reader.Address);
         Assert.AreEqual(429, strangerAgain.Status, strangerAgain.Body);
         Assert.AreEqual(429, readerAgain.Status, readerAgain.Body);
         Assert.AreEqual(Neutral(strangerAgain.Body, other.Address), Neutral(readerAgain.Body, reader.Address), "The once-a-minute limit answers a stranger's address the same way.");

         string codeMail = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         StringAssert.Contains("Subject: Your code to read a confidential message", codeMail);
         string code = Regex.Match(codeMail, "The code is: (\\d{6})").Groups[1].Value;
         Assert.AreEqual(6, code.Length, codeMail);
         // No code is mailed to an address the message did not go to.
         Pop3ClientSimulator.AssertMessageCount(other.Address, Password, 0);

         string wrong = code == "000000" ? "111111" : "000000";
         Response strangerWrong = Page("POST", token, null, "step=verify&address=" + other.Address + "&code=" + wrong);
         Response refused = Page("POST", token, null, "step=verify&address=" + reader.Address + "&code=" + wrong);
         Assert.AreEqual(403, refused.Status, refused.Body);
         StringAssert.Contains("not right", refused.Body);
         Assert.AreEqual(403, strangerWrong.Status, strangerWrong.Body);
         Assert.AreEqual(Neutral(strangerWrong.Body, other.Address), Neutral(refused.Body, reader.Address), "A wrong code for a stranger's address is answered the same way.");

         Response proved = Page("POST", token, null, "step=verify&address=" + reader.Address + "&code=" + code);
         Assert.AreEqual(303, proved.Status, proved.Body);
         Match grant = Regex.Match(proved.Headers, "Set-Cookie: hmconfidential=([0-9a-f]{64}); Path=/confidential/" + token + "; HttpOnly");
         Assert.IsTrue(grant.Success, "A grant cookie scoped to the message: " + proved.Headers);
         string cookie = "hmconfidential=" + grant.Groups[1].Value;

         Response shown = Page("GET", token, cookie, null);
         Assert.AreEqual(200, shown.Status, shown.Body);
         StringAssert.Contains("The figures are 42.", shown.Body);
         StringAssert.Contains("The board pack", shown.Body);
         StringAssert.Contains("Cache-Control: no-store", shown.Headers);

         (int status, string body) listed = Send("GET", "/api/v1/me/confidential", me, null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"available\":true", listed.body);
         StringAssert.Contains("\"subject\":\"The board pack\"", listed.body);
         StringAssert.Contains("\"opened_by\":\"" + reader.Address.ToLowerInvariant() + "\"", listed.body, "The sender sees who opened it.");
         StringAssert.Contains("\"revoked\":false", listed.body);
         StringAssert.Contains("/confidential/" + token, listed.body);
         Assert.IsTrue(Regex.IsMatch(listed.body, "\"opened\":[1-9]\\d*"), "And when: " + listed.body);

         Assert.AreEqual(404, Send("POST", "/api/v1/me/confidential/" + id + "/revoke", Basic(other.Address, Password), null).status, "Another account's message is not found.");
         Assert.AreEqual(200, Send("POST", "/api/v1/me/confidential/" + id + "/revoke", me, null).status);
         Assert.AreEqual(410, Page("GET", token, cookie, null).Status, "Withdrawn: the page refuses, grant or no grant.");
         StringAssert.Contains("\"revoked\":true", Send("GET", "/api/v1/me/confidential", me, null).body);
         Assert.IsFalse(File.Exists(kept), "The kept copy is deleted on revoke.");
      }

      [Test]
      [Description("A passcode the sender set opens the page and is recorded as the opener; a message whose expiry has passed is refused and the retention sweep deletes its file; a signed or encrypted entity, an expiry out of range, and a server with no base URL are refused.")]
      public void APasscodeOpensItAndAnExpiryEndsIt()
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "expires-passcode@" + _domain.Name, Password);
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "expires-passcode-reader@" + _domain.Name, Password);
         string me = Basic(sender.Address, Password);

         (int status, string body) sent = Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"With a passcode\",\"text\":\"Orange is the word.\",\"expires_in\":3600,\"passcode\":\"orange-tree\"}");
         Assert.AreEqual(201, sent.status, sent.body);
         string notice = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password).Replace("=\r\n", "");
         StringAssert.Contains("passcode", notice, "The notice says a passcode will do.");
         string token = Regex.Match(notice, "/confidential/([0-9a-f]{64})").Groups[1].Value;

         Response page = Page("GET", token, null, null);
         Assert.AreEqual(200, page.Status, page.Body);
         StringAssert.Contains("name=\"passcode\"", page.Body);
         Assert.AreEqual(403, Page("POST", token, null, "step=passcode&passcode=apple").Status);
         Response proved = Page("POST", token, null, "step=passcode&passcode=orange-tree");
         Assert.AreEqual(303, proved.Status, proved.Body);
         string cookie = "hmconfidential=" + Regex.Match(proved.Headers, "hmconfidential=([0-9a-f]{64})").Groups[1].Value;
         Response shown = Page("GET", token, cookie, null);
         Assert.AreEqual(200, shown.Status, shown.Body);
         StringAssert.Contains("Orange is the word.", shown.Body);
         (int status, string body) listed = Send("GET", "/api/v1/me/confidential", me, null);
         StringAssert.Contains("\"opened_by\":\"passcode\"", listed.body, listed.body);
         StringAssert.Contains("\"passcode\":true", listed.body);

         // An expiry that passes: refused on the page, listed as expired, and the
         // sweep - the retention task runs it on demand - deletes the kept file.
         // The token comes from the list, which is immediate, so the kept file is
         // looked for well inside the expiry rather than after a POP3 poll that
         // could straddle it and the minute sweep.
         DateTime briefSentAt = DateTime.UtcNow;
         (int status, string body) brief = Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"Gone in seconds\",\"text\":\"Blink.\",\"expires_in\":3}");
         Assert.AreEqual(201, brief.status, brief.body);
         (int status, string body) newest = Send("GET", "/api/v1/me/confidential", me, null);
         string briefToken = Regex.Match(newest.body, "\"link\":\"[^\"]*/confidential/([0-9a-f]{64})\"").Groups[1].Value;
         Assert.AreEqual(64, briefToken.Length, "The newest message is listed first: " + newest.body);
         string kept = Path.Combine(_settings.Directories.DataDirectory, "Confidential", sender.ID.ToString(), briefToken + ".eml");
         Assert.IsTrue(File.Exists(kept), "Kept until it expires: " + kept);
         string briefNotice = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password).Replace("=\r\n", "");
         StringAssert.Contains("/confidential/" + briefToken, briefNotice);
         int remaining = 3500 - (int) (DateTime.UtcNow - briefSentAt).TotalMilliseconds;
         if (remaining > 0)
            Thread.Sleep(remaining);
         Response expired = Page("GET", briefToken, null, null);
         Assert.AreEqual(410, expired.Status, expired.Body);
         StringAssert.Contains("expired", expired.Body);
         Assert.IsTrue(Regex.IsMatch(Send("GET", "/api/v1/me/confidential", me, null).body, "\"expires\":\\d+,\"expired\":true"), "Listed as expired.");
         _application.Utilities.RunMessageRetention();
         Assert.IsFalse(File.Exists(kept), "The sweep deletes the kept file.");
         Assert.IsTrue(File.Exists(Path.Combine(_settings.Directories.DataDirectory, "Confidential", sender.ID.ToString(), token + ".eml")), "And leaves the one still readable.");

         // What is refused before anything is written - including an expires_in
         // that is not a whole number of seconds, which must never be read as
         // "no expiry" and sent in full.
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"Too long\",\"text\":\"x\",\"expires_in\":7776001}").status, "More than ninety days.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"Negative\",\"text\":\"x\",\"expires_in\":-3600}").status, "A negative expiry.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"Words\",\"text\":\"x\",\"expires_in\":\"1h\"}").status, "An expiry that is not a number.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"True\",\"text\":\"x\",\"expires_in\":true}").status, "An expiry that is a boolean.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"Passcode alone\",\"text\":\"x\",\"passcode\":\"orange-tree\"}").status, "A passcode without an expiry.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"Short passcode\",\"text\":\"x\",\"expires_in\":60,\"passcode\":\"ab\"}").status, "A passcode under four characters.");
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"Signed\",\"mime\":\"Content-Type: text/plain\\r\\n\\r\\nx\",\"expires_in\":60}").status, "A signed or encrypted entity cannot expire.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\",\"quarantine_digest_base_url\":\"\",\"import_oauth_redirect_base_url\":\"\"}").status);
         StringAssert.Contains("\"available\":false", Send("GET", "/api/v1/me/confidential", me, null).body, "With no base URL it is not offered.");
         (int status, string body) unavailable = Send("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + reader.Address + "\",\"subject\":\"No address\",\"text\":\"x\",\"expires_in\":60}");
         Assert.AreEqual(403, unavailable.status, unavailable.body);
         StringAssert.Contains("confidential_unavailable", unavailable.body);
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 0);
      }

      private struct Response
      {
         public int Status;
         public string Headers;
         public string Body;
      }

      // The reading page takes no credentials: a form post and a cookie are all a
      // browser brings, so the request is written by hand and the headers kept.
      private static Response Page(string method, string token, string cookie, string form)
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
               headers.Append(method + " /confidential/" + token + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (cookie != null)
                  headers.Append("Cookie: " + cookie + "\r\n");
               byte[] bodyBytes = form == null ? new byte[0] : Encoding.UTF8.GetBytes(form);
               if (form != null)
               {
                  headers.Append("Content-Type: application/x-www-form-urlencoded\r\n");
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
               var response = new Response();
               string[] parts = raw.Split(' ');
               if (parts.Length >= 2)
                  int.TryParse(parts[1], out response.Status);
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               response.Headers = separator >= 0 ? raw.Substring(0, separator) : raw;
               response.Body = separator >= 0 ? raw.Substring(separator + 4) : "";
               return response;
            }
         }
      }

      // A page with the address it was asked about taken out, so that the page
      // shown for a stranger's address can be compared with a recipient's.
      private static string Neutral(string body, string address)
      {
         return body.Replace(address, "ADDRESS").Replace(address.ToLowerInvariant(), "ADDRESS");
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
