// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The texts the release review of 6.3.7 found saying what the server does not do
   /// (part A, CPV-1 and CPV-3 to CPV-8, and the two it saw outside the findings, 5 and
   /// 7; part B, CPB-3 and CPB-5, and its seen 1), each held to what the server does:
   /// read from the pages' source, the catalogue and the documentation, so that the
   /// claim cannot come back in a rewording of its sentence.
   /// </summary>
   public class ReleaseReviewTextsTests
   {
      /// <summary>
      /// The server serves CalDAV and CardDAV under /dav/ on the web services listener
      /// (WebServicesServer.cpp), and an empty redirect sends the discovery URL there
      /// (BuiltInDavUrl_): the card said hMailServer implemented neither and that empty
      /// answered 404, and SettingsReasons.md said the same (CPV-1, part B's seen 1).
      /// </summary>
      [Fact]
      public void TheCalendarCard_SaysTheServerServesDavItself_AndThatEmptyLeadsToItsOwnDav()
      {
         string page = File_(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.Contains("L(\"hMailServer serves CalDAV and CardDAV itself, under /dav/ on this listener, over HTTPS, to each account by its mail address and password.", page, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Redirect /.well-known/caldav to (empty = this server's own /dav/)\")", page, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Redirect /.well-known/carddav to (empty = this server's own /dav/)\")", page, StringComparison.Ordinal);
         Assert.DoesNotContain("does not implement CalDAV", page, StringComparison.Ordinal);
         Assert.DoesNotContain("(empty = answer 404)", page, StringComparison.Ordinal);

         string reasons = Flat_(File.ReadAllText(Path.Combine(Docs_(), "SettingsReasons.md")));
         Assert.Contains("hMailServer serves CalDAV and CardDAV itself, under /dav/ on the web services listener", reasons, StringComparison.Ordinal);
         Assert.Contains("empty sends the discovery URLs to this server's own /dav/", reasons, StringComparison.Ordinal);
         Assert.DoesNotContain("empty means the discovery URLs answer 404", reasons, StringComparison.Ordinal);
      }

      /// <summary>
      /// The scan runs at delivery, after the message was accepted
      /// (SMTPDeliverer::RunVirusProtection_): the overview's note, in both its plural
      /// forms, said the sender waited for it (CPV-3).
      /// </summary>
      [Fact]
      public void TheVirusOverview_PutsTheScanAfterTheMessageIsAccepted()
      {
         string pipeline = File_(Path.Join("Services", "VirusPipeline.cs"));
         Assert.Equal(2, Regex.Matches(pipeline, Regex.Escape("the scan runs on the delivery thread after the message has been accepted, so its cost is delivery delay, not the sender's wait.")).Count);
         Assert.DoesNotContain("while the sender is waiting", pipeline, StringComparison.Ordinal);
      }

      /// <summary>
      /// The default ranges are IPv4 alone, so an IPv6 client, ::1 included, matches none
      /// and is refused: the button and its question said "'Internet' (everything else)"
      /// (CPV-4).
      /// </summary>
      [Fact]
      public void RestoringTheDefaultRanges_SaysTheyAreIPv4Alone()
      {
         string xaml = File_(Path.Join("Views", "IPRangesView.xaml"));
         string code = File_(Path.Join("Views", "IPRangesView.xaml.cs"));
         Assert.Contains("(every IPv4 address), which no IPv6 client matches.", xaml, StringComparison.Ordinal);
         Assert.Contains("IPv6 clients, ::1 included, then match no range and are refused until an IPv6 range is added.", code, StringComparison.Ordinal);
         Assert.DoesNotContain("(everything else)", xaml + code, StringComparison.Ordinal);
      }

      /// <summary>
      /// With 0 no lifetime is set and OpenSSL's own applies, 7,200 seconds
      /// (tls1_default_timeout): the label said 300 (CPV-5).
      /// </summary>
      [Fact]
      public void TheResumptionLifetime_NamesOpenSslsOwnDefault()
      {
         string page = File_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("L(\"Resumption lifetime for cached sessions and tickets (seconds; 0 = OpenSSL's default of 7,200)\")", page, StringComparison.Ordinal);
         Assert.DoesNotContain("OpenSSL's default of 300", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// A verdict after the body is refused with 554 when it is not quarantined, one
      /// before it with 550, and an incoming relay's checks made after the body are of
      /// the second kind (SMTPConnection::DoSpamProtection_): the card said 550 for the
      /// verdicts it changes (CPV-6).
      /// </summary>
      [Fact]
      public void TheQuarantineCard_NamesTheCodesTheServerRefusesWith()
      {
         string page = File_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("Off, a message over it is refused during the SMTP conversation with a 554 and the sender is told.", page, StringComparison.Ordinal);
         Assert.Contains("stays a refusal, with a 550 - and so does that check when it is made after the body, for mail from an incoming relay, against the address in its headers.", page, StringComparison.Ordinal);
         Assert.DoesNotContain("with a 550 and the sender is told", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// The Send-As switch keeps an account from sending in another's name; the German
      /// said it could send no more than any other (CPV-7).
      /// </summary>
      [Fact]
      public void TheGermanSendAsSentence_SaysInTheNameOfAnotherAccount()
      {
         Dictionary<string, string> german = Catalogue_("Strings.de.resx");
         KeyValuePair<string, string> sendAs = Assert.Single(german, pair => pair.Key.StartsWith("With this on, MAIL FROM in an authenticated session must be the account's own address", StringComparison.Ordinal));
         Assert.EndsWith("kann also nicht mehr im Namen jedes anderen Kontos senden.", sendAs.Value, StringComparison.Ordinal);
         Assert.DoesNotContain(german.Values, value => value.Contains("nicht mehr als jedes andere senden", StringComparison.Ordinal));
      }

      /// <summary>
      /// A listed address's failures are not counted towards an auto-ban, and the
      /// per-name lockout counts them all the same (AccountLogon): the list's hint and
      /// its dialog said they were not counted (CPV-8).
      /// </summary>
      [Fact]
      public void TheNeverBanList_SaysThePerNameLockoutStillCounts()
      {
         string page = File_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string dialog = File_(Path.Join("Views", "ServerSettingsView.NeverBanAddresses.cs"));
         Assert.Contains("its failed sign-ins do not count towards an auto-ban; the per-name lockout below still counts them.", page, StringComparison.Ordinal);
         Assert.Contains("its failed sign-ins do not count towards an auto-ban; the per-name lockout still counts them.", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("its failed sign-ins are not counted", page + dialog, StringComparison.Ordinal);
      }

      /// <summary>
      /// The index only rules out messages it marks complete, so until a rebuild catches
      /// up a search reads the rest itself - slower, or refused with NO [LIMIT] - and is
      /// never short: the card, its question and its result said it found less than it
      /// should (part A, seen outside the findings 5).
      /// </summary>
      [Fact]
      public void TheIndexRebuild_SaysASearchIsSlower_NotShort()
      {
         string page = File_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.DoesNotContain("IMAP SEARCH finds less than it should", page, StringComparison.Ordinal);
         Assert.DoesNotContain("IMAP SEARCH will find less than it should", page, StringComparison.Ordinal);
         Assert.Contains("IMAP SEARCH reads every message the index does not cover yet, so a search is slower, and one past the search limits is refused with NO [LIMIT]; none finds less than it should.", page, StringComparison.Ordinal);
         Assert.Contains("IMAP SEARCH reads every message the index does not cover yet: searches are slower, and one past the search limits is refused with NO [LIMIT].", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// RFC 5426 asks a receiver to take 480 octets and recommends 2,048; 1,024 is the
      /// server's choice between them, and TCP frames up to 65,000
      /// (SyslogFormatter::UdpByteLimit, TcpByteLimit): the claim called 1,024 the RFC's
      /// guidance and said the streams truncated nothing (part A, seen outside the
      /// findings 7).
      /// </summary>
      [Fact]
      public void TheSyslogClaim_NamesTheRfcsTwoFigures_AndTheStreamsLimit()
      {
         string claims = File_(Path.Join("Services", "SettingClaims.cs"));
         Assert.Contains("more than the 480 RFC 5426 requires a receiver to take, less than the 2,048 it recommends", claims, StringComparison.Ordinal);
         Assert.Contains("truncate nothing below 65,000 bytes.", claims, StringComparison.Ordinal);
         Assert.DoesNotContain("RFC 5426's guidance", claims, StringComparison.Ordinal);
      }

      /// <summary>
      /// RuleLoopLimit bounds every message the server generates for a message - a rule's
      /// forward, reply, copy or notice, a forwarding, a bounce, an auto-reply, a
      /// webhook's return - and no other action of a rule: the card said it bounded how
      /// far a chain of the rules' messages went before rules stopped acting on it, and
      /// the row said nothing (part B, CPB-3).
      /// </summary>
      [Fact]
      public void TheRuleLimitsCard_SaysWhatTheLimitCounts()
      {
         string page = File_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("L(\"What every rule is held to: how many messages the server may generate for one message - a forward, a reply, a copy, a bounce - before it generates no more for it.\")", page, StringComparison.Ordinal);
         Assert.Contains("Blurb = L(\"How many generated messages a message may lead to before the server generates no more for it: a rule's forward, reply, copy or notice, an account's forwarding or redirect, a bounce, an out-of-office or Sieve reply, or an inbound webhook's return; a rule's other actions are not limited. 5 as shipped; 0 generates none, not even a bounce.\")", page, StringComparison.Ordinal);
         Assert.DoesNotContain("how far a chain of messages that rules send", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// Incoming relays' front-end proxies are the SMTP listener's alone; the IMAP and
      /// POP3 listeners have a PROXY switch and list of their own, on Protocols. The two
      /// places said nothing of each other, and the relays page's description, its task
      /// and SettingsReasons.md read as though its proxies covered every listener (part B,
      /// CPB-5).
      /// </summary>
      [Fact]
      public void TheProxySettings_SayWhichListenerTheyAreFor_AndPointToEachOther()
      {
         string page = File_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("proxies.Settings.Add(new ElsewhereRow { Label = L(\"The PROXY protocol on the IMAP listener\"), Page = \"protocols\", Tab = N(\"IMAP\") });", page, StringComparison.Ordinal);
         Assert.Contains("proxies.Settings.Add(new ElsewhereRow { Label = L(\"The PROXY protocol on the POP3 listener\"), Page = \"protocols\", Tab = N(\"POP3\") });", page, StringComparison.Ordinal);
         Assert.Contains("imap.Settings.Add(new ElsewhereRow { Label = L(\"The PROXY protocol and XCLIENT on the SMTP listener\"), Page = \"relays\" });", page, StringComparison.Ordinal);
         Assert.Contains("pop3.Settings.Add(new ElsewhereRow { Label = L(\"The PROXY protocol and XCLIENT on the SMTP listener\"), Page = \"relays\" });", page, StringComparison.Ordinal);

         Assert.Contains("the front-end proxies that may pass on the real client address to the SMTP listener with the PROXY protocol or XCLIENT.", File_(Path.Join("Services", "NavigationMap.cs")), StringComparison.Ordinal);
         Assert.Contains("the IMAP and POP3 listeners each have a PROXY switch of their own, on Protocols.", File_(Path.Join("Services", "IntentIndex.cs")), StringComparison.Ordinal);
         Assert.Contains("These settings are the SMTP listener's alone: the IMAP and POP3 listeners each have a PROXY switch and a list of proxies of their own", Flat_(File.ReadAllText(Path.Combine(Docs_(), "SettingsReasons.md"))), StringComparison.Ordinal);
      }

      private static string Flat_(string text) => Regex.Replace(text, @"\s+", " ");

      private static Dictionary<string, string> Catalogue_(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(ControlPanelFile_(Path.Join("Resources", file))).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }

      private static string File_(string relative) => File.ReadAllText(ControlPanelFile_(relative));

      private static string ControlPanelFile_(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }

      private static string Docs_()
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Combine(at.FullName, "hmailserver", "docs");
            if (Directory.Exists(candidate))
               return candidate;
         }

         throw new DirectoryNotFoundException("the documentation is not above " + AppContext.BaseDirectory);
      }
   }
}
