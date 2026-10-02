// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    The never-ban list by reverse name (GitLab work item #56): a provider named
   ///    by the reverse names of its hosts is never auto-banned, as long as the name
   ///    resolves back to the address - and nothing else is.
   ///
   ///    What these pin, each beside the case that must come out the other way:
   ///
   ///      * the list over REST: a row added, listed, changed and removed, and what
   ///        it may not hold (a public suffix, an address, a network, a star, a pair
   ///        already listed) refused in words;
   ///      * the check: a covered, forward-confirmed name spares its address; a
   ///        covered name that does not resolve back, a name no row covers and an
   ///        address with no reverse name do not;
   ///      * the ban itself, through two of its three writers - a spam trap hit and
   ///        a run of failed logons: a listed provider is left with no auto-ban
   ///        range, an unlisted one and one whose reverse name is forged are banned,
   ///        and a row removed spares nobody from the next hit on.
   ///
   ///    The suite reaches the server from 127.0.0.1, which must never be banned
   ///    here, and whose reverse name the resolver may answer itself. So every
   ///    session is moved to an address of 198.51.100.0/24 (RFC 5737) with XCLIENT,
   ///    from a trusted 127.0.0.1, and the suite's own DNS zone answers the reverse
   ///    and forward names of those addresses. An IP range for the /24 that runs the
   ///    spam tests is made for the fixture, so that a trap hit is one that may be
   ///    banned at all - which the negative controls prove it is.
   ///
   ///    Everything is made and taken back over REST and raw sockets, with no COM,
   ///    which is why the Linux suite links this fixture.
   /// </summary>
   [TestFixture]
   public class AutoBanNeverBanNames : TestFixtureBase
   {
      private static int RestPort = 9643;
      private const string AdminPassword = "testar";

      private const string Net = "198.51.100.";
      private const string Provider = "nbn-provider.test";
      private const string TrapLocalPart = "never-was-real-nbn";
      private const string Sender = "harvester@nbn-spammer.example";
      private const string RangeName = "never-ban names test net";
      private const string NotePrefix = "nbn-fixture";

      private long banMinutesBefore_;
      private bool autoBanBefore_;
      private long maxAttemptsBefore_;
      private long minutesToBanBefore_;

      [SetUp]
      public void StartRestApiAndXclient()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         IniFileSetting.Write("SMTPXClientEnabled", "1");
         IniFileSetting.Write("SMTPXClientTrustedIPs", "127.0.0.1");

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         Dictionary<string, object> settings = Settings();
         banMinutesBefore_ = MiniJson.Number(settings, "spam_trap_ban_minutes");
         autoBanBefore_ = MiniJson.Flag(settings, "auto_ban_on_logon_failure");
         maxAttemptsBefore_ = MiniJson.Number(settings, "max_invalid_logon_attempts");
         minutesToBanBefore_ = MiniJson.Number(settings, "minutes_to_ban");

         PutSettings("{\"spam_trap_ban_minutes\":5}");

         TakeBackOurs();

         // The fixture's net: SMTP and delivery to this server's domains allowed,
         // the spam tests run - so a trap hit from it is one that may be banned.
         (int status, string body) range = Http("POST", "/api/v1/ipranges",
            "{\"name\":\"" + RangeName + "\",\"lower\":\"" + Net + "0\",\"upper\":\"" + Net + "255\",\"priority\":20," +
            "\"allow_smtp\":true,\"allow_imap\":false,\"allow_pop3\":false,\"deliver_remote_to_local\":true," +
            "\"require_auth_remote_to_local\":false,\"require_tls_for_auth\":false,\"spam_protection\":true,\"virus_protection\":false}");
         ClassicAssert.AreEqual(201, range.status, range.body);
      }

      [TearDown]
      public void TakeItBack()
      {
         try
         {
            TakeBackOurs();

            PutSettings("{\"spam_trap_ban_minutes\":" + banMinutesBefore_ +
                        ",\"auto_ban_on_logon_failure\":" + (autoBanBefore_ ? "true" : "false") +
                        ",\"max_invalid_logon_attempts\":" + maxAttemptsBefore_ +
                        ",\"minutes_to_ban\":" + minutesToBanBefore_ + "}");
         }
         finally
         {
            SuiteDns.Reset();
            IniFileSetting.Delete("SMTPXClientEnabled");
            IniFileSetting.Delete("SMTPXClientTrustedIPs");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ the list

      [Test]
      [Description("A name is added, listed, changed and removed over REST, and a second removal is a 404.")]
      public void ANameIsAddedListedChangedAndRemoved()
      {
         (int status, string body) created = Http("POST", "/api/v1/never-ban-names", "{\"pattern\":\" .NBN-Provider.TEST. \",\"note\":\"" + NotePrefix + " outbound\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);

         Dictionary<string, object> row = MiniJson.Object(created.body);
         long id = MiniJson.Number(row, "id");
         ClassicAssert.IsTrue(id > 0, created.body);
         ClassicAssert.AreEqual("." + Provider, MiniJson.Text(row, "pattern"), "The pattern is stored as the list compares it: trimmed, lower-cased, no trailing dot.");
         ClassicAssert.IsTrue(MiniJson.Text(row, "created").Length > 0, created.body);

         ClassicAssert.IsTrue(Ours().Any(r => MiniJson.Number(r, "id") == id), "The new row is not listed.");

         (int status, string body) changed = Http("PUT", "/api/v1/never-ban-names/" + id, "{\"pattern\":\"mail." + Provider + "\"}");
         ClassicAssert.AreEqual(200, changed.status, changed.body);
         ClassicAssert.AreEqual("mail." + Provider, MiniJson.Text(MiniJson.Object(changed.body), "pattern"));
         ClassicAssert.AreEqual(NotePrefix + " outbound", MiniJson.Text(MiniJson.Object(changed.body), "note"), "A field left out of the PUT kept its value.");

         (int status, string body) removed = Http("DELETE", "/api/v1/never-ban-names/" + id);
         ClassicAssert.AreEqual(200, removed.status, removed.body);
         ClassicAssert.IsFalse(Ours().Any(r => MiniJson.Number(r, "id") == id), "The removed row is still listed.");

         (int status, string body) again = Http("DELETE", "/api/v1/never-ban-names/" + id);
         ClassicAssert.AreEqual(404, again.status, again.body);
      }

      [Test]
      [Description("A row added and a row removed are on the audit trail as a never-ban name, under its pattern - the delete included, whose statement carries only the row's id.")]
      public void TheAuditTrailNamesARowByItsPattern()
      {
         string pattern = "audit." + Provider;
         long id = Add(pattern);

         (int status, string body) removed = Http("DELETE", "/api/v1/never-ban-names/" + id);
         ClassicAssert.AreEqual(200, removed.status, removed.body);

         // Newest first, and shared with every fixture before this one, so the
         // rows are read by what they say rather than counted.
         (int status, string body) trail = Http("GET", "/api/v1/audit?object_type=never-ban+name&limit=20");
         ClassicAssert.AreEqual(200, trail.status, trail.body);

         List<Dictionary<string, object>> rows = MiniJson.List(MiniJson.Object(trail.body), "entries").Cast<Dictionary<string, object>>()
            .Where(r => MiniJson.Text(r, "object") == pattern).ToList();

         ClassicAssert.IsTrue(rows.Any(r => MiniJson.Text(r, "action") == "created"), "Adding the row left no 'created' row named by its pattern: " + trail.body);
         ClassicAssert.IsTrue(rows.Any(r => MiniJson.Text(r, "action") == "deleted"), "Removing the row left no 'deleted' row named by its pattern: " + trail.body);
         ClassicAssert.IsTrue(rows.All(r => MiniJson.Text(r, "object_type") == "never-ban name"), trail.body);
      }

      [Test]
      [Description("What the list may not hold is refused in words, and nothing is added: a public suffix, an address, a network, a star, an email address, a pair already listed, an unknown field.")]
      public void WhatTheListMayNotHoldIsRefused()
      {
         Add("." + Provider);
         int before = Ours().Count;

         var refusals = new[]
         {
            new { body = "{\"pattern\":\".com\"}", says = "public suffix" },
            new { body = "{\"pattern\":\"co.uk\"}", says = "public suffix" },
            new { body = "{\"pattern\":\".github.io\"}", says = "public suffix" },
            new { body = "{\"pattern\":\"localhost\"}", says = "public suffix" },
            new { body = "{\"pattern\":\"*.example.net\"}", says = ".example.net" },
            new { body = "{\"pattern\":\"192.0.2.1\"}", says = "AutoBanNeverBan" },
            new { body = "{\"pattern\":\"198.51.100.0/24\"}", says = "AutoBanNeverBan" },
            new { body = "{\"pattern\":\"2001:db8::1\"}", says = "AutoBanNeverBan" },
            new { body = "{\"pattern\":\"postmaster@example.net\"}", says = "not an address" },
            new { body = "{\"pattern\":\"\"}", says = "required" },
            new { body = "{\"note\":\"no pattern\"}", says = "required" },
            new { body = "{\"pattern\":\"." + Provider + "\"}", says = "already has" },
            new { body = "{\"pattern\":\"x." + Provider + "\",\"color\":\"red\"}", says = "unknown field" },
            new { body = "{\"pattern\":7}", says = "must be a string" },
         };

         foreach (var refusal in refusals)
         {
            (int status, string body) answer = Http("POST", "/api/v1/never-ban-names", refusal.body);
            ClassicAssert.AreEqual(400, answer.status, refusal.body + " -> " + answer.body);
            StringAssert.Contains(refusal.says, answer.body, refusal.body + " -> " + answer.body);
         }

         ClassicAssert.AreEqual(before, Ours().Count, "A refused body added a row.");

         // The negative control: a registrable domain under a public suffix is a
         // name the list may hold.
         Add("example.co.uk");
         ClassicAssert.AreEqual(before + 1, Ours().Count, "A registrable domain under co.uk was refused.");
      }

      // ----------------------------------------------------------------- the check

      [Test]
      [Description("The check spares an address whose reverse name is covered and resolves back to it, and only that: a forged name, an uncovered name and no name at all are not spared; text that is not an address is refused.")]
      public void TheCheckSparesAForwardConfirmedNameAndNothingElse()
      {
         Add("." + Provider);

         Reverse(81, "mx81." + Provider, Net + "81");
         Reverse(82, "mx82." + Provider, "203.0.113.82");
         Reverse(83, "mx83.unlisted-isp.test", Net + "83");

         Dictionary<string, object> spared = Check(Net + "81");
         ClassicAssert.IsTrue(MiniJson.Flag(spared, "spared"), Describe(spared));
         ClassicAssert.AreEqual("mx81." + Provider, MiniJson.Text(spared, "name"), Describe(spared));
         ClassicAssert.AreEqual("." + Provider, MiniJson.Text(spared, "pattern"), Describe(spared));

         Dictionary<string, object> forged = Check(Net + "82");
         ClassicAssert.IsFalse(MiniJson.Flag(forged, "spared"), "A reverse name that does not resolve back spared its address: " + Describe(forged));
         var forgedName = (Dictionary<string, object>) MiniJson.List(forged, "reverse_names").Single();
         ClassicAssert.AreEqual("." + Provider, MiniJson.Text(forgedName, "pattern"), Describe(forged));
         ClassicAssert.IsFalse(MiniJson.Flag(forgedName, "resolves_back"), Describe(forged));

         Dictionary<string, object> uncovered = Check(Net + "83");
         ClassicAssert.IsFalse(MiniJson.Flag(uncovered, "spared"), Describe(uncovered));
         var uncoveredName = (Dictionary<string, object>) MiniJson.List(uncovered, "reverse_names").Single();
         ClassicAssert.IsNull(MiniJson.At(uncoveredName, "pattern"), Describe(uncovered));
         ClassicAssert.IsTrue(MiniJson.Flag(uncoveredName, "resolves_back"), "The check does not say that a row covering the name would work: " + Describe(uncovered));

         Dictionary<string, object> nameless = Check(Net + "84");
         ClassicAssert.IsFalse(MiniJson.Flag(nameless, "spared"), Describe(nameless));
         ClassicAssert.AreEqual(0, MiniJson.List(nameless, "reverse_names").Count, Describe(nameless));

         (int status, string body) refused = Http("POST", "/api/v1/never-ban-names/check", "{\"address\":\"not an address\"}");
         ClassicAssert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("IPv4 or IPv6", refused.body, refused.body);
      }

      // ------------------------------------------------------------------- the ban

      [Test]
      [Description("A spam trap hit from a listed provider, whose reverse name resolves back, leaves no auto-ban range.")]
      public void ASpamTrapHitFromAListedProviderLeavesNoBan()
      {
         Add("." + Provider);
         DeclareTrap();
         Reverse(77, "mx77." + Provider, Net + "77");

         string reply = TrapHitFrom(Net + "77");
         StringAssert.Contains("550", reply, reply);

         ClassicAssert.IsTrue(NoBanWithin(Net + "77", TimeSpan.FromSeconds(15)),
            "An address the never-ban list covers, whose reverse name resolves back to it, is still auto-banned 15 seconds after the hit.");
      }

      [Test]
      [Description("The negative control: the same hit from a provider the list does not cover is banned, and stays banned.")]
      public void ASpamTrapHitFromAProviderNotListedIsBanned()
      {
         Add("." + Provider);
         DeclareTrap();
         Reverse(78, "mx78.unlisted-isp.test", Net + "78");

         TrapHitFrom(Net + "78");

         ClassicAssert.IsTrue(StillBannedAfter(Net + "78", TimeSpan.FromSeconds(4)),
            "A trap hit from an address the list does not cover was not banned, or its ban was lifted - so the test of the listed provider proves nothing.");
      }

      [Test]
      [Description("A reverse name the list covers but that does not resolve back to the address is banned: anybody who holds an address can write its reverse name.")]
      public void AReverseNameThatDoesNotResolveBackIsBanned()
      {
         Add("." + Provider);
         DeclareTrap();
         Reverse(79, "mx79." + Provider, "203.0.113.79");

         TrapHitFrom(Net + "79");

         ClassicAssert.IsTrue(StillBannedAfter(Net + "79", TimeSpan.FromSeconds(4)),
            "A forged reverse name - covered by the list, resolving elsewhere - spared its address from the auto-ban.");
      }

      [Test]
      [Description("A name taken off the list spares nobody from the next hit on, even an address it spared a moment before.")]
      public void ARemovedNameSparesNobody()
      {
         long id = Add("." + Provider);
         DeclareTrap();
         Reverse(86, "mx86." + Provider, Net + "86");

         TrapHitFrom(Net + "86");
         ClassicAssert.IsTrue(NoBanWithin(Net + "86", TimeSpan.FromSeconds(15)), "The listed provider was banned, so the rest of this test means nothing.");

         (int status, string body) removed = Http("DELETE", "/api/v1/never-ban-names/" + id);
         ClassicAssert.AreEqual(200, removed.status, removed.body);

         TrapHitFrom(Net + "86");
         ClassicAssert.IsTrue(StillBannedAfter(Net + "86", TimeSpan.FromSeconds(4)),
            "An address spared by a row that has since been removed was spared again: the verdict outlived the row.");
      }

      [Test]
      [Description("Failed logons from a listed provider are not counted and never ban it; the same failures from an unlisted address ban it.")]
      public void FailedLogonsFromAListedProviderAreNotBanned()
      {
         Add("." + Provider);
         PutSettings("{\"auto_ban_on_logon_failure\":true,\"max_invalid_logon_attempts\":2,\"minutes_to_ban\":5}");

         Reverse(80, "mx80." + Provider, Net + "80");
         Reverse(85, "mx85.unlisted-isp.test", Net + "85");

         for (int i = 0; i < 3; i++)
            FailedLogonFrom(Net + "80");

         ClassicAssert.IsTrue(NoBanWithin(Net + "80", TimeSpan.FromSeconds(15)),
            "A listed provider was auto-banned for failed logons.");

         for (int i = 0; i < 3; i++)
            FailedLogonFrom(Net + "85");

         ClassicAssert.IsTrue(StillBannedAfter(Net + "85", TimeSpan.FromSeconds(4)),
            "The negative control: three failed logons from an unlisted address did not ban it at a limit of two.");
      }

      [Test]
      [Description("The OpenAPI document the Deck builds its forms from carries the three paths.")]
      public void TheOpenApiDocumentCarriesTheRoutes()
      {
         (int status, string body) document = Http("GET", "/api/v1/openapi.json");
         ClassicAssert.AreEqual(200, document.status, document.body);
         ClassicAssert.IsNotNull(MiniJson.Object(document.body), "The OpenAPI document is not a JSON object.");

         foreach (string path in new[] { "/api/v1/never-ban-names", "/api/v1/never-ban-names/{id}", "/api/v1/never-ban-names/check" })
            StringAssert.Contains("\"" + path + "\"", document.body, path + " is not in the OpenAPI document.");
      }

      // ------------------------------------------------------------------- helpers

      /// <summary>A PTR for 198.51.100.n naming host, and an A record for host.</summary>
      private static void Reverse(int lastOctet, string host, string forwardAddress)
      {
         SuiteDns.Zone.WithRaw(lastOctet + ".100.51.198.in-addr.arpa", 12, DnsWire.Name(host));
         SuiteDns.Zone.WithA(host, forwardAddress);
      }

      private static long Add(string pattern)
      {
         (int status, string body) created = Http("POST", "/api/v1/never-ban-names", "{\"pattern\":\"" + pattern + "\",\"note\":\"" + NotePrefix + "\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);
         return MiniJson.Number(MiniJson.Object(created.body), "id");
      }

      private static Dictionary<string, object> Check(string address)
      {
         (int status, string body) answer = Http("POST", "/api/v1/never-ban-names/check", "{\"address\":\"" + address + "\"}");
         ClassicAssert.AreEqual(200, answer.status, answer.body);
         return MiniJson.Object(answer.body);
      }

      private static string Describe(Dictionary<string, object> verdict)
      {
         return MiniJson.Text(verdict, "reason") + " / " + string.Join(", ", MiniJson.List(verdict, "reverse_names").Select(n => MiniJson.Text((Dictionary<string, object>) n, "name")));
      }

      /// <summary>The rows this fixture made, by the note it gives them.</summary>
      private static List<Dictionary<string, object>> Ours()
      {
         (int status, string body) = Http("GET", "/api/v1/never-ban-names");
         ClassicAssert.AreEqual(200, status, body);
         return ((List<object>) MiniJson.Parse(body)).Cast<Dictionary<string, object>>()
            .Where(r => MiniJson.Text(r, "note").StartsWith(NotePrefix, StringComparison.Ordinal)).ToList();
      }

      private void DeclareTrap()
      {
         (int status, string body) created = Http("POST", "/api/v1/domains/" + _domain.Name + "/spam-traps", "{\"address\":\"" + TrapLocalPart + "\"}");
         ClassicAssert.IsTrue(created.status == 201 || created.status == 200, created.body);
      }

      /// <summary>One SMTP session moved to the address by XCLIENT, addressing the trap.</summary>
      private string TrapHitFrom(string address)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
            socket.ReadUntil("220");

            socket.Send("EHLO upstream.nbn.test\r\n");
            StringAssert.Contains("XCLIENT", socket.ReadUntil("250 "), "XCLIENT is not offered to 127.0.0.1.");

            socket.Send("XCLIENT ADDR=" + address + "\r\n");
            StringAssert.StartsWith("220", socket.ReadUntil("\r\n"), "XCLIENT was refused.");

            socket.Send("EHLO client.nbn.test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<" + Sender + ">\r\n");
            socket.ReadUntil("250");

            socket.Send("RCPT TO:<" + TrapLocalPart + "@" + _domain.Name + ">\r\n");
            string reply = socket.ReadUntil("\r\n");

            socket.Send("QUIT\r\n");
            return reply;
         }
      }

      /// <summary>One SMTP session moved to the address, failing one AUTH LOGIN.</summary>
      private void FailedLogonFrom(string address)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
            socket.ReadUntil("220");

            socket.Send("EHLO upstream.nbn.test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("XCLIENT ADDR=" + address + "\r\n");
            string greeting = socket.ReadUntil("\r\n");

            // An address already banned is refused here, at the fresh greeting:
            // that is the ban working, and the caller judges it by the ranges.
            if (!greeting.StartsWith("220", StringComparison.Ordinal))
               return;

            socket.Send("EHLO client.nbn.test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("AUTH LOGIN\r\n");
            socket.ReadUntil("334");
            socket.Send(Convert.ToBase64String(Encoding.ASCII.GetBytes("nobody-nbn@" + _domain.Name)) + "\r\n");
            socket.ReadUntil("334");
            socket.Send(Convert.ToBase64String(Encoding.ASCII.GetBytes("Not-The-Password-1")) + "\r\n");
            socket.ReadUntil(new List<string> { "535", "421", "550", "554" });

            // The second failure at a limit of two may have closed the session.
            try
            {
               socket.Send("QUIT\r\n");
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
            }
         }
      }

      /// <summary>The auto-ban ranges covering exactly that address.</summary>
      private static List<Dictionary<string, object>> AutoBans(string address)
      {
         return Ranges().Where(r => MiniJson.Text(r, "name").StartsWith("Auto-ban: ", StringComparison.Ordinal) &&
                                    MiniJson.Text(r, "lower") == address && MiniJson.Text(r, "upper") == address).ToList();
      }

      private static List<Dictionary<string, object>> Ranges()
      {
         (int status, string body) = Http("GET", "/api/v1/ipranges");
         ClassicAssert.AreEqual(200, status, body);

         object parsed = MiniJson.Parse(body);
         var list = parsed as List<object> ?? MiniJson.List((Dictionary<string, object>) parsed, "ranges");
         return list.Cast<Dictionary<string, object>>().ToList();
      }

      /// <summary>True once no auto-ban covers the address, polled until the deadline.</summary>
      private static bool NoBanWithin(string address, TimeSpan deadline)
      {
         DateTime until = DateTime.UtcNow + deadline;

         while (true)
         {
            if (AutoBans(address).Count == 0)
               return true;

            if (DateTime.UtcNow > until)
               return false;

            Thread.Sleep(250);
         }
      }

      /// <summary>True when the address is banned now and still is after the wait - long enough for a lift to have happened.</summary>
      private static bool StillBannedAfter(string address, TimeSpan wait)
      {
         if (AutoBans(address).Count == 0)
            return false;

         Thread.Sleep(wait);
         return AutoBans(address).Count > 0;
      }

      /// <summary>
      ///    Everything this fixture made, by what it is called: the bans on its
      ///    net, its range, its trap and the record of its hits, the failed logons
      ///    it counted, and its rows of the list.
      /// </summary>
      private void TakeBackOurs()
      {
         foreach (Dictionary<string, object> range in Ranges())
         {
            string name = MiniJson.Text(range, "name");
            bool ours = name == RangeName ||
                        (name.StartsWith("Auto-ban: ", StringComparison.Ordinal) && MiniJson.Text(range, "lower").StartsWith(Net, StringComparison.Ordinal));

            if (ours)
               Http("DELETE", "/api/v1/ipranges/" + MiniJson.Number(range, "id"));
         }

         Http("DELETE", "/api/v1/spam-traps/" + TrapLocalPart + "@" + _domain.Name);
         Http("DELETE", "/api/v1/spam-traps/hits");
         Http("POST", "/api/v1/settings/logon-failures/clear", "{}");

         foreach (Dictionary<string, object> row in Ours())
            Http("DELETE", "/api/v1/never-ban-names/" + MiniJson.Number(row, "id"));

         // A row this fixture made under a pattern the refusal test re-uses.
         foreach (Dictionary<string, object> row in ((List<object>) MiniJson.Parse(Http("GET", "/api/v1/never-ban-names").body)).Cast<Dictionary<string, object>>())
         {
            string pattern = MiniJson.Text(row, "pattern");
            if (pattern.EndsWith(Provider, StringComparison.Ordinal) || pattern == "example.co.uk")
               Http("DELETE", "/api/v1/never-ban-names/" + MiniJson.Number(row, "id"));
         }
      }

      private static Dictionary<string, object> Settings()
      {
         (int status, string body) = Http("GET", "/api/v1/settings");
         ClassicAssert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static void PutSettings(string body)
      {
         (int status, string answer) = Http("PUT", "/api/v1/settings", body);
         ClassicAssert.AreEqual(200, status, answer);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));

         using (var client = new TcpClient())
         {
            Exception last = null;

            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect(TestPorts.HostAddress, RestPort);
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
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               head.Append("Authorization: Basic " + credentials + "\r\n");

               if (requestBody != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);

               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[8192];
               int read;

               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               string[] first = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               int status = 0;

               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
