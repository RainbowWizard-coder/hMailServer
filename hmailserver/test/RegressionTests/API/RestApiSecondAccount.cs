// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A second account in the same webmail page (Roadmap2 row L50): the
   ///    bearer form of the session route. POST /api/v1/session?bearer=1 answers
   ///    an account's session token in the body and sets no cookie, and the
   ///    token then authenticates as "Authorization: Bearer" exactly as the
   ///    cookie would - the same routes, the same refusals, the same header a
   ///    write must carry - and in no other place.
   ///
   ///    What is pinned here is the separation the row lives on: a page that
   ///    holds one account's cookie and another's token reaches each account
   ///    with its own credential and never the other's mailbox with either; the
   ///    token is never in a Set-Cookie and ending it never touches the cookie;
   ///    the administrator is never given one, and an account's role does not
   ///    travel with one - a bearer is the mailbox and nothing more; and the
   ///    account the cookie already stands for is refused the form.
   /// </summary>
   [TestFixture]
   public class RestApiSecondAccount : TestFixtureBase
   {
      // From the 9666-9670 range reserved for this row; the suite's own port
      // where RestListener finds one already on.
      private static int RestPort = 9666;

      private const string AdminPassword = "testar";
      private const string FirstPassword = "First-Passw0rd!";
      private const string SecondPassword = "Second-Passw0rd!";

      private Account _first;
      private Account _second;

      private string FirstAddress
      {
         get { return "first@" + _domain.Name; }
      }

      private string SecondAddress
      {
         get { return "second@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         _first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, FirstAddress, FirstPassword);
         _second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, SecondAddress, SecondPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");
         _application.Reinitialize();

         Response probe = Raw("GET", "/api/v1/status", AdminHeader(), null);
         Assert.AreEqual(200, probe.Status, "REST API did not answer /api/v1/status. Body: " + probe.Body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("POST /api/v1/session?bearer=1 answers the second account's token in the body and no Set-Cookie; the token then answers /api/v1/me as that account, beside the first account's cookie, and neither credential reads the other's mail")]
      public void ABearerSessionIsTheSecondAccountsAndNotTheFirsts()
      {
         string cookie = SignInWithCookie(FirstAddress, FirstPassword);

         Response started = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(SecondAddress, SecondPassword), null);
         Assert.AreEqual(201, started.Status, started.Body);
         StringAssert.Contains("\"address\":\"" + SecondAddress + "\"", started.Body);
         StringAssert.Contains("\"bearer\":true", started.Body);
         StringAssert.Contains("\"idle_seconds\":", started.Body);
         StringAssert.Contains("\"lifetime_seconds\":", started.Body);
         Assert.AreEqual("", started.Header("Set-Cookie"), "The bearer form sets no cookie: the browser's cookie is the first account's. Headers: " + string.Join(" | ", started.Headers.Keys));

         string token = TokenOf(started.Body);
         Assert.AreEqual(64, token.Length, "A 32-byte token in hex: " + started.Body);
         Assert.IsFalse(started.Body.Contains("Set-Cookie"), started.Body);

         // Each credential is its own account.
         Response asSecond = Raw("GET", "/api/v1/me", "Bearer " + token, null);
         Assert.AreEqual(200, asSecond.Status, asSecond.Body);
         StringAssert.Contains("\"address\":\"" + SecondAddress + "\"", asSecond.Body);
         Assert.IsFalse(asSecond.Body.Contains(FirstAddress), "The bearer answers as the second account only: " + asSecond.Body);

         Response asFirst = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, asFirst.Status, asFirst.Body);
         StringAssert.Contains("\"address\":\"" + FirstAddress + "\"", asFirst.Body);

         // Both at once, as the page sends them: the header decides, and it
         // decides for the second account.
         Response both = Raw("GET", "/api/v1/me", "Bearer " + token, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, both.Status, both.Body);
         StringAssert.Contains("\"address\":\"" + SecondAddress + "\"", both.Body);

         // A message of the first account is not the second's to read, by
         // either route that names it, however the second is authenticated.
         Deliver(FirstAddress, "Only the first's", "A line for the first account.");
         Pop3ClientSimulator.AssertMessageCount(FirstAddress, FirstPassword, 1);

         Response tree = Raw("GET", "/api/v1/me/folders", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, tree.Status, tree.Body);
         long firstInbox = IdBefore(tree.Body, "\"path\":\"INBOX\"");
         Response page = Raw("GET", "/api/v1/me/folders/" + firstInbox + "/messages", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, page.Status, page.Body);
         long messageId = IdBefore(page.Body, "\"subject\":\"Only the first's\"");

         Response theirsMessage = Raw("GET", "/api/v1/me/messages/" + messageId, "Bearer " + token, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(404, theirsMessage.Status, "The second account must not read the first's message: " + theirsMessage.Body);
         Response theirsFolder = Raw("GET", "/api/v1/me/folders/" + firstInbox + "/messages", "Bearer " + token, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(404, theirsFolder.Status, "The second account must not list the first's folder: " + theirsFolder.Body);
         Response theirsHtml = Raw("GET", "/api/v1/me/messages/" + messageId + "/html", "Bearer " + token, null);
         Assert.AreEqual(404, theirsHtml.Status, theirsHtml.Body);

         Response secondTree = Raw("GET", "/api/v1/me/folders", "Bearer " + token, null);
         Assert.AreEqual(200, secondTree.Status, secondTree.Body);
         long secondInbox = IdBefore(secondTree.Body, "\"path\":\"INBOX\"");
         Assert.AreNotEqual(firstInbox, secondInbox, "Two mailboxes, two inboxes.");
         Response secondPage = Raw("GET", "/api/v1/me/folders/" + secondInbox + "/messages", "Bearer " + token, null);
         Assert.AreEqual(200, secondPage.Status, secondPage.Body);
         Assert.IsFalse(secondPage.Body.Contains("Only the first's"), "The first's mail is not in the second's listing: " + secondPage.Body);

         // The account's own session list names it as what it is.
         Response sessions = Raw("GET", "/api/v1/me/sessions", "Bearer " + token, null);
         Assert.AreEqual(200, sessions.Status, sessions.Body);
         StringAssert.Contains("\"current\":true", sessions.Body);
         StringAssert.Contains("\"bearer\":true", sessions.Body);
         Response firstSessions = Raw("GET", "/api/v1/me/sessions", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, firstSessions.Status, firstSessions.Body);
         StringAssert.Contains("\"bearer\":false", firstSessions.Body);
         Assert.IsFalse(firstSessions.Body.Contains("\"bearer\":true"), "The second's session is not among the first's: " + firstSessions.Body);

         // The bearer is an account's session and nothing more: the
         // administration API refuses it as it refuses the cookie.
         Response admin = Raw("GET", "/api/v1/status", "Bearer " + token, null);
         Assert.AreEqual(403, admin.Status, admin.Body);
         Response another = Raw("POST", "/api/v1/session", "Bearer " + token, null, "X-Requested-With: hMailServer\r\n");
         Assert.AreEqual(403, another.Status, "A session cannot start another session: " + another.Body);
      }

      [Test]
      [Description("A change made on a bearer session must carry X-Requested-With as one on the cookie must, and lands on the second account and not the first")]
      public void AChangeOnABearerNeedsTheHeaderAndLandsOnTheSecondAccount()
      {
         string cookie = SignInWithCookie(FirstAddress, FirstPassword);
         string token = SignInWithBearer(SecondAddress, SecondPassword);

         Response bare = Raw("PUT", "/api/v1/me/vacation", "Bearer " + token,
            "{\"enabled\":true,\"subject\":\"Away\",\"message\":\"Back Monday.\"}",
            "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(403, bare.Status, bare.Body);
         StringAssert.Contains("X-Requested-With", bare.Body);
         Assert.IsFalse(_domain.Accounts.ItemByAddress[SecondAddress].VacationMessageIsOn, "The refused request must have changed nothing.");

         Response withHeader = Raw("PUT", "/api/v1/me/vacation", "Bearer " + token,
            "{\"enabled\":true,\"subject\":\"Away\",\"message\":\"Back Monday.\"}",
            "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(200, withHeader.Status, withHeader.Body);

         Assert.IsTrue(_domain.Accounts.ItemByAddress[SecondAddress].VacationMessageIsOn, "COM must see the second account's automatic reply switched on.");
         Assert.IsFalse(_domain.Accounts.ItemByAddress[FirstAddress].VacationMessageIsOn, "The first account, whose cookie travelled with the request, must be untouched.");

         Response reset = Raw("PUT", "/api/v1/me/vacation", BasicHeader(SecondAddress, SecondPassword),
            "{\"enabled\":false,\"subject\":\"\",\"message\":\"\"}");
         Assert.AreEqual(200, reset.Status, reset.Body);
      }

      [Test]
      [Description("DELETE /api/v1/session with the bearer ends that session without a Set-Cookie, and the first account's cookie goes on working; an ended, a garbage and a wrong-place token are refused")]
      public void DeleteEndsTheBearerSessionAndLeavesTheCookieAlone()
      {
         string cookie = SignInWithCookie(FirstAddress, FirstPassword);
         string token = SignInWithBearer(SecondAddress, SecondPassword);

         // The right token in the wrong place: a cookie's value as a bearer,
         // and a bearer's value as a cookie. Neither is how its session was
         // made, and neither is accepted.
         Response cookieAsBearer = Raw("GET", "/api/v1/me", "Bearer " + cookie, null);
         Assert.AreEqual(401, cookieAsBearer.Status, "A cookie's value is not a bearer: " + cookieAsBearer.Body);
         Response bearerAsCookie = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + token + "\r\n");
         Assert.AreEqual(401, bearerAsCookie.Status, "A bearer's value is not a cookie: " + bearerAsCookie.Body);

         // Refused in the wrong place, the sessions still stand in the right one.
         Assert.AreEqual(200, Raw("GET", "/api/v1/me", "Bearer " + token, null).Status);
         Assert.AreEqual(200, Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n").Status);

         Response noHeader = Raw("DELETE", "/api/v1/session", "Bearer " + token, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(403, noHeader.Status, "Ending a session is a change, and a change carries the header: " + noHeader.Body);

         Response ended = Raw("DELETE", "/api/v1/session", "Bearer " + token, null,
            "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(200, ended.Status, ended.Body);
         StringAssert.Contains("\"ended\":true", ended.Body);
         StringAssert.Contains("\"bearer\":true", ended.Body);
         Assert.AreEqual("", ended.Header("Set-Cookie"), "Ending the bearer session must not clear the first account's cookie.");

         Response afterwards = Raw("GET", "/api/v1/me", "Bearer " + token, null);
         Assert.AreEqual(401, afterwards.Status, afterwards.Body);

         Response firstStays = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, firstStays.Status, "The first account's session is not what was ended: " + firstStays.Body);
         StringAssert.Contains("\"address\":\"" + FirstAddress + "\"", firstStays.Body);

         Response garbage = Raw("GET", "/api/v1/me", "Bearer " + new string('a', 64), null);
         Assert.AreEqual(401, garbage.Status, garbage.Body);
         Response shortOne = Raw("GET", "/api/v1/me", "Bearer deadbeef", null);
         Assert.AreEqual(401, shortOne.Status, shortOne.Body);
      }

      [Test]
      [Description("The token is never in a Set-Cookie, in the sign-in's answer, in the delete's answer, or in the first account's own sign-out; and a password change of the second account ends its bearer session")]
      public void TheTokenIsNeverACookieAndAPasswordChangeEndsIt()
      {
         string cookie = SignInWithCookie(FirstAddress, FirstPassword);

         Response started = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(SecondAddress, SecondPassword), null,
            "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(201, started.Status, started.Body);
         string token = TokenOf(started.Body);
         foreach (KeyValuePair<string, string> header in started.Headers)
            Assert.IsFalse(header.Value.Contains(token), "The token was in a header: " + header.Key);
         Assert.IsFalse(started.Headers.ContainsKey("Set-Cookie"), "No Set-Cookie at all on the bearer form.");

         // The first account signing out clears its cookie and says nothing of
         // the token; the bearer session is not the cookie's to end.
         Response signedOut = Raw("DELETE", "/api/v1/session", null, null,
            "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(200, signedOut.Status, signedOut.Body);
         StringAssert.Contains("Max-Age=0", signedOut.Header("Set-Cookie"));
         Assert.IsFalse(signedOut.Header("Set-Cookie").Contains(token));
         Assert.AreEqual(200, Raw("GET", "/api/v1/me", "Bearer " + token, null).Status, "The bearer session stands until it is ended itself.");

         // A new password ends the account's sessions, the bearer one included.
         const string rotated = "Rotated-Passw0rd!";
         Response changed = Raw("POST", "/api/v1/me/password", BasicHeader(SecondAddress, SecondPassword),
            "{\"current\":\"" + SecondPassword + "\",\"new\":\"" + rotated + "\"}");
         Assert.AreEqual(200, changed.Status, changed.Body);
         Response gone = Raw("GET", "/api/v1/me", "Bearer " + token, null);
         Assert.AreEqual(401, gone.Status, "The password change must have ended the bearer session: " + gone.Body);

         Response back = Raw("POST", "/api/v1/me/password", BasicHeader(SecondAddress, rotated),
            "{\"current\":\"" + rotated + "\",\"new\":\"" + SecondPassword + "\"}");
         Assert.AreEqual(200, back.Status, back.Body);
      }

      [Test]
      [Description("The bearer form is never minted for the administrator, and a wrong password or an API key does not get one either")]
      public void TheAdministratorIsRefusedTheBearerForm()
      {
         Response admin = Raw("POST", "/api/v1/session?bearer=1", AdminHeader(), null);
         Assert.AreEqual(403, admin.Status, admin.Body);
         StringAssert.Contains("administrator", admin.Body);
         Assert.AreEqual("", admin.Header("Set-Cookie"), "A refusal sets nothing.");
         Assert.IsFalse(admin.Body.Contains("\"token\""), admin.Body);

         Response wrong = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(SecondAddress, "not-the-password"), null);
         Assert.AreEqual(401, wrong.Status, wrong.Body);
         Assert.IsFalse(wrong.Body.Contains("\"token\""), wrong.Body);

         // The administrator's own cookie session is still the cookie, and
         // the plain form still answers the account a cookie.
         Response plain = Raw("POST", "/api/v1/session", BasicHeader(SecondAddress, SecondPassword), null);
         Assert.AreEqual(201, plain.Status, plain.Body);
         StringAssert.StartsWith("hmailsession=", plain.Header("Set-Cookie"));
         Assert.IsFalse(plain.Body.Contains("\"token\""), "The plain form never puts the token in the body: " + plain.Body);
      }

      [Test]
      [Description("An account that holds an administrative role reaches the administration API on its password and on its cookie, and never on a bearer: a bearer session is the mailbox and nothing more, and says so to GET /api/v1/session")]
      public void ARoleOnTheAccountDoesNotTravelWithTheBearer()
      {
         _second.AdminLevel = eAdminLevel.hAdminLevelServerAdmin;
         _second.Save();

         // The password and the cookie carry the role, as they always have:
         // this is the server administrator's own mailbox, and it administers.
         Response onPassword = Raw("GET", "/api/v1/status", BasicHeader(SecondAddress, SecondPassword), null);
         Assert.AreEqual(200, onPassword.Status, onPassword.Body);

         string cookie = SignInWithCookie(SecondAddress, SecondPassword);
         Response onCookie = Raw("GET", "/api/v1/status", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, onCookie.Status, onCookie.Body);
         Response whoOnCookie = Raw("GET", "/api/v1/session", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, whoOnCookie.Status, whoOnCookie.Body);
         StringAssert.Contains("\"administrative\":true", whoOnCookie.Body);
         StringAssert.Contains("\"role\":\"serveradmin\"", whoOnCookie.Body);

         // The bearer does not. A token a page holds in script is a credential
         // a cross-site script could read, which is why the administrator
         // password is refused the form; the same reach by way of a role on
         // the account would be the same exposure, so the role goes with the
         // form, as it goes for an app password and for a support session.
         string token = SignInWithBearer(SecondAddress, SecondPassword);
         Response onBearer = Raw("GET", "/api/v1/status", "Bearer " + token, null);
         Assert.AreEqual(403, onBearer.Status, "A server administrator's bearer must not reach the administration API: " + onBearer.Body);
         StringAssert.Contains("an account's credentials reach only the account's own endpoints", onBearer.Body);
         Response settings = Raw("GET", "/api/v1/settings", "Bearer " + token, null);
         Assert.AreEqual(403, settings.Status, settings.Body);
         Response domains = Raw("GET", "/api/v1/domains", "Bearer " + token, null);
         Assert.AreEqual(403, domains.Status, domains.Body);
         Response assignments = Raw("GET", "/api/v1/role-assignments", "Bearer " + token, null);
         Assert.AreEqual(403, assignments.Status, assignments.Body);

         Response whoOnBearer = Raw("GET", "/api/v1/session", "Bearer " + token, null);
         Assert.AreEqual(200, whoOnBearer.Status, whoOnBearer.Body);
         StringAssert.Contains("\"credential\":\"account\"", whoOnBearer.Body);
         StringAssert.Contains("\"administrative\":false", whoOnBearer.Body);
         StringAssert.Contains("\"permissions\":[]", whoOnBearer.Body);
         StringAssert.Contains("\"domains\":[]", whoOnBearer.Body);

         // The mailbox itself is what the form is for, and it is reached.
         Response me = Raw("GET", "/api/v1/me", "Bearer " + token, null);
         Assert.AreEqual(200, me.Status, me.Body);
         StringAssert.Contains("\"address\":\"" + SecondAddress + "\"", me.Body);
         Response folders = Raw("GET", "/api/v1/me/folders", "Bearer " + token, null);
         Assert.AreEqual(200, folders.Status, folders.Body);
      }

      [Test]
      [Description("The account the request's own cookie already stands for is refused the bearer form with 409, under its own spelling and under an alias domain's, and the cookie's session stands; another account beside it, or the same account with no cookie, is minted one")]
      public void TheAccountTheCookieStandsForIsRefusedTheBearerForm()
      {
         string cookie = SignInWithCookie(FirstAddress, FirstPassword);

         // The page refuses the address it knows before asking. The server
         // refuses the account itself, whatever it was spelled as, because a
         // second sign-in of the cookie's account from the same device would
         // end the cookie's session under the one-per-device policy - and
         // the page would sign itself out for adding the account it was in.
         Response same = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(FirstAddress, FirstPassword), null,
            "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(409, same.Status, same.Body);
         StringAssert.Contains("already signed in as", same.Body);
         Assert.IsFalse(same.Body.Contains("\"token\""), same.Body);
         Assert.AreEqual("", same.Header("Set-Cookie"), "A refusal sets nothing.");

         // The spelling the page cannot know to be the same account: its
         // address at an alias of its domain, which the sign-in resolves to
         // the one account as IMAP and POP3 do.
         DomainAlias alias = _domain.DomainAliases.Add();
         alias.DomainID = _domain.ID;
         alias.AliasName = "second-alias.test";
         alias.Save();

         Response aliased = Raw("POST", "/api/v1/session?bearer=1", BasicHeader("first@second-alias.test", FirstPassword), null,
            "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(409, aliased.Status, "The alias spelling is the cookie's own account: " + aliased.Body);
         Assert.IsFalse(aliased.Body.Contains("\"token\""), aliased.Body);

         // Neither refusal touched the cookie's session.
         Response stillFirst = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, stillFirst.Status, stillFirst.Body);
         StringAssert.Contains("\"address\":\"" + FirstAddress + "\"", stillFirst.Body);

         // Another account beside it is what the form is for.
         Response other = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(SecondAddress, SecondPassword), null,
            "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(201, other.Status, other.Body);
         StringAssert.Contains("\"address\":\"" + SecondAddress + "\"", other.Body);

         // A cookie that no longer stands for anybody is not this case, and
         // neither is no cookie at all: there is no browser already signed in
         // as the account then.
         Response signedOut = Raw("DELETE", "/api/v1/session", null, null,
            "Cookie: hmailsession=" + cookie + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(200, signedOut.Status, signedOut.Body);
         Response afterwards = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(FirstAddress, FirstPassword), null,
            "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(201, afterwards.Status, afterwards.Body);
         Response noCookie = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(FirstAddress, FirstPassword), null);
         Assert.AreEqual(201, noCookie.Status, noCookie.Body);
      }

      // ------------------------------------------------------------ helpers ---

      private string SignInWithCookie(string address, string password)
      {
         Response started = Raw("POST", "/api/v1/session", BasicHeader(address, password), null);
         Assert.AreEqual(201, started.Status, started.Body);
         string setCookie = started.Header("Set-Cookie");
         StringAssert.StartsWith("hmailsession=", setCookie);
         int start = setCookie.IndexOf('=') + 1;
         int end = setCookie.IndexOf(';', start);
         return end > start ? setCookie.Substring(start, end - start) : setCookie.Substring(start);
      }

      private string SignInWithBearer(string address, string password)
      {
         Response started = Raw("POST", "/api/v1/session?bearer=1", BasicHeader(address, password), null);
         Assert.AreEqual(201, started.Status, started.Body);
         Assert.AreEqual("", started.Header("Set-Cookie"), "The bearer form sets no cookie.");
         return TokenOf(started.Body);
      }

      private static string TokenOf(string body)
      {
         const string marker = "\"token\":\"";
         int at = body.IndexOf(marker, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "No token in: " + body);
         int start = at + marker.Length;
         int end = body.IndexOf('"', start);
         return body.Substring(start, end - start);
      }

      private static void Deliver(string to, string subject, string body)
      {
         SmtpClientSimulator.StaticSend("sender@example.com", to, subject, body);
      }

      private static long NumberAt(string body, int start)
      {
         int end = start;
         while (end < body.Length && char.IsDigit(body[end]))
            end++;
         return long.Parse(body.Substring(start, end - start));
      }

      // The id of the object a marker sits in: the last "id" before it.
      private static long IdBefore(string body, string marker)
      {
         int at = body.IndexOf(marker, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "Missing " + marker + " in: " + body);
         int idAt = body.LastIndexOf("\"id\":", at, StringComparison.Ordinal);
         Assert.IsTrue(idAt >= 0, "No id before " + marker + " in: " + body);
         return NumberAt(body, idAt + 5);
      }

      private static string AdminHeader()
      {
         return BasicHeader("Administrator", AdminPassword);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public readonly Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : "";
         }
      }

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

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               var response = new Response();

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string head = separator >= 0 ? raw.Substring(0, separator) : raw;
               response.Body = separator >= 0 ? raw.Substring(separator + 4) : "";

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
   }
}
