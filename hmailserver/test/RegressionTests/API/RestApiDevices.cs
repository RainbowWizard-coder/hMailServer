// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
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
   ///    The clients that have signed in to an account, and the session policies.
   ///
   ///    A client is what SignInStore records: the protocol, the address and the
   ///    user agent where the protocol carries one, with the credential it used.
   ///    The account holder reads its own list at GET /api/v1/me/devices and an
   ///    administrator reads the same rows under the address; either revokes one
   ///    with a DELETE, which ends the browser session the client holds and drops
   ///    its live IMAP, POP3 and SMTP connections, and with credential=revoke
   ///    turns off the app password behind it.
   ///
   ///    The four policies - idle timeout, absolute lifetime, one session per
   ///    device, a session bound to the address it began on - are settings of the
   ///    server (Settings.SessionIdleSeconds and the other three over COM, the
   ///    session_* keys of /api/v1/settings) and four fields of a domain
   ///    (Utilities.SetDomainSessionPolicy, the domain's session_* fields over
   ///    REST). Every assertion is against what the server then did to a real
   ///    session: a cookie that stops working, a connection that is closed.
   ///
   ///    The two timeouts are in seconds so that these tests can set one to a
   ///    second or two and prove the refusal without waiting out a minute; the
   ///    documents say a value under sixty is for testing.
   /// </summary>
   [TestFixture]
   public class RestApiDevices : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9590;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Original-Passw0rd!";

      // The second address a request can come from on the loopback interface.
      // Every 127.0.0.0/8 address is loopback on Windows and on Linux, so binding
      // a client socket to this one presents the server with a peer that is not
      // 127.0.0.1 without any network at all.
      private const string OtherLoopback = "127.0.0.2";

      private Account _account;

      private string Address
      {
         get { return "devices@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status", AdminHeader());
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         // Every policy back to unset, whether or not the test that set one got
         // to its own end: a one-second idle timeout left behind would end every
         // browser session of every fixture after this one.
         _settings.SessionIdleSeconds = 0;
         _settings.SessionLifetimeSeconds = 0;
         _settings.SessionOnePerDevice = false;
         _settings.SessionBindToAddress = false;

         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("Two sign-ins from two clients are two entries, each with what it is, and the one asking is marked current; the administrator reads the same two under the address")]
      public void TwoSignInsAreListedSeparately()
      {
         string phone = SignIn("Phone/1.0");
         string desktop = SignIn("Desktop/2.0");

         Response listed = Raw("GET", "/api/v1/me/devices", null, null, "Cookie: hmailsession=" + phone + "\r\n");
         Assert.AreEqual(200, listed.Status, listed.Body);

         List<Dictionary<string, object>> devices = DevicesOf(listed.Body);
         Assert.AreEqual(2, devices.Count, "Two clients, two entries. " + listed.Body);

         Dictionary<string, object> phoneEntry = EntryWithAgent(devices, "Phone/1.0");
         Dictionary<string, object> desktopEntry = EntryWithAgent(devices, "Desktop/2.0");

         Assert.AreEqual("web", MiniJson.Text(phoneEntry, "protocol"));
         Assert.AreEqual("127.0.0.1", MiniJson.Text(phoneEntry, "address"));
         Assert.AreEqual("password", MiniJson.Text(phoneEntry, "credential"), "Both signed in with the account's own password.");
         Assert.AreEqual("", MiniJson.Text(phoneEntry, "credential_name"));
         Assert.AreEqual(32, MiniJson.Text(phoneEntry, "id").Length, "A device id is thirty-two hex characters.");
         Assert.AreNotEqual(MiniJson.Text(phoneEntry, "id"), MiniJson.Text(desktopEntry, "id"));
         Assert.IsFalse(MiniJson.Flag(phoneEntry, "revoked"));
         Assert.IsTrue(MiniJson.Number(phoneEntry, "sign_ins") >= 1, listed.Body);
         Assert.IsFalse(string.IsNullOrEmpty(MiniJson.Text(phoneEntry, "last_seen")), listed.Body);

         Assert.IsTrue(MiniJson.Flag(phoneEntry, "current"), "The phone's session asked, so the phone is current. " + listed.Body);
         Assert.IsFalse(MiniJson.Flag(desktopEntry, "current"), listed.Body);

         // The other session sees the same two, with the mark on itself.
         Response fromDesktop = Raw("GET", "/api/v1/me/devices", null, null, "Cookie: hmailsession=" + desktop + "\r\n");
         Assert.AreEqual(200, fromDesktop.Status, fromDesktop.Body);
         Assert.IsTrue(MiniJson.Flag(EntryWithAgent(DevicesOf(fromDesktop.Body), "Desktop/2.0"), "current"), fromDesktop.Body);

         // A request that carried the password rather than a session came in on
         // no client the list knows, so nothing is current.
         (int status, string body) byPassword = Http("GET", "/api/v1/me/devices", UserHeader(UserPassword));
         Assert.AreEqual(200, byPassword.status, byPassword.body);
         foreach (Dictionary<string, object> entry in DevicesOf(byPassword.body))
            Assert.IsFalse(MiniJson.Flag(entry, "current"), byPassword.body);

         // The administrator reads the same rows under the address.
         (int status, string body) asAdmin = Http("GET", "/api/v1/accounts/" + Address + "/devices", AdminHeader());
         Assert.AreEqual(200, asAdmin.status, asAdmin.body);
         List<Dictionary<string, object>> adminView = DevicesOf(asAdmin.body);
         Assert.AreEqual(2, adminView.Count, asAdmin.body);
         Assert.AreEqual(MiniJson.Text(phoneEntry, "id"), MiniJson.Text(EntryWithAgent(adminView, "Phone/1.0"), "id"));

         // Never a password, and nothing derived from one, in either listing.
         StringAssert.DoesNotContain(UserPassword, listed.Body);
         StringAssert.DoesNotContain(UserPassword, asAdmin.body);

         // And COM answers the same document.
         string overCom = _account.GetDevices();
         StringAssert.Contains("\"id\":\"" + MiniJson.Text(phoneEntry, "id") + "\"", overCom);
         StringAssert.Contains("\"id\":\"" + MiniJson.Text(desktopEntry, "id") + "\"", overCom);

         (int status, string body) unknownAccount = Http("GET", "/api/v1/accounts/nobody@" + _domain.Name + "/devices", AdminHeader());
         Assert.AreEqual(404, unknownAccount.status, unknownAccount.body);
      }

      [Test]
      [Description("A revoked session is refused on its next request; the session that revoked it stays; a garbage id is refused and an unknown one is not found")]
      public void ARevokedSessionIsRefusedOnItsNextRequest()
      {
         string mine = SignIn("Mine/1.0");
         string theirs = SignIn("Theirs/1.0");

         Response theirsBefore = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + theirs + "\r\n");
         Assert.AreEqual(200, theirsBefore.Status, theirsBefore.Body);

         Response listed = Raw("GET", "/api/v1/me/devices", null, null, "Cookie: hmailsession=" + mine + "\r\n");
         string theirId = MiniJson.Text(EntryWithAgent(DevicesOf(listed.Body), "Theirs/1.0"), "id");

         Response garbage = Raw("DELETE", "/api/v1/me/devices/not-a-device", null, null,
            "Cookie: hmailsession=" + mine + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(400, garbage.Status, garbage.Body);

         Response unknown = Raw("DELETE", "/api/v1/me/devices/00000000000000000000000000000000", null, null,
            "Cookie: hmailsession=" + mine + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(404, unknown.Status, unknown.Body);

         Response revoked = Raw("DELETE", "/api/v1/me/devices/" + theirId, null, null,
            "Cookie: hmailsession=" + mine + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(200, revoked.Status, revoked.Body);
         Dictionary<string, object> outcome = MiniJson.Object(revoked.Body);
         Assert.IsTrue(MiniJson.Flag(outcome, "revoked"), revoked.Body);
         Assert.AreEqual(1, MiniJson.Number(outcome, "sessions_ended"), "The other client held one session. " + revoked.Body);
         Assert.IsFalse(MiniJson.Flag(outcome, "credential_disabled"), "Nothing asked for the credential to go. " + revoked.Body);

         Response theirsAfter = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + theirs + "\r\n");
         Assert.AreEqual(401, theirsAfter.Status, "The revoked client's session must be refused. " + theirsAfter.Body);

         Response mineAfter = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + mine + "\r\n");
         Assert.AreEqual(200, mineAfter.Status, "The session that revoked stays. " + mineAfter.Body);

         // The entry stays, marked: a revoke is not a ban.
         Response listedAfter = Raw("GET", "/api/v1/me/devices", null, null, "Cookie: hmailsession=" + mine + "\r\n");
         Dictionary<string, object> marked = EntryWithAgent(DevicesOf(listedAfter.Body), "Theirs/1.0");
         Assert.IsTrue(MiniJson.Flag(marked, "revoked"), listedAfter.Body);
         Assert.IsFalse(string.IsNullOrEmpty(MiniJson.Text(marked, "revoked_at")), listedAfter.Body);

         // And a client whose credential still works signs in again, which
         // clears the mark.
         SignIn("Theirs/1.0");
         Response listedAgain = Raw("GET", "/api/v1/me/devices", null, null, "Cookie: hmailsession=" + mine + "\r\n");
         Assert.IsFalse(MiniJson.Flag(EntryWithAgent(DevicesOf(listedAgain.Body), "Theirs/1.0"), "revoked"), listedAgain.Body);

         // forget=true removes the entry rather than marking it.
         Response forgotten = Raw("DELETE", "/api/v1/me/devices/" + theirId + "?forget=true", null, null,
            "Cookie: hmailsession=" + mine + "\r\nX-Requested-With: hMailServer\r\n");
         Assert.AreEqual(200, forgotten.Status, forgotten.Body);
         Assert.IsTrue(MiniJson.Flag(MiniJson.Object(forgotten.Body), "forgotten"), forgotten.Body);
         Response listedLast = Raw("GET", "/api/v1/me/devices", null, null, "Cookie: hmailsession=" + mine + "\r\n");
         Assert.IsNull(EntryWithAgentOrNull(DevicesOf(listedLast.Body), "Theirs/1.0"), listedLast.Body);
      }

      [Test]
      [Description("An IMAP client is listed with the app password it used by name; revoking it drops the live connection, and with credential=revoke the app password no longer signs in while the account's own password still does")]
      public void AMailClientIsListedAndRevokingItDropsItsConnection()
      {
         (int status, string body) issued = Http("POST", "/api/v1/accounts/" + Address + "/app-passwords", AdminHeader(), "{\"name\":\"Phone\"}");
         Assert.AreEqual(201, issued.status, issued.body);
         string appPassword = MiniJson.Text(MiniJson.Object(issued.body), "password");
         Assert.IsFalse(string.IsNullOrEmpty(appPassword), issued.body);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(Address, appPassword), "The app password must log on over IMAP.");

         (int status, string body) listed = Http("GET", "/api/v1/accounts/" + Address + "/devices", AdminHeader());
         Assert.AreEqual(200, listed.status, listed.body);
         Dictionary<string, object> client = EntryWithProtocol(DevicesOf(listed.body), "imap");
         Assert.AreEqual("app_password", MiniJson.Text(client, "credential"), listed.body);
         Assert.AreEqual("Phone", MiniJson.Text(client, "credential_name"), "The app password's name, so that the person knows which credential the client holds. " + listed.body);
         Assert.AreEqual("", MiniJson.Text(client, "agent"), "IMAP carries no user agent. " + listed.body);
         StringAssert.DoesNotContain(appPassword, listed.body, "Never the secret.");

         string id = MiniJson.Text(client, "id");

         // A request authenticated with an app password may not revoke: a
         // credential for a program does not revoke the person's credentials.
         (int status, string body) byAppPassword = Http("DELETE", "/api/v1/me/devices/" + id, BasicHeader(Address, appPassword));
         Assert.AreEqual(403, byAppPassword.status, byAppPassword.body);

         (int status, string body) revoked = Http("DELETE", "/api/v1/accounts/" + Address + "/devices/" + id + "?credential=revoke", AdminHeader());
         Assert.AreEqual(200, revoked.status, revoked.body);
         Dictionary<string, object> outcome = MiniJson.Object(revoked.body);
         Assert.AreEqual(1, MiniJson.Number(outcome, "connections_dropped"), "The one live IMAP connection. " + revoked.body);
         Assert.IsTrue(MiniJson.Flag(outcome, "credential_disabled"), revoked.body);

         // The connection was dropped where it stood: the next command gets no
         // OK, because there is nobody at the other end to give one.
         string afterwards;
         try
         {
            afterwards = imap.SendSingleCommand("A02 NOOP");
         }
         catch (Exception ex)
         {
            afterwards = "(closed: " + ex.GetType().Name + ")";
         }
         Assert.IsFalse(afterwards.Contains("A02 OK"), "The connection must have been dropped, but NOOP answered: " + afterwards);
         try { imap.Disconnect(); } catch (Exception) { }

         // The app password is off, so the stolen phone cannot simply sign in
         // again; the person's own password is untouched.
         var again = new ImapClientSimulator();
         Assert.IsFalse(again.ConnectAndLogon(Address, appPassword), "The deactivated app password must be refused.");
         try { again.Disconnect(); } catch (Exception) { }

         var own = new ImapClientSimulator();
         Assert.IsTrue(own.ConnectAndLogon(Address, UserPassword), "The account's own password still logs on.");
         own.Disconnect();

         (int status, string body) passwords = Http("GET", "/api/v1/accounts/" + Address + "/app-passwords", AdminHeader());
         StringAssert.Contains("\"active\":false", passwords.body, "The app password is deactivated, not deleted. " + passwords.body);

         // Over COM the same client is there, and RevokeDevice answers whether
         // the account had such a client.
         StringAssert.Contains("\"id\":\"" + id + "\"", _account.GetDevices());
         Assert.IsTrue(_account.RevokeDevice(id, false));
         Assert.IsFalse(_account.RevokeDevice("00000000000000000000000000000000", false));
      }

      [Test]
      [Description("A session past the idle timeout is refused, and the timeout is what the sign-in announced")]
      public void ASessionPastItsIdleTimeoutIsRefused()
      {
         _settings.SessionIdleSeconds = 1;

         Response started = Raw("POST", "/api/v1/session", UserHeader(UserPassword), null, "User-Agent: Idle/1.0\r\n");
         Assert.AreEqual(201, started.Status, started.Body);
         Assert.AreEqual(1, MiniJson.Number(MiniJson.Object(started.Body), "idle_seconds"), started.Body);
         Assert.AreEqual(43200, MiniJson.Number(MiniJson.Object(started.Body), "lifetime_seconds"), "The lifetime is unset, so the ceiling applies. " + started.Body);
         string cookie = CookieOf(started.Header("Set-Cookie"));

         Response atOnce = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(200, atOnce.Status, "A fresh session works. " + atOnce.Body);

         Thread.Sleep(2200);

         Response later = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(401, later.Status, "A session idle for longer than the timeout must be refused. " + later.Body);

         // The refusal is the timeout's and not the listener's: a new session
         // starts as before.
         Response fresh = Raw("POST", "/api/v1/session", UserHeader(UserPassword), null, "User-Agent: Idle/1.0\r\n");
         Assert.AreEqual(201, fresh.Status, fresh.Body);
      }

      [Test]
      [Description("A session past its absolute lifetime is refused however busy it has been")]
      public void ASessionPastItsAbsoluteLifetimeIsRefused()
      {
         _settings.SessionLifetimeSeconds = 2;

         Response started = Raw("POST", "/api/v1/session", UserHeader(UserPassword), null, "User-Agent: Busy/1.0\r\n");
         Assert.AreEqual(201, started.Status, started.Body);
         Assert.AreEqual(2, MiniJson.Number(MiniJson.Object(started.Body), "lifetime_seconds"), started.Body);
         Assert.AreEqual(1800, MiniJson.Number(MiniJson.Object(started.Body), "idle_seconds"), "The idle timeout is unset, so the ceiling applies. " + started.Body);
         StringAssert.Contains("Max-Age=2", started.Header("Set-Cookie"), "The cookie's own ceiling is the lifetime.");
         string cookie = CookieOf(started.Header("Set-Cookie"));

         // Kept busy: a request every few hundred milliseconds, so that only the
         // lifetime can be what ends it.
         int accepted = 0;
         int refusedAfterMilliseconds = -1;
         DateTime from = DateTime.UtcNow;
         for (int attempt = 0; attempt < 20; attempt++)
         {
            Response probe = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
            if (probe.Status == 200)
            {
               accepted++;
            }
            else
            {
               Assert.AreEqual(401, probe.Status, probe.Body);
               refusedAfterMilliseconds = (int) (DateTime.UtcNow - from).TotalMilliseconds;
               break;
            }

            Thread.Sleep(300);
         }

         Assert.IsTrue(accepted >= 1, "The session must have worked at first.");
         Assert.IsTrue(refusedAfterMilliseconds >= 0, "The session must have been refused once its lifetime was over.");
         Assert.IsTrue(refusedAfterMilliseconds >= 1500, "Refused too early, after " + refusedAfterMilliseconds + " ms, for a lifetime of two seconds.");
      }

      [Test]
      [Description("With the policy off an address that changes is ordinary; with SessionBindToAddress on a session presented from another address is refused, and ended")]
      public void ASessionWhoseAddressChangesIsRefusedWhenBound()
      {
         string unbound = SignIn("Roaming/1.0");
         Response moved = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + unbound + "\r\n", OtherLoopback);
         Assert.AreEqual(200, moved.Status, "With the policy off, a session from another address is ordinary. " + moved.Body);

         _settings.SessionBindToAddress = true;

         Response started = Raw("POST", "/api/v1/session", UserHeader(UserPassword), null, "User-Agent: Bound/1.0\r\n");
         Assert.AreEqual(201, started.Status, started.Body);
         Assert.IsTrue(MiniJson.Flag(MiniJson.Object(started.Body), "bound_to_address"), started.Body);
         string bound = CookieOf(started.Header("Set-Cookie"));

         Response sameAddress = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + bound + "\r\n");
         Assert.AreEqual(200, sameAddress.Status, sameAddress.Body);

         Response elsewhere = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + bound + "\r\n", OtherLoopback);
         Assert.AreEqual(401, elsewhere.Status, "A bound session presented from another address must be refused. " + elsewhere.Body);

         // Refused and ended, not merely refused there: the cookie is of no use
         // back where it began either.
         Response backHome = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + bound + "\r\n");
         Assert.AreEqual(401, backHome.Status, "The session was ended when it was presented from elsewhere. " + backHome.Body);

         // The policy is re-read at every request, so turning it on reaches the
         // session that began before it did.
         _settings.SessionBindToAddress = false;
         string beforeSwitch = SignIn("Late/1.0");
         _settings.SessionBindToAddress = true;
         Response lateElsewhere = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + beforeSwitch + "\r\n", OtherLoopback);
         Assert.AreEqual(401, lateElsewhere.Status, lateElsewhere.Body);
      }

      [Test]
      [Description("With SessionOnePerDevice on, signing in again from the same client ends its earlier session and leaves another client's alone")]
      public void OneSessionPerDeviceEndsTheEarlierSessionOfTheSameClient()
      {
         _settings.SessionOnePerDevice = true;

         string first = SignIn("Shared/1.0");
         string other = SignIn("Other/1.0");
         string second = SignIn("Shared/1.0");

         Response firstAfter = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + first + "\r\n");
         Assert.AreEqual(401, firstAfter.Status, "The earlier session of the same client must have been ended. " + firstAfter.Body);

         Response secondAfter = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + second + "\r\n");
         Assert.AreEqual(200, secondAfter.Status, secondAfter.Body);

         Response otherAfter = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + other + "\r\n");
         Assert.AreEqual(200, otherAfter.Status, "A different client's session is nobody else's business. " + otherAfter.Body);
      }

      [Test]
      [Description("A domain's own policy wins over the server's where it is set, and a domain can switch a server-wide policy off for itself")]
      public void ADomainsPolicyWinsOverTheServers()
      {
         Assert.IsFalse(_application.Utilities.SetDomainSessionPolicy("nobody.invalid", 0, 0, 0, 0), "A domain this server does not host.");

         // The server says nothing about idle; the domain says one second.
         Assert.IsTrue(_application.Utilities.SetDomainSessionPolicy(_domain.Name, 1, 0, 0, 0));

         string policy = _application.Utilities.GetSessionPolicy(_domain.Name);
         Dictionary<string, object> document = MiniJson.Object(policy);
         Assert.AreEqual(1, MiniJson.Number(document, "in_force.idle_seconds"), policy);
         Assert.AreEqual(43200, MiniJson.Number(document, "in_force.lifetime_seconds"), policy);
         Assert.AreEqual(1, MiniJson.Number(document, "domain_policy.idle_seconds"), policy);
         Assert.AreEqual(0, MiniJson.Number(document, "server.idle_seconds"), policy);

         (int status, string body) overRest = Http("GET", "/api/v1/domains/" + _domain.Name + "/session-policy", AdminHeader());
         Assert.AreEqual(200, overRest.status, overRest.body);
         Assert.AreEqual(policy, overRest.body, "REST and COM answer one document.");

         (int status, string body) domainRows = Http("GET", "/api/v1/domains", AdminHeader());
         Assert.AreEqual(200, domainRows.status, domainRows.body);
         StringAssert.Contains("\"session_idle_seconds\":1", domainRows.body, "The domain's row carries its own policy.");

         Response started = Raw("POST", "/api/v1/session", UserHeader(UserPassword), null, "User-Agent: Domain/1.0\r\n");
         Assert.AreEqual(201, started.Status, started.Body);
         Assert.AreEqual(1, MiniJson.Number(MiniJson.Object(started.Body), "idle_seconds"), started.Body);
         string cookie = CookieOf(started.Header("Set-Cookie"));
         Thread.Sleep(2200);
         Response later = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + cookie + "\r\n");
         Assert.AreEqual(401, later.Status, "The domain's one-second idle timeout applies to its accounts. " + later.Body);

         // The server binds sessions to their address; the domain says "not here".
         _settings.SessionBindToAddress = true;
         Assert.IsTrue(_application.Utilities.SetDomainSessionPolicy(_domain.Name, 0, 0, 0, 2));
         string unbound = SignIn("Unbound/1.0");
         Response elsewhere = Raw("GET", "/api/v1/me", null, null, "Cookie: hmailsession=" + unbound + "\r\n", OtherLoopback);
         Assert.AreEqual(200, elsewhere.Status, "The domain switched the server-wide policy off for its own accounts. " + elsewhere.Body);

         // The administrator's own session belongs to no domain: the server's
         // value alone governs it.
         Response adminStarted = Raw("POST", "/api/v1/session", AdminHeader(), null, "User-Agent: Admin/1.0\r\n");
         Assert.AreEqual(201, adminStarted.Status, adminStarted.Body);
         Assert.IsTrue(MiniJson.Flag(MiniJson.Object(adminStarted.Body), "bound_to_address"), adminStarted.Body);

         // A number past the ceiling is refused at every interface with the same
         // sentence, and nothing is stored.
         Assert.Throws<System.Runtime.InteropServices.COMException>(() => _settings.SessionIdleSeconds = 2592001);
         Assert.AreEqual(0, _settings.SessionIdleSeconds);
         (int status, string body) tooLong = Http("PUT", "/api/v1/domains/" + _domain.Name, AdminHeader(), "{\"active\":true,\"session_lifetime_seconds\":2592001}");
         Assert.AreEqual(400, tooLong.status, tooLong.body);
         (int status, string body) badSwitch = Http("PUT", "/api/v1/domains/" + _domain.Name, AdminHeader(), "{\"active\":true,\"session_one_per_device\":3}");
         Assert.AreEqual(400, badSwitch.status, badSwitch.body);

         Assert.IsTrue(_application.Utilities.SetDomainSessionPolicy(_domain.Name, 0, 0, 0, 0));
      }

      [Test]
      [Description("An upgrade leaves every policy unset: the server's four rows and a domain's four fields read 0, and the ceilings that always applied are what a session gets")]
      public void TheUpgradeLeavesEveryPolicyOff()
      {
         Assert.AreEqual(0, _settings.SessionIdleSeconds);
         Assert.AreEqual(0, _settings.SessionLifetimeSeconds);
         Assert.IsFalse(_settings.SessionOnePerDevice);
         Assert.IsFalse(_settings.SessionBindToAddress);

         (int status, string body) settings = Http("GET", "/api/v1/settings", AdminHeader());
         Assert.AreEqual(200, settings.status, settings.body);
         StringAssert.Contains("\"session_idle_seconds\":0", settings.body);
         StringAssert.Contains("\"session_lifetime_seconds\":0", settings.body);
         StringAssert.Contains("\"session_one_per_device\":false", settings.body);
         StringAssert.Contains("\"session_bind_to_address\":false", settings.body);

         string serverWide = _application.Utilities.GetSessionPolicy("");
         Dictionary<string, object> document = MiniJson.Object(serverWide);
         Assert.AreEqual(1800, MiniJson.Number(document, "in_force.idle_seconds"), serverWide);
         Assert.AreEqual(43200, MiniJson.Number(document, "in_force.lifetime_seconds"), serverWide);
         Assert.IsFalse(MiniJson.Flag(document, "in_force.one_per_device"), serverWide);
         Assert.IsFalse(MiniJson.Flag(document, "in_force.bind_to_address"), serverWide);
         Assert.AreEqual(1800, MiniJson.Number(document, "defaults.idle_seconds"), serverWide);
         Assert.IsNull(MiniJson.At(document, "domain_policy"), "No domain was named. " + serverWide);

         string forDomain = _application.Utilities.GetSessionPolicy(_domain.Name);
         Dictionary<string, object> domainDocument = MiniJson.Object(forDomain);
         Assert.AreEqual(0, MiniJson.Number(domainDocument, "domain_policy.idle_seconds"), forDomain);
         Assert.AreEqual(0, MiniJson.Number(domainDocument, "domain_policy.lifetime_seconds"), forDomain);
         Assert.AreEqual(0, MiniJson.Number(domainDocument, "domain_policy.one_per_device"), forDomain);
         Assert.AreEqual(0, MiniJson.Number(domainDocument, "domain_policy.bind_to_address"), forDomain);
         Assert.AreEqual(1800, MiniJson.Number(domainDocument, "in_force.idle_seconds"), forDomain);

         Response started = Raw("POST", "/api/v1/session", UserHeader(UserPassword), null, "User-Agent: Upgrade/1.0\r\n");
         Assert.AreEqual(201, started.Status, started.Body);
         Dictionary<string, object> session = MiniJson.Object(started.Body);
         Assert.AreEqual(1800, MiniJson.Number(session, "idle_seconds"), started.Body);
         Assert.AreEqual(43200, MiniJson.Number(session, "lifetime_seconds"), started.Body);
         Assert.IsFalse(MiniJson.Flag(session, "one_per_device"), started.Body);
         Assert.IsFalse(MiniJson.Flag(session, "bound_to_address"), started.Body);
      }

      // ------------------------------------------------------------ helpers ---

      private string SignIn(string agent)
      {
         Response started = Raw("POST", "/api/v1/session", UserHeader(UserPassword), null, "User-Agent: " + agent + "\r\n");
         Assert.AreEqual(201, started.Status, started.Body);
         return CookieOf(started.Header("Set-Cookie"));
      }

      private static List<Dictionary<string, object>> DevicesOf(string body)
      {
         var result = new List<Dictionary<string, object>>();
         foreach (object entry in MiniJson.List(MiniJson.Object(body), "devices"))
            result.Add((Dictionary<string, object>) entry);
         return result;
      }

      private static Dictionary<string, object> EntryWithAgentOrNull(List<Dictionary<string, object>> devices, string agent)
      {
         foreach (Dictionary<string, object> entry in devices)
         {
            if (MiniJson.Text(entry, "agent") == agent)
               return entry;
         }

         return null;
      }

      private static Dictionary<string, object> EntryWithAgent(List<Dictionary<string, object>> devices, string agent)
      {
         Dictionary<string, object> entry = EntryWithAgentOrNull(devices, agent);
         Assert.IsNotNull(entry, "No client with the user agent " + agent + " is listed.");
         return entry;
      }

      private static Dictionary<string, object> EntryWithProtocol(List<Dictionary<string, object>> devices, string protocol)
      {
         foreach (Dictionary<string, object> entry in devices)
         {
            if (MiniJson.Text(entry, "protocol") == protocol)
               return entry;
         }

         Assert.Fail("No " + protocol + " client is listed.");
         return null;
      }

      private static string CookieOf(string setCookie)
      {
         int start = setCookie.IndexOf('=') + 1;
         int end = setCookie.IndexOf(';', start);
         return end > start ? setCookie.Substring(start, end - start) : setCookie.Substring(start);
      }

      private static string AdminHeader()
      {
         return BasicHeader("Administrator", AdminPassword);
      }

      private string UserHeader(string password)
      {
         return BasicHeader(Address, password);
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

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null)
      {
         Response response = Raw(method, path, authorization, requestBody);
         return (response.Status, response.Body);
      }

      /// <summary>
      ///    One HTTP/1.0 request over a socket of its own. localAddress, when
      ///    given, is the loopback address the socket is bound to, which is the
      ///    address the server sees the request come from.
      /// </summary>
      private static Response Raw(string method, string path, string authorization, string requestBody, string extraHeaders = null, string localAddress = null)
      {
         using (var client = localAddress == null
            ? new TcpClient()
            : new TcpClient(new IPEndPoint(IPAddress.Parse(localAddress), 0)))
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
