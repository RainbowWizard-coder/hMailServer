// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Linq;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The two client-discovery endpoints added alongside the existing
   ///    Thunderbird autoconfig and Outlook autodiscover handlers, both served
   ///    by the web services listener (hMailServer.ini [Settings]
   ///    WebServicesHttpPort).
   ///
   ///    (a) /email.mobileconfig - an Apple configuration profile. Apple
   ///    devices were the one mainstream client with no discovery path here, so
   ///    every iPhone had to be set up by hand from a wiki page. The property
   ///    worth pinning is not that the file exists but that it agrees with the
   ///    other two formats: the profile is built from the same
   ///    GetClientAccessSettings_ data as the Thunderbird XML, so the two must
   ///    advertise the same ports. A second, independently written source of
   ///    truth for "which port is IMAP on" is exactly how an autoconfig handler
   ///    ends up quietly pointing clients at a port that was renumbered years
   ///    ago. The PayloadUUID must also be stable across requests: iOS and
   ///    macOS key an installed profile by that UUID, so a fresh random one per
   ///    request would leave a user who reinstalls the profile with a stack of
   ///    duplicate mail accounts instead of one replaced account.
   ///
   ///    (b) /.well-known/caldav and /.well-known/carddav (RFC 6764). Both
   ///    protocols are served by this server under /dav/ (CardDAV since
   ///    September 2026, CalDAV since the calendar wave that followed), so
   ///    with no CalDavRedirectUrl or CardDavRedirectUrl each path points at
   ///    the built-in /dav/ on the host the client used, and a configured
   ///    target still wins - the redirect settings exist so an administrator
   ///    can pair a separate calendar or contacts server and have clients
   ///    find it. Before CalDAV was written here the unconfigured caldav path
   ///    answered 404 on purpose, because a redirect to a server that does
   ///    not speak the protocol leaves a client retrying a broken account
   ///    forever; it speaks it now (the CalDav fixture covers the calendar,
   ///    the CardDav fixture the address book).
   ///
   ///    Against a build without these handlers every test here fails with 404
   ///    "not found", because neither path is routed.
   ///
   ///    (c) /autodiscover/autodiscover.xml - Outlook's POX Autodiscover, which
   ///    had no test before the delegated-mailbox row of September 2026. The
   ///    settings are answered to everybody, because Outlook's first request
   ///    carries no credentials; the mailboxes other accounts have opened to
   ///    the account asked about are answered as Exchange's AlternativeMailbox
   ///    elements, and only to a request that signed in, over HTTPS, as that
   ///    address - who delegates to whom is not public. Outlook reads the
   ///    element for an Exchange account and ignores it for an IMAP one, and
   ///    it asks again with credentials only after a 401, which this server
   ///    never sends for an unauthenticated Autodiscover request; so what makes
   ///    a delegated mailbox appear in an IMAP client is the "#Users" namespace
   ///    (RegressionTests/IMAP/SharedMailboxes.cs), and the tests here pin the
   ///    element for what it is: the announcement, private to its owner.
   /// </summary>
   [TestFixture]
   public class ClientDiscovery : TestFixtureBase
   {
      private const int WebServicesPort = 9103;

      private const string CalDavTarget = "https://calendar.example.test/dav/";
      private const string CardDavTarget = "https://contacts.example.test/dav/";

      private void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      // Issues a minimal HTTP/1.0 GET against the web services listener and
      // returns the status line code, the raw response headers and the body.
      // The headers are needed as well as the body here: the whole point of the
      // RFC 6764 endpoints is the Location header.
      private static (int status, string headers, string body) HttpGet(string path, string host, string extraHeaders = "")
      {
         return HttpExchange("GET", path, host, null, extraHeaders);
      }

      // The same, as a POST with a body - the way Outlook asks Autodiscover.
      private static (int status, string headers, string body) HttpPost(string path, string host, string body, string extraHeaders = "")
      {
         return HttpExchange("POST", path, host, body, extraHeaders);
      }

      private static (int status, string headers, string body) HttpExchange(string method, string path, string host, string requestBody, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", WebServicesPort);
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
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n" + extraHeaders);
               if (requestBody != null)
               {
                  head.Append("Content-Type: text/xml; charset=utf-8\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length.ToString(CultureInfo.InvariantCulture) + "\r\n");
               }
               head.Append("\r\n");

               byte[] request = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(request, 0, request.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int statusCode = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string headers = separator >= 0 ? raw.Substring(0, separator) : raw;
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               return (statusCode, headers, body);
            }
         }
      }

      private static string HeaderValue(string headers, string name)
      {
         string header = headers.Split(new[] { "\r\n" }, StringSplitOptions.None)
            .FirstOrDefault(raw => raw.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase));

         return header?.Substring(name.Length + 1).Trim();
      }

      // Reads the value that follows <key>name</key> in an Apple plist. The
      // profile is small and generated, so locating the next value element is
      // enough - no XML parser is needed to pin these properties.
      private static string PlistValue(string body, string name, string element)
      {
         int keyPosition = body.IndexOf("<key>" + name + "</key>", StringComparison.Ordinal);
         if (keyPosition < 0)
            return null;

         int valueStart = body.IndexOf("<" + element + ">", keyPosition, StringComparison.Ordinal);
         if (valueStart < 0)
            return null;

         valueStart += element.Length + 2;

         int valueEnd = body.IndexOf("</" + element + ">", valueStart, StringComparison.Ordinal);
         if (valueEnd < 0)
            return null;

         return body.Substring(valueStart, valueEnd - valueStart);
      }

      // The <port> of the first server element of the given type in the
      // Thunderbird autoconfig XML.
      private static int AutoconfigPort(string xml, string serverType)
      {
         int typePosition = xml.IndexOf("type=\"" + serverType + "\"", StringComparison.Ordinal);
         Assert.Greater(typePosition, 0, "The autoconfig XML has no " + serverType + " server. XML: " + xml);

         int portStart = xml.IndexOf("<port>", typePosition, StringComparison.Ordinal) + 6;
         int portEnd = xml.IndexOf("</port>", portStart, StringComparison.Ordinal);

         return int.Parse(xml.Substring(portStart, portEnd - portStart), CultureInfo.InvariantCulture);
      }

      [SetUp]
      public void StartWebServices()
      {
         // Start from a known-clean discovery configuration: another test in
         // this fixture may have left a redirect target behind.
         WriteSetting("CalDavRedirectUrl", "");
         WriteSetting("CardDavRedirectUrl", "");

         WriteSetting("WebServicesBindAddress", "127.0.0.1");
         WriteSetting("WebServicesHttpPort", WebServicesPort.ToString());
         // Plain HTTP only: the built-in CardDAV redirect names the HTTPS
         // listener when one is configured, and the test below pins the
         // answer without one.
         WriteSetting("WebServicesHttpsPort", "0");

         // Reinitialize (not Stop/Start): the web services ports are cached in
         // IniFileSettings, which is only re-read by InitInstance().
         _application.Reinitialize();
      }

      [TearDown]
      public void StopWebServices()
      {
         // The redirect targets must not survive this fixture. A non-empty but
         // unusable value makes the server report HM5780, which would fail the
         // clean-error-log assertion of whichever fixture ran next.
         WriteSetting("CalDavRedirectUrl", "");
         WriteSetting("CardDavRedirectUrl", "");
         WriteSetting("WebServicesHttpPort", "0");

         _application.Reinitialize();

         LogHandler.DeleteErrorLog();
      }

      // The configuration profile is served over HTTPS only. This listener is plain
      // HTTP, so the tests say what a TLS-terminating proxy in front of it would say.
      private static (int status, string headers, string body) HttpsGet(string path, string host)
      {
         return HttpGet(path, host, "X-Forwarded-Proto: https\r\n");
      }

      [Test]
      [Description("A .mobileconfig asked for over plain HTTP, with no proxy saying otherwise, is refused with the reason - the profile decides which servers get the user's password")]
      public void MobileConfigIsRefusedOverPlainHttp()
      {
         (int status, string headers, string body) response = HttpGet("/email.mobileconfig", "example.test");
         Assert.AreEqual(403, response.status, "Body: " + response.body);
         StringAssert.Contains("HTTPS", response.body);
         StringAssert.Contains("X-Forwarded-Proto", response.body);
         Assert.IsFalse(response.body.Contains("<plist"), "No profile at all over plain HTTP. Body: " + response.body);
      }

      [Test]
      [Description("An Apple .mobileconfig profile is served with the Apple content type and a complete mail payload")]
      public void MobileConfigProfileIsServed()
      {
         (int status, string headers, string body) response = HttpsGet("/email.mobileconfig", "example.test");

         Assert.AreEqual(200, response.status, "A configuration profile should be served. Body: " + response.body);

         // The content type is what makes iOS hand the file to the profile
         // installer instead of showing it as text.
         Assert.AreEqual("application/x-apple-aspen-config", HeaderValue(response.headers, "Content-Type"),
            "Headers: " + response.headers);

         Assert.AreEqual("com.apple.mail.managed", PlistValue(response.body, "PayloadType", "string"),
            "The first PayloadType is the mail payload's. Body: " + response.body);

         // Both directions are mandatory in a com.apple.mail.managed payload. A
         // profile with only an incoming server installs and then cannot send.
         Assert.IsNotNull(PlistValue(response.body, "IncomingMailServerHostName", "string"),
            "Body: " + response.body);
         Assert.IsNotNull(PlistValue(response.body, "OutgoingMailServerHostName", "string"),
            "Body: " + response.body);
         Assert.IsNotNull(PlistValue(response.body, "IncomingMailServerPortNumber", "integer"),
            "Body: " + response.body);
         Assert.IsNotNull(PlistValue(response.body, "OutgoingMailServerPortNumber", "integer"),
            "Body: " + response.body);
      }

      [Test]
      [Description("The .mobileconfig ports match the Thunderbird autoconfig ports - one source of truth, three formats")]
      public void MobileConfigAgreesWithThunderbirdAutoconfig()
      {
         (int status, string headers, string body) autoconfig =
            HttpGet("/mail/config-v1.1.xml", "example.test");

         Assert.AreEqual(200, autoconfig.status, "Body: " + autoconfig.body);

         (int status, string headers, string body) profile =
            HttpsGet("/email.mobileconfig", "example.test");

         Assert.AreEqual(200, profile.status, "Body: " + profile.body);

         int expectedIncoming = AutoconfigPort(autoconfig.body, "imap");
         int expectedOutgoing = AutoconfigPort(autoconfig.body, "smtp");

         Assert.AreEqual(expectedIncoming.ToString(CultureInfo.InvariantCulture),
            PlistValue(profile.body, "IncomingMailServerPortNumber", "integer"),
            "The profile must advertise the same IMAP port as the autoconfig XML. Profile: " + profile.body);

         Assert.AreEqual(expectedOutgoing.ToString(CultureInfo.InvariantCulture),
            PlistValue(profile.body, "OutgoingMailServerPortNumber", "integer"),
            "The profile must advertise the same SMTP port as the autoconfig XML. Profile: " + profile.body);
      }

      [Test]
      [Description("The PayloadUUID is stable across requests, so reinstalling replaces the profile instead of duplicating the account")]
      public void MobileConfigUuidIsStableAcrossRequests()
      {
         (int status, string headers, string body) first = HttpsGet("/email.mobileconfig", "example.test");
         (int status, string headers, string body) second = HttpsGet("/email.mobileconfig", "example.test");

         Assert.AreEqual(200, first.status, "Body: " + first.body);
         Assert.AreEqual(200, second.status, "Body: " + second.body);

         string firstUuid = PlistValue(first.body, "PayloadUUID", "string");
         Assert.IsNotNull(firstUuid, "Body: " + first.body);

         Assert.AreEqual(firstUuid, PlistValue(second.body, "PayloadUUID", "string"),
            "Two fetches of the same profile must carry the same PayloadUUID. A random UUID per request "
            + "makes every reinstall add another copy of the account on the device.");
      }

      [Test]
      [Description("A supplied address is pre-filled, and omitted entirely when none was supplied so the device prompts")]
      public void MobileConfigPrefillsOnlyASuppliedAddress()
      {
         (int status, string headers, string body) withAddress =
            HttpsGet("/email.mobileconfig?emailaddress=someone%40example.test", "example.test");

         Assert.AreEqual(200, withAddress.status, "Body: " + withAddress.body);
         Assert.AreEqual("someone@example.test", PlistValue(withAddress.body, "EmailAddress", "string"),
            "Body: " + withAddress.body);

         (int status, string headers, string body) withoutAddress =
            HttpsGet("/email.mobileconfig", "example.test");

         Assert.AreEqual(200, withoutAddress.status, "Body: " + withoutAddress.body);

         // Absent rather than empty: an empty EmailAddress installs an account
         // with no address on it, while an absent one makes the device ask.
         Assert.IsNull(PlistValue(withoutAddress.body, "EmailAddress", "string"),
            "With no address supplied the key must be left out so the device prompts. Body: "
            + withoutAddress.body);
      }

      [Test]
      [Description("/.well-known/caldav redirects to the configured calendar server (RFC 6764)")]
      public void CalDavWellKnownRedirectsToConfiguredTarget()
      {
         WriteSetting("CalDavRedirectUrl", CalDavTarget);
         _application.Reinitialize();

         (int status, string headers, string body) response = HttpGet("/.well-known/caldav", "example.test");

         Assert.AreEqual(301, response.status,
            "A configured target should produce a permanent redirect. Headers: " + response.headers);

         Assert.AreEqual(CalDavTarget, HeaderValue(response.headers, "Location"),
            "Headers: " + response.headers);
      }

      [Test]
      [Description("/.well-known/carddav redirects to the configured contacts server, independently of CalDAV")]
      public void CardDavWellKnownRedirectsToConfiguredTarget()
      {
         WriteSetting("CardDavRedirectUrl", CardDavTarget);
         _application.Reinitialize();

         (int status, string headers, string body) carddav = HttpGet("/.well-known/carddav", "example.test");

         Assert.AreEqual(301, carddav.status, "Headers: " + carddav.headers);
         Assert.AreEqual(CardDavTarget, HeaderValue(carddav.headers, "Location"), "Headers: " + carddav.headers);

         // The two settings are separate: configuring contacts must not send
         // calendar clients to the contacts server. The caldav path keeps
         // pointing at the built-in calendar.
         (int status, string headers, string body) caldav = HttpGet("/.well-known/caldav", "example.test");

         Assert.AreEqual(301, caldav.status, "Headers: " + caldav.headers);
         Assert.AreNotEqual(CardDavTarget, HeaderValue(caldav.headers, "Location"),
            "CardDavRedirectUrl must not answer for CalDAV. Headers: " + caldav.headers);
         StringAssert.EndsWith("/dav/", HeaderValue(caldav.headers, "Location"), "Headers: " + caldav.headers);
      }

      [Test]
      [Description("With no CalDavRedirectUrl, /.well-known/caldav points at this server's own calendar under /dav/, on the host the client used (RFC 6764 section 6)")]
      public void CalDavWellKnownPointsAtTheBuiltInCalendarWithoutAConfiguredTarget()
      {
         // Until the calendar was written here this path answered 404 when
         // unconfigured, and a test guarded that: a redirect to a server that
         // does not speak CalDAV leaves a client retrying a broken account
         // forever. The server speaks it now, so a client that arrives by
         // discovery is sent to it, exactly as the carddav path sends it to
         // the address book - absolute, on the host and port the client
         // named, https when the request came over TLS or a proxy says it did.
         (int status, string headers, string body) caldav =
            HttpGet("/.well-known/caldav", "mail.example.test:" + WebServicesPort, "X-Forwarded-Proto: https\r\n");

         Assert.AreEqual(301, caldav.status, "Headers: " + caldav.headers);
         Assert.AreEqual("https://mail.example.test:" + WebServicesPort + "/dav/", HeaderValue(caldav.headers, "Location"),
            "Headers: " + caldav.headers);

         (int status, string headers, string body) plain = HttpGet("/.well-known/caldav", "mail.example.test:" + WebServicesPort);

         Assert.AreEqual(301, plain.status, "Headers: " + plain.headers);
         Assert.AreEqual("http://mail.example.test:" + WebServicesPort + "/dav/", HeaderValue(plain.headers, "Location"),
            "Headers: " + plain.headers);
      }

      [Test]
      [Description("With no CardDavRedirectUrl, /.well-known/carddav points at this server's own address book under /dav/, on the host the client used (RFC 6764 section 6)")]
      public void CardDavWellKnownPointsAtTheBuiltInAddressBookWithoutAConfiguredTarget()
      {
         // The opposite of the caldav case, and for the opposite reason: this
         // server does speak CardDAV, so a client that arrives by discovery
         // must be sent to it rather than told there is nothing. The Location
         // is absolute, on the host and port the client named, and https
         // when the request came over TLS or a proxy says it did - /dav/
         // itself refuses Basic over plain HTTP.
         (int status, string headers, string body) carddav =
            HttpGet("/.well-known/carddav", "mail.example.test:" + WebServicesPort, "X-Forwarded-Proto: https\r\n");

         Assert.AreEqual(301, carddav.status, "Headers: " + carddav.headers);
         Assert.AreEqual("https://mail.example.test:" + WebServicesPort + "/dav/", HeaderValue(carddav.headers, "Location"),
            "Headers: " + carddav.headers);

         // Over plain HTTP with no HTTPS listener configured, the client is
         // still sent to /dav/ - where it is refused with the reason, which is
         // the sentence an administrator can act on.
         (int status, string headers, string body) plain = HttpGet("/.well-known/carddav", "mail.example.test:" + WebServicesPort);

         Assert.AreEqual(301, plain.status, "Headers: " + plain.headers);
         Assert.AreEqual("http://mail.example.test:" + WebServicesPort + "/dav/", HeaderValue(plain.headers, "Location"),
            "Headers: " + plain.headers);
      }

      [Test]
      [Description("A configured target that is not an absolute URL is reported as HM5780 and answers 404 rather than emitting a broken Location")]
      public void UnusableCalDavTargetIsReportedAndDoesNotRedirect()
      {
         try
         {
            // No scheme. Left unchecked this would go straight into a Location
            // header, and anything a client did with it would be undefined.
            WriteSetting("CalDavRedirectUrl", "calendar.example.test/dav/");
            _application.Reinitialize();

            (int status, string headers, string body) response = HttpGet("/.well-known/caldav", "example.test");

            Assert.AreEqual(404, response.status,
               "An unusable target must not produce a redirect. Headers: " + response.headers);

            Assert.IsNull(HeaderValue(response.headers, "Location"), "Headers: " + response.headers);

            // An empty setting is the shipped default and stays silent; a
            // non-empty one that cannot be used is a real misconfiguration and
            // is reported, so it can be found and corrected.
            CustomAsserts.AssertReportedError("HM5780");
         }
         finally
         {
            WriteSetting("CalDavRedirectUrl", "");
            LogHandler.DeleteErrorLog();
         }
      }

      // ------------------------------------------------------------ Autodiscover

      private const string AutodiscoverPath = "/autodiscover/autodiscover.xml";

      // Outlook's POX request for an address.
      private static string AutodiscoverRequest(string address)
      {
         return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n" +
            "<Autodiscover xmlns=\"http://schemas.microsoft.com/exchange/autodiscover/outlook/requestschema/2006\">" +
            "<Request><EMailAddress>" + address + "</EMailAddress>" +
            "<AcceptableResponseSchema>http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a</AcceptableResponseSchema>" +
            "</Request></Autodiscover>";
      }

      // Outlook's request as it arrives through a TLS-terminating proxy, with
      // or without credentials; overHttps false is the bare listener.
      private static (int status, string headers, string body) Autodiscover(string address, string authorization = null, bool overHttps = true)
      {
         string extra = (overHttps ? "X-Forwarded-Proto: https\r\n" : "") +
                        (authorization == null ? "" : "Authorization: " + authorization + "\r\n");

         return HttpPost(AutodiscoverPath, "example.test", AutodiscoverRequest(address), extra);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      // The <AlternativeMailbox> blocks of an answer, each as its text.
      private static List<string> AlternativeMailboxes(string xml)
      {
         var blocks = new List<string>();
         int position = 0;
         while (true)
         {
            int start = xml.IndexOf("<AlternativeMailbox>", position, StringComparison.Ordinal);
            if (start < 0)
               return blocks;

            int end = xml.IndexOf("</AlternativeMailbox>", start, StringComparison.Ordinal);
            Assert.Greater(end, start, "An unterminated AlternativeMailbox. XML: " + xml);

            blocks.Add(xml.Substring(start, end - start));
            position = end;
         }
      }

      // The text of the first <element> in the XML given.
      private static string Element(string xml, string element)
      {
         int start = xml.IndexOf("<" + element + ">", StringComparison.Ordinal);
         if (start < 0)
            return null;

         start += element.Length + 2;
         int end = xml.IndexOf("</" + element + ">", start, StringComparison.Ordinal);
         return end < start ? null : xml.Substring(start, end - start);
      }

      // The <Port> of the <Protocol> of the given type in the Autodiscover answer.
      private static int AutodiscoverPort(string xml, string type)
      {
         int typePosition = xml.IndexOf("<Type>" + type + "</Type>", StringComparison.Ordinal);
         Assert.Greater(typePosition, 0, "The Autodiscover answer has no " + type + " protocol. XML: " + xml);

         int portStart = xml.IndexOf("<Port>", typePosition, StringComparison.Ordinal) + 6;
         int portEnd = xml.IndexOf("</Port>", portStart, StringComparison.Ordinal);

         return int.Parse(xml.Substring(portStart, portEnd - portStart), CultureInfo.InvariantCulture);
      }

      // The owner opens one of its own folders to the delegate with the
      // rights named, through its own IMAP session: SETACL on a folder the
      // owner holds is the grant surface a person has.
      private static void ShareFolder(Account owner, string folder, Account delegateAccount, string rights)
      {
         var simulator = new ImapClientSimulator();
         Assert.IsTrue(simulator.ConnectAndLogon(owner.Address, "test"));
         // A folder shared twice already exists the second time; the grant
         // below fails if it does not exist for any other reason.
         simulator.CreateFolder(folder);
         Assert.IsTrue(simulator.SetACL(folder, delegateAccount.Address, rights), "granting " + rights + " on " + folder);
         simulator.Disconnect();
      }

      private static void Unshare(Account owner, string folder, Account delegateAccount)
      {
         var simulator = new ImapClientSimulator();
         Assert.IsTrue(simulator.ConnectAndLogon(owner.Address, "test"));
         Assert.IsTrue(simulator.DeleteACL(folder, delegateAccount.Address), "revoking on " + folder);
         simulator.Disconnect();
      }

      [Test]
      [Description("Outlook's POX Autodiscover answers the server's settings to anybody, in the 2006a shape, with the ports the other formats advertise and no delegation in it")]
      public void AutodiscoverAnswersTheSettingsToAnybody()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "someone@example.test", "test");

         // Over the bare listener, with no credentials: Outlook's first
         // request is exactly this, and the settings must come back to it.
         (int status, string headers, string body) response = Autodiscover("someone@example.test", null, false);

         Assert.AreEqual(200, response.status, "Body: " + response.body);
         StringAssert.StartsWith("text/xml", HeaderValue(response.headers, "Content-Type") ?? "", "Headers: " + response.headers);

         StringAssert.Contains("<Autodiscover xmlns=\"http://schemas.microsoft.com/exchange/autodiscover/responseschema/2006\">", response.body);
         StringAssert.Contains("<Response xmlns=\"http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a\">", response.body);
         Assert.AreEqual("email", Element(response.body, "AccountType"), response.body);
         Assert.AreEqual("settings", Element(response.body, "Action"), response.body);
         StringAssert.Contains("<LoginName>someone@example.test</LoginName>", response.body);
         StringAssert.Contains("<AuthRequired>on</AuthRequired>", response.body);

         // One source of truth, three formats: the ports Outlook is told are
         // the ports Thunderbird is told.
         (int status, string headers, string body) autoconfig = HttpGet("/mail/config-v1.1.xml", "example.test");
         Assert.AreEqual(200, autoconfig.status, "Body: " + autoconfig.body);
         Assert.AreEqual(AutoconfigPort(autoconfig.body, "imap"), AutodiscoverPort(response.body, "IMAP"), response.body);
         Assert.AreEqual(AutoconfigPort(autoconfig.body, "smtp"), AutodiscoverPort(response.body, "SMTP"), response.body);

         Assert.AreEqual(0, AlternativeMailboxes(response.body).Count,
            "Nobody has delegated anything to this address, and nobody asked as it. Body: " + response.body);

         // Not cacheable: the answer can differ by who asks.
         StringAssert.Contains("no-store", HeaderValue(response.headers, "Cache-Control") ?? "", "Headers: " + response.headers);

         // GET is accepted beside POST, as it always was.
         (int status, string headers, string body) get = HttpGet(AutodiscoverPath, "example.test");
         Assert.AreEqual(200, get.status, "Body: " + get.body);
         Assert.AreEqual("settings", Element(get.body, "Action"), get.body);
      }

      [Test]
      [Description("The mailboxes opened to an account are announced as AlternativeMailbox elements, XML-escaped, only to a request signed in as that account and asking about it; a bystander, an outsider and the unauthenticated get none; a folder that can only be looked up is not a mailbox; the public folders are not one; a revoked grant is gone")]
      public void AutodiscoverNamesDelegationsOnlyToTheAccountTheyWereGrantedTo()
      {
         Account tom = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "tom@example.test", "test");
         tom.PersonFirstName = "<Tom";
         tom.PersonLastName = "& Jerry>";
         tom.Save();

         Account plain = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "plain@example.test", "test");
         Account delegateAccount = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "delegate@example.test", "test");
         Account bystander = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bystander@example.test", "test");

         ShareFolder(tom, "Projects", delegateAccount, "lr");
         ShareFolder(plain, "Cases", delegateAccount, "lrs");

         // Lookup alone: the bystander can see that Cases exists and cannot
         // open it, which is not a mailbox anybody can use.
         var lookupOnly = new ImapClientSimulator();
         Assert.IsTrue(lookupOnly.ConnectAndLogon(plain.Address, "test"));
         Assert.IsTrue(lookupOnly.SetACL("Cases", bystander.Address, "l"));
         lookupOnly.Disconnect();

         // A public folder opened to the delegate: real, readable, and not a
         // mailbox - Outlook has no element for it and none is invented.
         var publicFolder = _settings.PublicFolders.Add("Notices");
         publicFolder.Save();
         var permission = publicFolder.Permissions.Add();
         permission.PermissionAccountID = delegateAccount.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionLookup, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.Save();

         // Unauthenticated, asking about the delegate: the settings and no more.
         (int status, string headers, string body) anonymous = Autodiscover(delegateAccount.Address);
         Assert.AreEqual(200, anonymous.status, "Body: " + anonymous.body);
         StringAssert.Contains("<LoginName>delegate@example.test</LoginName>", anonymous.body);
         Assert.AreEqual(0, AlternativeMailboxes(anonymous.body).Count,
            "Who delegates to whom is not public. Body: " + anonymous.body);

         // Signed in as the delegate, asking about the delegate: both owners,
         // in id order, each with the four elements Outlook reads.
         (int status, string headers, string body) mine = Autodiscover(delegateAccount.Address, Basic(delegateAccount.Address, "test"));
         Assert.AreEqual(200, mine.status, "Body: " + mine.body);
         StringAssert.Contains("<Action>settings</Action>", mine.body);

         List<string> mailboxes = AlternativeMailboxes(mine.body);
         Assert.AreEqual(2, mailboxes.Count, "Body: " + mine.body);

         Assert.AreEqual("Delegate", Element(mailboxes[0], "Type"), mine.body);
         Assert.AreEqual("&lt;Tom &amp; Jerry&gt;", Element(mailboxes[0], "DisplayName"),
            "The person name, XML-escaped. Body: " + mine.body);
         Assert.AreEqual("tom@example.test", Element(mailboxes[0], "SmtpAddress"), mine.body);
         Assert.AreEqual("tom@example.test", Element(mailboxes[0], "OwnerSmtpAddress"), mine.body);

         Assert.AreEqual("Delegate", Element(mailboxes[1], "Type"), mine.body);
         Assert.AreEqual("plain@example.test", Element(mailboxes[1], "DisplayName"),
            "An owner with no person name is named by the address. Body: " + mine.body);
         Assert.AreEqual("plain@example.test", Element(mailboxes[1], "SmtpAddress"), mine.body);
         Assert.AreEqual("plain@example.test", Element(mailboxes[1], "OwnerSmtpAddress"), mine.body);

         Assert.IsFalse(mine.body.Contains("Notices"), "The public folder is not a mailbox. Body: " + mine.body);
         Assert.IsFalse(mine.body.Contains(_settings.IMAPPublicFolderName), "The public folder is not a mailbox. Body: " + mine.body);

         // The elements sit inside <Account>, after the protocols, where the
         // 2006a schema places them.
         int lastProtocol = mine.body.LastIndexOf("</Protocol>", StringComparison.Ordinal);
         int firstMailbox = mine.body.IndexOf("<AlternativeMailbox>", StringComparison.Ordinal);
         int accountEnd = mine.body.IndexOf("</Account>", StringComparison.Ordinal);
         Assert.IsTrue(lastProtocol > 0 && firstMailbox > lastProtocol && accountEnd > firstMailbox, "Body: " + mine.body);

         // The delegate's own address is what it signed in as; asking about
         // the owner's address while signed in as the delegate is asking about
         // somebody else, and gets today's answer.
         (int status, string headers, string body) somebodyElse = Autodiscover(tom.Address, Basic(delegateAccount.Address, "test"));
         Assert.AreEqual(200, somebodyElse.status, "Body: " + somebodyElse.body);
         Assert.AreEqual(0, AlternativeMailboxes(somebodyElse.body).Count, "Body: " + somebodyElse.body);

         // The bystander, signed in as itself and asking about the delegate.
         (int status, string headers, string body) nosy = Autodiscover(delegateAccount.Address, Basic(bystander.Address, "test"));
         Assert.AreEqual(200, nosy.status, "Body: " + nosy.body);
         Assert.AreEqual(0, AlternativeMailboxes(nosy.body).Count, "Body: " + nosy.body);

         // The bystander asking about itself: a lookup-only grant is no mailbox.
         (int status, string headers, string body) lookupOnlyAnswer = Autodiscover(bystander.Address, Basic(bystander.Address, "test"));
         Assert.AreEqual(200, lookupOnlyAnswer.status, "Body: " + lookupOnlyAnswer.body);
         Assert.AreEqual(0, AlternativeMailboxes(lookupOnlyAnswer.body).Count,
            "A folder that can be seen but not opened is not a mailbox. Body: " + lookupOnlyAnswer.body);

         // The address is matched without regard to case, as addresses are.
         (int status, string headers, string body) upper = Autodiscover("Delegate@Example.Test", Basic(delegateAccount.Address, "test"));
         Assert.AreEqual(200, upper.status, "Body: " + upper.body);
         Assert.AreEqual(2, AlternativeMailboxes(upper.body).Count, "Body: " + upper.body);

         // Revoked: gone from the answer at once, with nothing cached.
         Unshare(tom, "Projects", delegateAccount);

         (int status, string headers, string body) afterRevocation = Autodiscover(delegateAccount.Address, Basic(delegateAccount.Address, "test"));
         Assert.AreEqual(200, afterRevocation.status, "Body: " + afterRevocation.body);
         List<string> remaining = AlternativeMailboxes(afterRevocation.body);
         Assert.AreEqual(1, remaining.Count, "Body: " + afterRevocation.body);
         Assert.AreEqual("plain@example.test", Element(remaining[0], "SmtpAddress"), afterRevocation.body);
         Assert.IsFalse(afterRevocation.body.Contains("tom@example.test"), "Body: " + afterRevocation.body);
      }

      [Test]
      [Description("A wrong password is refused as the DAV logon refuses it - 401 with the Basic challenge - and counts against the per-name lockout the same way, so the correct password is then refused with the same answer while another account is untouched")]
      public void AutodiscoverRefusesAWrongPasswordAsTheDavLogonDoesAndTheNameLocks()
      {
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "lockowner@example.test", "test");
         Account victim = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "lockvictim@example.test", "test");
         Account neighbour = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "lockneighbour@example.test", "test");

         ShareFolder(owner, "Shared", victim, "lr");
         ShareFolder(owner, "Shared", neighbour, "lr");

         // The ordinary refusal, measured before any lock: the same status,
         // the same challenge, no settings and no delegation in the body. The
         // CalDav fixture pins the 401 and the challenge for the DAV logon;
         // this is the same refusal from the same function.
         (int status, string headers, string body) wrong = Autodiscover(victim.Address, Basic(victim.Address, "not-the-password"));
         Assert.AreEqual(401, wrong.status, "Body: " + wrong.body);
         StringAssert.Contains("Basic", HeaderValue(wrong.headers, "WWW-Authenticate") ?? "", "Headers: " + wrong.headers);
         Assert.IsFalse(wrong.body.Contains("<Autodiscover"), "No settings with a refused credential. Body: " + wrong.body);
         Assert.AreEqual(0, AlternativeMailboxes(wrong.body).Count, wrong.body);

         try
         {
            // Three failures lock the name, exactly as they do over POP3 in
            // Security/AccountLockoutTests: the guesses reach the same
            // AccountLogon. The CalDav fixture does not prove this half, so it
            // is proved the way the lockout fixture proves it.
            IniFileSetting.Write("AccountLockoutThreshold", "3");
            _application.Reinitialize();

            for (int i = 0; i < 3; i++)
            {
               (int status, string headers, string body) guess = Autodiscover(victim.Address, Basic(victim.Address, "wrong" + i));
               Assert.AreEqual(401, guess.status, "Guess " + i + ". Body: " + guess.body);
            }

            // The correct password, refused with the ordinary answer: nothing
            // in it says the account exists and has just been locked.
            (int status, string headers, string body) locked = Autodiscover(victim.Address, Basic(victim.Address, "test"));
            Assert.AreEqual(wrong.status, locked.status, "Body: " + locked.body);
            Assert.AreEqual(HeaderValue(wrong.headers, "WWW-Authenticate"), HeaderValue(locked.headers, "WWW-Authenticate"), "Headers: " + locked.headers);
            Assert.AreEqual(wrong.body, locked.body, "The refusal for a locked name must be the ordinary refusal.");

            // Per name: the neighbour, delegated to by the same owner, is untouched.
            (int status, string headers, string body) neighbourAnswer = Autodiscover(neighbour.Address, Basic(neighbour.Address, "test"));
            Assert.AreEqual(200, neighbourAnswer.status, "Body: " + neighbourAnswer.body);
            Assert.AreEqual(1, AlternativeMailboxes(neighbourAnswer.body).Count, "Body: " + neighbourAnswer.body);
         }
         finally
         {
            IniFileSetting.Delete("AccountLockoutThreshold");
            _application.Reinitialize();

            // The lockout store lives in the service process, which did not
            // restart: with the threshold back at its default a successful
            // logon erases the entry, and this one proves the name is free.
            (int status, string headers, string body) freed = Autodiscover(victim.Address, Basic(victim.Address, "test"));
            Assert.AreEqual(200, freed.status, "Body: " + freed.body);
            Assert.AreEqual(1, AlternativeMailboxes(freed.body).Count, "Body: " + freed.body);
         }
      }

      [Test]
      [Description("A credential offered over plain HTTP is refused before it is looked at, with the reason, as CardDAV refuses it; the settings without a credential are still served over plain HTTP")]
      public void AutodiscoverRefusesACredentialOverPlainHttp()
      {
         Account owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "plainowner@example.test", "test");
         Account delegateAccount = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "plaindelegate@example.test", "test");
         ShareFolder(owner, "Shared", delegateAccount, "lr");

         (int status, string headers, string body) refused = Autodiscover(delegateAccount.Address, Basic(delegateAccount.Address, "test"), false);
         Assert.AreEqual(403, refused.status, "Body: " + refused.body);
         StringAssert.Contains("HTTPS", refused.body);
         StringAssert.Contains("X-Forwarded-Proto", refused.body);
         Assert.IsFalse(refused.body.Contains("<Autodiscover"), "Body: " + refused.body);
         Assert.IsFalse(refused.body.Contains(owner.Address), "Body: " + refused.body);

         // A wrong password over plain HTTP gets the same 403, not a 401: it
         // was never checked, so it is neither confirmed nor counted.
         (int status, string headers, string body) unverified = Autodiscover(delegateAccount.Address, Basic(delegateAccount.Address, "not-the-password"), false);
         Assert.AreEqual(403, unverified.status, "Body: " + unverified.body);

         // And through the proxy, the same credential is honoured.
         (int status, string headers, string body) honoured = Autodiscover(delegateAccount.Address, Basic(delegateAccount.Address, "test"));
         Assert.AreEqual(200, honoured.status, "Body: " + honoured.body);
         Assert.AreEqual(1, AlternativeMailboxes(honoured.body).Count, "Body: " + honoured.body);
      }
   }
}
