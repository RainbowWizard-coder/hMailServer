// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Brand logos (BIMI) from the mail to the answer: a message this server
   ///    judged dmarc=pass carries the brand in the listing and in the message,
   ///    and one that failed carries none; the logo route then answers for each
   ///    brand by what DNS publishes for it - a record whose logo lives at an
   ///    address that is not public is refused at the fetch, a domain whose
   ///    policy is p=none is refused for its policy, a domain with no record and
   ///    one whose record declines are told apart - and the switch turns both
   ///    halves off. The SVG Tiny PS check a fetched logo must pass is proved at
   ///    the function, in BrandLogoTester, which Utilities.RunTestSuite runs:
   ///    nothing on the bench can serve a logo from a public address.
   /// </summary>
   [TestFixture]
   public class RestApiBrandLogos : TestFixtureBase
   {
      private static int RestPort = 9613;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Brand-Logo-Passw0rd!";

      private static string DmarcAndSpf(FakeDnsServer zone, string domain, string policy, string spf)
      {
         zone.WithTxt(domain, spf)
             .WithTxt("_dmarc." + domain, "v=DMARC1; p=" + policy);
         return domain;
      }

      [Test]
      [Description("A dmarc=pass message carries its brand in the listing and the message and a dmarc=fail one does not; the logo route refuses a private logo address at the fetch, a p=none domain for its policy, and tells no record from a declining one; the switch removes the brand and refuses the route.")]
      public void ABrandIsNamedOnlyForMailThatPassedAndItsLogoOnlyWhereThePolicyEnforces()
      {
         string domainName = _domain.Name;
         string address = "brand-reader@" + domainName;

         _settings.SetAdministratorPassword(AdminPassword);
         var antiSpam = _application.Settings.AntiSpam;
         antiSpam.SpamMarkThreshold = 100;
         antiSpam.SpamDeleteThreshold = 1000;
         antiSpam.DMARCEnabled = true;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, UserPassword);

         FakeDnsServer zone = SuiteDns.Zone;
         DmarcAndSpf(zone, "brand.logo-test.test", "reject", "v=spf1 ip4:127.0.0.1 -all");
         zone.WithTxt("default._bimi.brand.logo-test.test", "v=BIMI1; l=https://10.255.255.1/logo.svg");
         DmarcAndSpf(zone, "weak.logo-test.test", "none", "v=spf1 ip4:127.0.0.1 -all");
         zone.WithTxt("default._bimi.weak.logo-test.test", "v=BIMI1; l=https://weak.logo-test.test/logo.svg");
         DmarcAndSpf(zone, "plain.logo-test.test", "quarantine", "v=spf1 ip4:127.0.0.1 -all");
         DmarcAndSpf(zone, "declined.logo-test.test", "reject", "v=spf1 ip4:127.0.0.1 -all");
         zone.WithTxt("default._bimi.declined.logo-test.test", "v=BIMI1; l=;");
         DmarcAndSpf(zone, "spoof.logo-test.test", "none", "v=spf1 -all");

         try
         {
            ServerIniFile.SetSetting("AuthenticationResultsEnabled", "1");
            ServerIniFile.SetSetting("DNSQueryTimeout", "5");
            RestPort = RestListener.Start(RestPort);
            RestartServerAndReacquireCom();

            foreach (string sender in new[] { "brand", "weak", "plain", "declined", "spoof" })
            {
               SmtpClientSimulator.StaticSendRaw("news@" + sender + ".logo-test.test", address,
                  "From: <news@" + sender + ".logo-test.test>\r\nTo: <" + address + ">\r\nSubject: From " + sender + "\r\n" +
                  "Date: Thu, 17 Sep 2026 10:00:00 +0000\r\n\r\nA letter.\r\n");
            }
            ImapClientSimulator.AssertMessageCount(address, UserPassword, "Inbox", 5);

            string user = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + UserPassword));
            string admin = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));

            (int status, string body) tree = Send("GET", "/api/v1/me/folders", user, null);
            Assert.AreEqual(200, tree.status, tree.body);
            string inbox = NumberBefore(tree.body, "\"path\":\"INBOX\"");
            (int status, string body) listing = Send("GET", "/api/v1/me/folders/" + inbox + "/messages", user, null);
            Assert.AreEqual(200, listing.status, listing.body);

            foreach (string passed in new[] { "brand", "weak", "plain", "declined" })
               StringAssert.Contains("\"brand\":\"" + passed + ".logo-test.test\"", EntryWithSubject(listing.body, "From " + passed),
                  passed + " passed DMARC, so the listing names its brand.");
            StringAssert.DoesNotContain("\"brand\"", EntryWithSubject(listing.body, "From spoof"),
               "SPF failed for spoof, so DMARC failed and there is no brand to name.");

            string brandId = NumberBefore(listing.body, "\"subject\":\"From brand\"");
            (int status, string body) message = Send("GET", "/api/v1/me/messages/" + brandId, user, null);
            Assert.AreEqual(200, message.status, message.body);
            StringAssert.Contains("\"brand\":\"brand.logo-test.test\"", message.body);

            AssertOutcome(Send("GET", "/api/v1/me/brand-logo?domain=brand.logo-test.test", user, null), 404, "fetch_failed");
            AssertOutcome(Send("GET", "/api/v1/me/brand-logo?domain=weak.logo-test.test", user, null), 404, "weak_policy");
            AssertOutcome(Send("GET", "/api/v1/me/brand-logo?domain=plain.logo-test.test", user, null), 404, "no_record");
            AssertOutcome(Send("GET", "/api/v1/me/brand-logo?domain=declined.logo-test.test", user, null), 404, "declined");
            AssertOutcome(Send("GET", "/api/v1/me/brand-logo?domain=not%20a%20domain", user, null), 400, "bad_domain");
            Assert.AreEqual(401, Send("GET", "/api/v1/me/brand-logo?domain=brand.logo-test.test", null, null).status,
               "The route is the reader's, and answers nobody else.");

            (int status, string body) off = Send("PUT", "/api/v1/settings", admin, "{\"brand_logos_enabled\":false}");
            Assert.AreEqual(200, off.status, off.body);
            (int status, string body) listingOff = Send("GET", "/api/v1/me/folders/" + inbox + "/messages", user, null);
            StringAssert.DoesNotContain("\"brand\"", listingOff.body, "Switched off, no brand is named.");
            AssertOutcome(Send("GET", "/api/v1/me/brand-logo?domain=brand.logo-test.test", user, null), 404, "switched_off");
         }
         finally
         {
            Send("PUT", "/api/v1/settings", "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)), "{\"brand_logos_enabled\":true}");
            SuiteDns.Reset();
            ServerIniFile.SetSetting("AuthenticationResultsEnabled", null);
            ServerIniFile.SetSetting("DNSQueryTimeout", null);
            RestListener.Stop();
            RestartServerAndReacquireCom();
         }
      }

      private static void AssertOutcome((int status, string body) answer, int status, string outcome)
      {
         Assert.AreEqual(status, answer.status, answer.body);
         StringAssert.Contains("\"outcome\":\"" + outcome + "\"", answer.body);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string body)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, body);
      }

      private static string NumberBefore(string body, string marker)
      {
         int at = body.IndexOf(marker, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "Missing " + marker + " in: " + body);
         int idAt = body.LastIndexOf("\"id\":", at, StringComparison.Ordinal);
         Assert.IsTrue(idAt >= 0, "No id before " + marker + " in: " + body);
         int start = idAt + 5, end = start;
         while (end < body.Length && char.IsDigit(body[end]))
            end++;
         return body.Substring(start, end - start);
      }

      private static string EntryWithSubject(string listing, string subject)
      {
         int at = listing.IndexOf("\"subject\":\"" + subject + "\"", StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "No entry for " + subject + " in: " + listing);
         int start = listing.LastIndexOf('{', at);
         int depth = 0;
         for (int i = start; i < listing.Length; i++)
         {
            if (listing[i] == '{')
               depth++;
            else if (listing[i] == '}' && --depth == 0)
               return listing.Substring(start, i + 1 - start);
         }
         Assert.Fail("Unterminated entry for " + subject);
         return null;
      }
   }
}
