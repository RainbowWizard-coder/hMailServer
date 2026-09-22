// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Xml;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A calendar shared with somebody, the three parts of the row: free-busy
   ///    for everyone in the domain (the route and the CalDAV free-busy-query
   ///    REPORT, one computation), a calendar shared with a group whose members
   ///    see it as a named sharee does, and a delegate who answers the owner's
   ///    invitations on the owner's behalf - which needs the invitation filed
   ///    into the owner's calendar on delivery, and answered from the message
   ///    without a second copy. Each case with its negative control, so a
   ///    free-busy that leaked a title, a share that outlived a membership, or a
   ///    reply a plain writer could send would fail here rather than pass.
   /// </summary>
   [TestFixture]
   public class RestApiCalendarSharing : TestFixtureBase
   {
      // 9816 is ClientDiscovery's REST port; these two are this fixture's own.
      private const int WebServicesPort = 9818;
      private static int RestPort = 9819;
      private const int SmtpPort = 25;
      private const string Password = "Sharing-Passw0rd!";
      private const string AdminPassword = "testar";

      private const string NsDav = "DAV:";
      private const string NsCalDav = "urn:ietf:params:xml:ns:caldav";

      private Account _owner;
      private Account _colleague;
      private Account _delegate;
      private Account _writer;
      private Account _reader;
      private Account _organiser;
      private Account _member;

      private string Owner { get { return "owner@" + _domain.Name; } }
      private string Colleague { get { return "colleague@" + _domain.Name; } }
      private string Deputy { get { return "delegate@" + _domain.Name; } }
      private string Writer { get { return "writer@" + _domain.Name; } }
      private string Reader { get { return "reader@" + _domain.Name; } }
      private string Organiser { get { return "organiser@" + _domain.Name; } }
      private string Member { get { return "member@" + _domain.Name; } }

      private static string HomeOf(string address)
      {
         return "/dav/calendars/" + address + "/";
      }

      [SetUp]
      public void StartListeners()
      {
         _owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Owner, Password);
         _owner.PersonFirstName = "Olivia";
         _owner.PersonLastName = "Owner";
         _owner.Save();
         _colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Colleague, Password);
         _delegate = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Deputy, Password);
         _delegate.PersonFirstName = "Dan";
         _delegate.PersonLastName = "Delegate";
         _delegate.Save();
         _writer = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Writer, Password);
         _reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Reader, Password);
         _organiser = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Organiser, Password);
         _member = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Member, Password);
         _settings.SetAdministratorPassword(AdminPassword);

         IniFileSetting.Write("CalDavRedirectUrl", "");
         IniFileSetting.Write("CardDavRedirectUrl", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebServicesPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", "0");

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         Response probe = Dav("OPTIONS", "/dav/", Auth(Owner, Password));
         Assert.AreEqual(200, probe.Status, "The DAV endpoint did not answer OPTIONS. Body: " + probe.Body);
      }

      [TearDown]
      public void StopListeners()
      {
         IniFileSetting.Write("WebServicesHttpPort", "0");
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("Free-busy: a colleague's own calendars over a day answer busy and tentative periods with no titles; a transparent, a cancelled and a declined event are left out, and so is a calendar shared with the colleague; a window past 62 days is 400; another hosted domain's account, an unknown address and an account that chose nobody are one 404; an alias domain's spelling resolves; the choice put back answers again")]
      public void FreeBusyOfAColleagueSaysWhenAndNothingElse()
      {
         // The colleague's day, made over REST and over CalDAV: what a client
         // wrote is what is read.
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Colleague, Password).status);
         (int status, string body) busy = Me("POST", "/api/v1/me/calendar/events", "{\"summary\":\"Dentist\",\"location\":\"High Street\",\"start\":\"2026-11-03T10:00\",\"end\":\"2026-11-03T11:00\",\"tz\":\"UTC\"}", Colleague, Password);
         Assert.AreEqual(201, busy.status, busy.body);
         Assert.AreEqual(201, Put(HomeOf(Colleague) + "calendar/tentative.ics", Colleague, Ics("fb-tentative", "20261103T130000Z", "20261103T133000Z", "Maybe lunch",
            "ORGANIZER:mailto:" + Organiser, "ATTENDEE;PARTSTAT=TENTATIVE:mailto:" + Colleague)).Status);
         Assert.AreEqual(201, Put(HomeOf(Colleague) + "calendar/transparent.ics", Colleague, Ics("fb-transparent", "20261103T120000Z", "20261103T123000Z", "Reading", "TRANSP:TRANSPARENT")).Status);
         Assert.AreEqual(201, Put(HomeOf(Colleague) + "calendar/declined.ics", Colleague, Ics("fb-declined", "20261103T140000Z", "20261103T150000Z", "Declined thing",
            "ORGANIZER:mailto:" + Organiser, "ATTENDEE;PARTSTAT=DECLINED:mailto:" + Colleague)).Status);
         Assert.AreEqual(201, Put(HomeOf(Colleague) + "calendar/cancelled.ics", Colleague, Ics("fb-cancelled", "20261103T170000Z", "20261103T180000Z", "Cancelled thing", "STATUS:CANCELLED")).Status);
         // A weekly meeting that recurs onto the day: the recurrence is expanded.
         Assert.AreEqual(201, Put(HomeOf(Colleague) + "calendar/weekly.ics", Colleague, Ics("fb-weekly", "20261020T090000Z", "20261020T093000Z", "Weekly", "RRULE:FREQ=WEEKLY;COUNT=6")).Status);

         // A calendar the owner shares with the colleague, with an event on the
         // same day: the colleague's free-busy is their own calendars only.
         (int status, string body) rota = Me("POST", "/api/v1/me/calendars", "{\"display_name\":\"Rota\"}", Owner, Password);
         Assert.AreEqual(201, rota.status, rota.body);
         long rotaId = IdOf(rota.body);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/" + Colleague, "{\"write\":false}", Owner, Password).status);
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + rotaId + ",\"summary\":\"Rota shift\",\"start\":\"2026-11-03T15:00\",\"end\":\"2026-11-03T16:00\",\"tz\":\"UTC\"}", Owner, Password).status);

         string window = "&from=2026-11-03T00:00:00Z&to=2026-11-04T00:00:00Z";
         (int status, string body) answer = Me("GET", "/api/v1/me/freebusy?address=" + Colleague + window, null, Owner, Password);
         Assert.AreEqual(200, answer.status, answer.body);
         StringAssert.Contains("\"address\":\"" + Colleague + "\"", answer.body);
         StringAssert.Contains("\"from\":\"20261103T000000Z\",\"to\":\"20261104T000000Z\"", answer.body);
         List<string> periods = Periods(answer.body);
         CollectionAssert.AreEqual(new[] { "20261103T090000Z/20261103T093000Z/busy", "20261103T100000Z/20261103T110000Z/busy", "20261103T130000Z/20261103T133000Z/tentative" }, periods,
            "The weekly occurrence, the dentist and the tentative lunch, sorted; nothing transparent, declined, cancelled or shared in. Body: " + answer.body);
         StringAssert.DoesNotContain("Dentist", answer.body, "No title leaves the route.");
         StringAssert.DoesNotContain("High Street", answer.body, "No location either.");
         StringAssert.DoesNotContain("\"summary\"", answer.body);

         // The window's forms: a day, an offset, the iCalendar form; and its bounds.
         Assert.AreEqual(200, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + "&from=2026-11-03&to=2026-11-04", null, Owner, Password).status);
         (int status, string body) offset = Me("GET", "/api/v1/me/freebusy?address=" + Colleague + "&from=2026-11-03T11:00%2B01:00&to=2026-11-03T12:00%2B01:00", null, Owner, Password);
         Assert.AreEqual(200, offset.status, offset.body);
         CollectionAssert.AreEqual(new[] { "20261103T100000Z/20261103T110000Z/busy" }, Periods(offset.body), "11:00 to 12:00 at +01:00 is 10:00Z to 11:00Z, and the dentist fills it. Body: " + offset.body);
         Assert.AreEqual(200, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + "&from=20261103T000000Z&to=20261104T000000Z", null, Owner, Password).status);
         Assert.AreEqual(400, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + "&from=2026-11-03&to=2027-01-05", null, Owner, Password).status, "63 days is past the ceiling.");
         Assert.AreEqual(200, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + "&from=2026-11-03&to=2027-01-04", null, Owner, Password).status, "62 days is the ceiling.");
         Assert.AreEqual(400, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + "&from=2026-11-04&to=2026-11-03", null, Owner, Password).status, "to before from.");
         Assert.AreEqual(400, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + "&from=2026-11-03", null, Owner, Password).status, "no to.");
         Assert.AreEqual(400, Me("GET", "/api/v1/me/freebusy?from=2026-11-03&to=2026-11-04", null, Owner, Password).status, "no address.");

         // One 404 for everybody the route does not answer for, so nothing is
         // enumerated: another hosted domain's account, an address nobody has,
         // and an account of the domain that chose nobody.
         Domain elsewhere = SingletonProvider<TestSetup>.Instance.AddDomain("sharing-elsewhere.test");
         Account stranger = SingletonProvider<TestSetup>.Instance.AddAccount(elsewhere, "stranger@sharing-elsewhere.test", Password);
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"summary\":\"Elsewhere\",\"start\":\"2026-11-03T10:00\",\"end\":\"2026-11-03T11:00\",\"tz\":\"UTC\"}", "stranger@sharing-elsewhere.test", Password).status);
         (int status, string body) otherDomain = Me("GET", "/api/v1/me/freebusy?address=stranger@sharing-elsewhere.test" + window, null, Owner, Password);
         (int status, string body) unknown = Me("GET", "/api/v1/me/freebusy?address=nobody@" + _domain.Name + window, null, Owner, Password);
         Assert.AreEqual(404, otherDomain.status, otherDomain.body);
         Assert.AreEqual(404, unknown.status, unknown.body);
         Assert.AreEqual(otherDomain.body, unknown.body, "The same answer for an account elsewhere and for no account.");
         Assert.AreEqual(404, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + window, null, "stranger@sharing-elsewhere.test", Password).status,
            "And the stranger cannot ask about this domain's account either.");

         Assert.AreEqual(200, Me("PUT", "/api/v1/me/preferences", "{\"freebusy\":\"nobody\"}", Colleague, Password).status);
         (int status, string body) hidden = Me("GET", "/api/v1/me/freebusy?address=" + Colleague + window, null, Owner, Password);
         Assert.AreEqual(404, hidden.status, hidden.body);
         Assert.AreEqual(unknown.body, hidden.body, "Nobody means the account is not there to this route.");
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/preferences", "{\"freebusy\":\"domain\"}", Colleague, Password).status);
         Assert.AreEqual(200, Me("GET", "/api/v1/me/freebusy?address=" + Colleague + window, null, Owner, Password).status, "The choice put back, the route answers again.");

         // An alias domain's spelling names the same account; one's own free-busy is answered too.
         DomainAlias alias = _domain.DomainAliases.Add();
         alias.DomainID = _domain.ID;
         alias.AliasName = "sharing-alias.test";
         alias.Save();
         (int status, string body) aliased = Me("GET", "/api/v1/me/freebusy?address=colleague@sharing-alias.test" + window, null, Owner, Password);
         Assert.AreEqual(200, aliased.status, aliased.body);
         StringAssert.Contains("\"address\":\"" + Colleague + "\"", aliased.body, "Answered under the account's own address.");
         (int status, string body) self = Me("GET", "/api/v1/me/freebusy?address=" + Owner + window, null, Owner, Password);
         Assert.AreEqual(200, self.status, self.body);
         CollectionAssert.AreEqual(new[] { "20261103T150000Z/20261103T160000Z/busy" }, Periods(self.body), "The owner's rota shift is the owner's own. Body: " + self.body);
      }

      [Test]
      [Description("CalDAV free-busy-query: on the account's own calendar and on one shared with it the REPORT answers one VFREEBUSY whose FREEBUSY lines are the route's periods, BUSY and BUSY-TENTATIVE; a calendar not shared with the caller is not found; a report without a time-range, and one over 62 days, are refused; the collection advertises the report")]
      public void FreeBusyQueryAnswersTheSameAsTheRoute()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Owner, Password).status);
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"summary\":\"Board\",\"start\":\"2026-11-05T09:00\",\"end\":\"2026-11-05T10:00\",\"tz\":\"UTC\"}", Owner, Password).status);
         Assert.AreEqual(201, Put(HomeOf(Owner) + "calendar/maybe.ics", Owner, Ics("fbq-tentative", "20261105T140000Z", "20261105T143000Z", "Maybe",
            "ORGANIZER:mailto:" + Organiser, "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + Owner)).Status);
         Assert.AreEqual(201, Put(HomeOf(Owner) + "calendar/free.ics", Owner, Ics("fbq-free", "20261105T110000Z", "20261105T120000Z", "Reading", "TRANSP:TRANSPARENT")).Status);

         string report = "<C:free-busy-query xmlns:C=\"" + NsCalDav + "\"><C:time-range start=\"20261105T000000Z\" end=\"20261106T000000Z\"/></C:free-busy-query>";
         Response own = Dav("REPORT", HomeOf(Owner) + "calendar/", Auth(Owner, Password), report);
         Assert.AreEqual(200, own.Status, own.Body);
         StringAssert.StartsWith("text/calendar", own.Headers["Content-Type"]);
         StringAssert.Contains("BEGIN:VFREEBUSY", own.Body);
         StringAssert.Contains("DTSTART:20261105T000000Z", own.Body);
         StringAssert.Contains("DTEND:20261106T000000Z", own.Body);
         StringAssert.Contains("FREEBUSY;FBTYPE=BUSY:20261105T090000Z/20261105T100000Z", own.Body);
         StringAssert.Contains("FREEBUSY;FBTYPE=BUSY-TENTATIVE:20261105T140000Z/20261105T143000Z", own.Body);
         StringAssert.DoesNotContain("20261105T110000Z", own.Body, "The transparent hour is free.");
         StringAssert.DoesNotContain("Board", own.Body, "No title in a VFREEBUSY.");

         (int status, string body) route = Me("GET", "/api/v1/me/freebusy?address=" + Owner + "&from=20261105T000000Z&to=20261106T000000Z", null, Owner, Password);
         Assert.AreEqual(200, route.status, route.body);
         CollectionAssert.AreEqual(new[] { "20261105T090000Z/20261105T100000Z/busy", "20261105T140000Z/20261105T143000Z/tentative" }, Periods(route.body), "The route and the report are one computation. Body: " + route.body);

         // Shared read-only with the reader: the report answers it there, under
         // shared-{id}; a non-sharee finds nothing.
         long defaultId = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars", null, Owner, Password).body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/" + Reader, "{\"write\":false}", Owner, Password).status);
         string sharedHref = HomeOf(Reader) + "shared-" + defaultId + "/";
         Response shared = Dav("REPORT", sharedHref, Auth(Reader, Password), report);
         Assert.AreEqual(200, shared.Status, shared.Body);
         StringAssert.Contains("FREEBUSY;FBTYPE=BUSY:20261105T090000Z/20261105T100000Z", shared.Body);
         StringAssert.Contains("FREEBUSY;FBTYPE=BUSY-TENTATIVE:20261105T140000Z/20261105T143000Z", shared.Body, "Tentative as it concerns the owner, whoever asks.");
         Assert.AreEqual(404, Dav("REPORT", HomeOf(Writer) + "shared-" + defaultId + "/", Auth(Writer, Password), report).Status, "Not shared with the writer: not there.");

         Assert.AreEqual(400, Dav("REPORT", HomeOf(Owner) + "calendar/", Auth(Owner, Password), "<C:free-busy-query xmlns:C=\"" + NsCalDav + "\"/>").Status, "No time-range.");
         Assert.AreEqual(400, Dav("REPORT", HomeOf(Owner) + "calendar/", Auth(Owner, Password),
            "<C:free-busy-query xmlns:C=\"" + NsCalDav + "\"><C:time-range start=\"20261105T000000Z\" end=\"20270110T000000Z\"/></C:free-busy-query>").Status, "Past 62 days.");
         Response supported = Dav("PROPFIND", HomeOf(Owner) + "calendar/", Auth(Owner, Password), "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:supported-report-set/></D:prop></D:propfind>", "Depth: 0\r\n");
         Assert.AreEqual(207, supported.Status, supported.Body);
         StringAssert.Contains("free-busy-query", supported.Body);
      }

      [Test]
      [Description("A calendar shared with a group: a member sees it over REST and in its CalDAV home as a named sharee does, a non-member does not, it is gone when the member leaves the group and back when they rejoin, the member cannot leave what is the group's share, a name that is both an account and a group is the account, the owner's listing names the group with its size, the share is taken back by the group's name, and a group deleted takes its share with it")]
      public void ACalendarSharedWithAGroupFollowsItsMembership()
      {
         hMailServer.Group group = _settings.Groups.Add();
         group.Name = "Calendar Sharers";
         group.Save();
         var membership = group.Members.Add();
         membership.AccountID = _member.ID;
         membership.Save();

         (int status, string body) rota = Me("POST", "/api/v1/me/calendars", "{\"display_name\":\"Rota\",\"color\":\"#188038\"}", Owner, Password);
         Assert.AreEqual(201, rota.status, rota.body);
         long rotaId = IdOf(rota.body);
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + rotaId + ",\"summary\":\"Shift\",\"start\":\"2026-11-10T08:00\",\"end\":\"2026-11-10T12:00\",\"tz\":\"UTC\"}", Owner, Password).status);

         (int status, string body) shared = Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/" + Uri.EscapeDataString("Calendar Sharers"), "{\"write\":false}", Owner, Password);
         Assert.AreEqual(200, shared.status, shared.body);
         StringAssert.Contains("{\"group\":\"Calendar Sharers\",\"members\":1,\"write\":false,\"delegate\":false}", shared.body);
         Assert.AreEqual(404, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/No%20Such%20Group", "{}", Owner, Password).status, "Neither an account nor a group.");

         string sharedHref = HomeOf(Member) + "shared-" + rotaId + "/";
         (int status, string body) memberList = Me("GET", "/api/v1/me/calendars", null, Member, Password);
         string entry = CalendarEntry(memberList.body, rotaId);
         StringAssert.Contains("\"display_name\":\"Rota\"", entry, memberList.body);
         StringAssert.Contains("\"mine\":false", entry);
         StringAssert.Contains("\"writable\":false", entry);
         StringAssert.Contains("\"owner\":\"" + Owner + "\"", entry);
         StringAssert.Contains("\"summary\":\"Shift\"", Me("GET", "/api/v1/me/calendar/events?start=20261110T000000Z&end=20261111T000000Z", null, Member, Password).body, "The member sees the owner's event.");
         Response memberHome = Propfind(HomeOf(Member), "1", "<D:displayname/><D:owner/><D:current-user-privilege-set/>", Auth(Member, Password));
         Assert.AreEqual(207, memberHome.Status, memberHome.Body);
         CollectionAssert.Contains(Hrefs(memberHome), sharedHref, memberHome.Body);
         StringAssert.Contains("/dav/principals/" + Owner + "/", PropText(memberHome, sharedHref, "D:owner"));
         StringAssert.DoesNotContain("write-content", Prop(memberHome, sharedHref, "D:current-user-privilege-set").InnerXml, "Read-only, as the share says.");
         Assert.AreEqual(403, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + rotaId + ",\"summary\":\"Not mine\",\"start\":\"2026-11-10T13:00\"}", Member, Password).status);

         // Not a member: not there, over either door.
         StringAssert.DoesNotContain("\"id\":" + rotaId + ",", Me("GET", "/api/v1/me/calendars", null, Reader, Password).body);
         Assert.AreEqual(404, Dav("PROPFIND", HomeOf(Reader) + "shared-" + rotaId + "/", Auth(Reader, Password), "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:displayname/></D:prop></D:propfind>", "Depth: 0\r\n").Status);

         // The share is the group's: a member cannot leave it, and cannot be
         // named to take it back either.
         Assert.AreEqual(403, Me("DELETE", "/api/v1/me/calendars/" + rotaId, null, Member, Password).status, "A group's share is not the member's to leave.");
         Assert.AreEqual(403, Dav("DELETE", sharedHref, Auth(Member, Password)).Status);
         Assert.AreEqual(404, Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/" + Member, null, Owner, Password).status, "Not shared with the member by name.");

         // The membership changed: read at the time the member asks.
         membership.Delete();
         StringAssert.DoesNotContain("\"id\":" + rotaId + ",", Me("GET", "/api/v1/me/calendars", null, Member, Password).body, "Gone with the membership.");
         Assert.AreEqual(404, Dav("PROPFIND", sharedHref, Auth(Member, Password), "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:displayname/></D:prop></D:propfind>", "Depth: 0\r\n").Status);
         membership = group.Members.Add();
         membership.AccountID = _member.ID;
         membership.Save();
         StringAssert.Contains("\"id\":" + rotaId + ",", Me("GET", "/api/v1/me/calendars", null, Member, Password).body, "Back with it.");

         // Named by the group and by an account at once, the account wins; the
         // owner's one calendar lists both kinds of share.
         Account both = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "both@" + _domain.Name, Password);
         hMailServer.Group namedLikeAnAccount = _settings.Groups.Add();
         namedLikeAnAccount.Name = "both@" + _domain.Name;
         namedLikeAnAccount.Save();
         (int status, string body) bothShared = Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/both@" + _domain.Name, "{\"write\":true}", Owner, Password);
         Assert.AreEqual(200, bothShared.status, bothShared.body);
         StringAssert.Contains("{\"address\":\"both@" + _domain.Name + "\",\"write\":true,\"delegate\":false}", bothShared.body, "The account, not the group of the same name.");
         StringAssert.DoesNotContain("\"group\":\"both@", bothShared.body);
         (int status, string body) one = Me("GET", "/api/v1/me/calendars/" + rotaId, null, Owner, Password);
         Assert.AreEqual(200, one.status, one.body);
         StringAssert.Contains("\"shares\":[{\"address\":\"both@" + _domain.Name + "\",\"write\":true,\"delegate\":false},{\"group\":\"Calendar Sharers\",\"members\":1,\"write\":false,\"delegate\":false}]", one.body);
         Assert.AreEqual(404, Me("GET", "/api/v1/me/calendars/" + rotaId, null, Reader, Password).status, "Not visible to a non-sharee.");
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars/" + rotaId, null, Member, Password).status, "Visible to a member.");
         namedLikeAnAccount.Delete();
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/both@" + _domain.Name, null, Owner, Password).status);

         // A second group's share taken back by name; the first group deleted
         // takes its share rows with it.
         hMailServer.Group temporary = _settings.Groups.Add();
         temporary.Name = "Temporary";
         temporary.Save();
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/Temporary", "{\"write\":true}", Owner, Password).status);
         StringAssert.Contains("\"group\":\"Temporary\"", Me("GET", "/api/v1/me/calendars/" + rotaId, null, Owner, Password).body);
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/Temporary", null, Owner, Password).status);
         Assert.AreEqual(404, Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/Temporary", null, Owner, Password).status, "Taken back once.");
         StringAssert.DoesNotContain("\"group\":\"Temporary\"", Me("GET", "/api/v1/me/calendars/" + rotaId, null, Owner, Password).body);
         temporary.Delete();

         group.Delete();
         StringAssert.DoesNotContain("\"group\":", Me("GET", "/api/v1/me/calendars/" + rotaId, null, Owner, Password).body, "The group gone, its share gone.");
         StringAssert.DoesNotContain("\"id\":" + rotaId + ",", Me("GET", "/api/v1/me/calendars", null, Member, Password).body);
         Assert.AreEqual(404, Dav("PROPFIND", sharedHref, Auth(Member, Password), "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:displayname/></D:prop></D:propfind>", "Depth: 0\r\n").Status);
      }

      [Test]
      [Description("A REQUEST delivered to an account with a calendar is filed into its default calendar as it arrived, NEEDS-ACTION, and the answer from the message updates that one event rather than adding a second; an account with no calendar row gets nothing filed")]
      public void AnInvitationIsFiledOnDeliveryAndAnsweredWithoutADouble()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Reader, Password).status, "The reader's calendar exists before the invitation arrives.");
         // The writer has never opened a calendar: nothing is filed for them.

         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Kick-off\",\"start\":\"2026-11-12T09:00\",\"end\":\"2026-11-12T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + Reader + "\",\"" + Writer + "\"],\"invite\":true}", Organiser, Password);
         Assert.AreEqual(201, made.status, made.body);
         StringAssert.Contains("\"invited\":2", made.body);

         string window = "/api/v1/me/calendar/events?start=20261112T000000Z&end=20261113T000000Z";
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
         {
            (int status, string body) events = Me("GET", window, null, Reader, Password);
            RetryableAssert.StringContains("\"summary\":\"Kick-off\"", events.body);
            RetryableAssert.StringContains("\"my_partstat\":\"NEEDS-ACTION\"", events.body);
         });
         (int status, string body) filed = Me("GET", window, null, Reader, Password);
         Assert.AreEqual(1, Regex.Matches(filed.body, "\"summary\":\"Kick-off\"").Count, filed.body);
         StringAssert.Contains("\"organizer\":\"" + Organiser + "\"", filed.body);

         long messageId = MessageIdOf(Reader, Password, "Invitation: Kick-off");
         (int status, string body) card = Me("GET", "/api/v1/me/messages/" + messageId + "/invitation", null, Reader, Password);
         Assert.AreEqual(200, card.status, card.body);
         StringAssert.Contains("\"in_calendar\":true", card.body, "The message's card finds the filed copy.");
         StringAssert.Contains("\"stored_partstat\":\"NEEDS-ACTION\"", card.body);

         (int status, string body) accepted = Me("POST", "/api/v1/me/messages/" + messageId + "/invitation/reply", "{\"action\":\"accept\"}", Reader, Password);
         Assert.AreEqual(200, accepted.status, accepted.body);
         (int status, string body) after = Me("GET", window, null, Reader, Password);
         Assert.AreEqual(1, Regex.Matches(after.body, "\"summary\":\"Kick-off\"").Count, "One event, updated, not two. Body: " + after.body);
         StringAssert.Contains("\"my_partstat\":\"ACCEPTED\"", after.body);

         // The writer, with no calendar row at delivery: the card's look makes
         // the default now, and finds nothing in it.
         long writerMessage = MessageIdOf(Writer, Password, "Invitation: Kick-off");
         (int status, string body) writerCard = Me("GET", "/api/v1/me/messages/" + writerMessage + "/invitation", null, Writer, Password);
         Assert.AreEqual(200, writerCard.status, writerCard.body);
         StringAssert.Contains("\"in_calendar\":false", writerCard.body, "Nothing was filed for an account with no calendar.");
         Assert.AreEqual(0, Regex.Matches(Me("GET", window, null, Writer, Password).body, "\"summary\":\"Kick-off\"").Count);
      }

      [Test]
      [Description("A delegate answers the owner's invitation on the owner's behalf: allowed to the delegate and the owner, refused 403 to a plain writer and to a reader; the owner's PARTSTAT is set with SENT-BY the delegate, the organiser receives the REPLY with SENT-BY on the ATTENDEE and Sender: the delegate beside From: the owner, and files it; the delegate's own calendar is untouched; the owner's own answer clears SENT-BY; a bad partstat and the owner's own event are 400")]
      public void ADelegateAnswersOnTheOwnersBehalf()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Owner, Password).status);
         long defaultId = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars", null, Owner, Password).body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         (int status, string body) delegated = Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/" + Deputy, "{\"delegate\":true}", Owner, Password);
         Assert.AreEqual(200, delegated.status, delegated.body);
         StringAssert.Contains("{\"address\":\"" + Deputy + "\",\"write\":true,\"delegate\":true}", delegated.body, "A delegate may write.");
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/" + Writer, "{\"write\":true}", Owner, Password).status);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/" + Reader, "{\"write\":false}", Owner, Password).status);
         string delegateEntry = CalendarEntry(Me("GET", "/api/v1/me/calendars", null, Deputy, Password).body, defaultId);
         StringAssert.Contains("\"writable\":true,\"delegate\":true", delegateEntry, "The delegate's listing says so.");
         StringAssert.Contains("\"writable\":true,\"delegate\":false", CalendarEntry(Me("GET", "/api/v1/me/calendars", null, Writer, Password).body, defaultId));

         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Budget\",\"start\":\"2026-11-20T14:00\",\"end\":\"2026-11-20T15:00\",\"tz\":\"UTC\",\"attendees\":[\"Olivia Owner <" + Owner + ">\"],\"invite\":true}", Organiser, Password);
         Assert.AreEqual(201, made.status, made.body);
         long organiserEventId = IdOf(made.body);

         string window = "/api/v1/me/calendar/events?start=20261120T000000Z&end=20261121T000000Z";
         long eventId = 0;
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
         {
            (int status, string body) events = Me("GET", window, null, Owner, Password);
            RetryableAssert.StringContains("\"summary\":\"Budget\"", events.body);
            eventId = long.Parse(Regex.Match(events.body.Substring(events.body.IndexOf("\"events\":[", StringComparison.Ordinal)), "\"id\":(\\d+)").Groups[1].Value);
         });
         (int status, string body) seenByDelegate = Me("GET", window, null, Deputy, Password);
         StringAssert.Contains("\"summary\":\"Budget\"", seenByDelegate.body, "The delegate sees the invitation in the shared calendar.");
         StringAssert.Contains("\"delegate\":true", EventEntry(seenByDelegate.body, eventId), seenByDelegate.body);

         // Who may answer.
         Assert.AreEqual(403, Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"ACCEPTED\"}", Reader, Password).status, "A reader may not.");
         Assert.AreEqual(403, Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"ACCEPTED\"}", Writer, Password).status, "A plain writer may not.");
         Assert.AreEqual(400, Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"MAYBE\"}", Deputy, Password).status, "Not a partstat.");
         Assert.AreEqual(0, Regex.Matches(Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Owner, Password).body, "\"partstat\":\"ACCEPTED\"").Count, "Nothing was set by the refusals.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Organiser, Password, 0);

         (int status, string body) answered = Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"ACCEPTED\",\"comment\":\"Olivia will be there.\"}", Deputy, Password);
         Assert.AreEqual(200, answered.status, answered.body);
         StringAssert.Contains("\"partstat\":\"ACCEPTED\",\"on_behalf_of\":\"" + Owner + "\",\"replied\":true,\"to\":\"" + Organiser + "\"", answered.body);

         (int status, string body) ownerEvent = Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Owner, Password);
         StringAssert.Contains("\"address\":\"" + Owner + "\",\"name\":\"Olivia Owner\",\"partstat\":\"ACCEPTED\",\"sent_by\":\"" + Deputy + "\"", ownerEvent.body, "The owner's answer, and who gave it, in the stored event.");
         StringAssert.Contains("\"my_partstat\":\"ACCEPTED\"", ownerEvent.body);
         // The object's text is folded at 75 octets (RFC 5545 section 3.1), and
         // the fold may fall inside the parameter; unfolded, the line is whole.
         StringAssert.Contains("SENT-BY=\\\"mailto:" + Deputy + "\\\"", ownerEvent.body.Replace("\\r\\n ", ""), "As RFC 5545 writes it, quoted, in the object CalDAV serves.");
         (int status, string body) delegatesOwn = Me("GET", "/api/v1/me/calendars", null, Deputy, Password);
         long delegateDefault = long.Parse(Regex.Match(delegatesOwn.body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         Assert.AreEqual(0, Regex.Matches(Me("GET", window, null, Deputy, Password).body, "\"calendar\":" + delegateDefault + ",").Count, "The delegate's own calendar is not touched.");

         // The organiser's side: the REPLY, its headers, and the event filed.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Organiser, Password, 1);
         string replyMail = FirstMessageKept(Organiser, Password);
         StringAssert.Contains("Subject: Accepted: Budget @", replyMail);
         StringAssert.Contains("From: \"Olivia Owner\" <" + Owner + ">", replyMail, "From the owner.");
         StringAssert.Contains("Sender: \"Dan Delegate\" <" + Deputy + ">", replyMail, "Sender the delegate: on behalf of.");
         StringAssert.Contains("method=REPLY", replyMail);
         long replyId = MessageIdOf(Organiser, Password, "Accepted: Budget");
         (int status, string body) replyCard = Me("GET", "/api/v1/me/messages/" + replyId + "/invitation", null, Organiser, Password);
         Assert.AreEqual(200, replyCard.status, replyCard.body);
         StringAssert.Contains("\"method\":\"REPLY\"", replyCard.body);
         StringAssert.Contains("\"address\":\"" + Owner + "\",\"name\":\"Olivia Owner\",\"partstat\":\"ACCEPTED\",\"sent_by\":\"" + Deputy + "\"", replyCard.body, "The ATTENDEE is the owner, SENT-BY the delegate.");
         RetryHelper.TryAction(TimeSpan.FromSeconds(15), () =>
         {
            (int status, string body) organiserEvent = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Organiser, Password);
            RetryableAssert.StringContains("\"address\":\"" + Owner + "\",\"name\":\"Olivia Owner\",\"partstat\":\"ACCEPTED\"", organiserEvent.body);
         });

         // The owner answering in person clears the delegate's mark.
         (int status, string body) inPerson = Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"TENTATIVE\"}", Owner, Password);
         Assert.AreEqual(200, inPerson.status, inPerson.body);
         StringAssert.Contains("\"partstat\":\"TENTATIVE\",\"on_behalf_of\":\"\"", inPerson.body);
         ownerEvent = Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Owner, Password);
         StringAssert.Contains("\"partstat\":\"TENTATIVE\",\"sent_by\":\"\"", ownerEvent.body, ownerEvent.body);
         StringAssert.DoesNotContain("SENT-BY", ownerEvent.body);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Organiser, Password, 2);

         // The owner's own event, with guests, is not an invitation to answer.
         (int status, string body) own = Me("POST", "/api/v1/me/calendar/events", "{\"summary\":\"Mine\",\"start\":\"2026-11-21T09:00\",\"end\":\"2026-11-21T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + Reader + "\"]}", Owner, Password);
         Assert.AreEqual(201, own.status, own.body);
         Assert.AreEqual(400, Me("POST", "/api/v1/me/calendar/events/" + IdOf(own.body) + "/reply", "{\"partstat\":\"ACCEPTED\"}", Deputy, Password).status, "The owner organises it.");
         Assert.AreEqual(403, Me("POST", "/api/v1/me/calendar/events/" + eventId + "x/reply", "{\"partstat\":\"ACCEPTED\"}", Deputy, Password).status, "Not an event id, so not a route.");
      }

      [Test]
      [Description("A forged REQUEST cannot rewrite the organiser's event: one carrying the organiser's own UID, a foreign ORGANIZER and a higher SEQUENCE is delivered to the organiser and to a guest and leaves both copies as they were, while the real organiser's resend at a higher SEQUENCE updates the guest's copy")]
      public void AForgedRequestCannotRewriteTheOrganisersEvent()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Reader, Password).status);
         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Steering\",\"start\":\"2026-12-01T09:00\",\"end\":\"2026-12-01T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + Reader + "\"],\"invite\":true}", Organiser, Password);
         Assert.AreEqual(201, made.status, made.body);
         long organiserEventId = IdOf(made.body);
         string uid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
         string window = "/api/v1/me/calendar/events?start=20261201T000000Z&end=20261202T000000Z";
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Steering\"", Me("GET", window, null, Reader, Password).body));

         // The attack: the UID every guest was mailed, under a stranger's name,
         // with a higher SEQUENCE and a new time - delivered to the organiser.
         SmtpRaw("attacker@evil.test", Organiser, "mail.evil.test", Invitation("attacker@evil.test", Organiser, "Invitation: Hijacked", "Hijacked", uid, "20261201T140000Z", "20261201T150000Z", 99, "attacker@evil.test", Organiser));
         MessageIdOf(Organiser, Password, "Invitation: Hijacked");
         (int status, string body) kept = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Organiser, Password);
         Assert.AreEqual(200, kept.status, kept.body);
         StringAssert.Contains("\"summary\":\"Steering\"", kept.body, "The organiser's event is the organiser's.");
         StringAssert.Contains("\"organizer\":\"" + Organiser + "\"", kept.body);
         StringAssert.Contains("\"start\":\"20261201T090000Z\"", kept.body);
         StringAssert.DoesNotContain("Hijacked", Me("GET", window, null, Organiser, Password).body, "Nor was a second event made.");

         // The same forgery at the guest: their filed copy names the real organiser and stays.
         SmtpRaw("attacker@evil.test", Reader, "mail.evil.test", Invitation("attacker@evil.test", Reader, "Invitation: Hijacked", "Hijacked", uid, "20261201T140000Z", "20261201T150000Z", 99, "attacker@evil.test", Reader));
         MessageIdOf(Reader, Password, "Invitation: Hijacked");

         // The control: the real organiser's resend at a higher SEQUENCE does reach the guest's copy.
         SmtpRaw(Organiser, Reader, "mail." + _domain.Name, Invitation(Organiser, Reader, "Updated invitation: Steering", "Steering (moved)", uid, "20261201T110000Z", "20261201T120000Z", 3, Organiser, Reader));
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Steering (moved)\"", Me("GET", window, null, Reader, Password).body));
         (int status, string body) guestCopy = Me("GET", window, null, Reader, Password);
         Assert.AreEqual(1, Regex.Matches(guestCopy.body, "\"uid\":\"" + Regex.Escape(uid) + "\"").Count, "One event, updated, never a second. Body: " + guestCopy.body);
         StringAssert.Contains("\"start\":\"20261201T110000Z\"", guestCopy.body);
         StringAssert.DoesNotContain("Hijacked", guestCopy.body, "The forgery delivered before the real resend left nothing.");
         StringAssert.Contains("\"organizer\":\"" + Organiser + "\"", guestCopy.body);
      }

      [Test]
      [Description("Calendar spam is not filed: a REQUEST in a message flagged as spam files nothing while the same kind of message unflagged is filed; a REQUEST that does not name the account as a guest files nothing while one naming an alias of the account is filed")]
      public void SpamAndUninvitedRequestsAreNotFiled()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Reader, Password).status);
         string window = "/api/v1/me/calendar/events?start=20261203T000000Z&end=20261204T000000Z";

         // The verdict is forced the way the sender-blacklist fixture forces it:
         // a blocked sender whose score reaches the mark threshold and not the
         // delete threshold, which needs no DNS and no host name that resolves.
         hMailServer.AntiSpam antiSpam = _settings.AntiSpam;
         int markBefore = antiSpam.SpamMarkThreshold;
         int deleteBefore = antiSpam.SpamDeleteThreshold;
         bool headerBefore = antiSpam.AddHeaderSpam;
         antiSpam.SpamMarkThreshold = 2;
         antiSpam.SpamDeleteThreshold = 1000;
         antiSpam.AddHeaderSpam = true;
         hMailServer.BlockedSender blocked = antiSpam.BlockedSenders.Add();
         blocked.Address = "promo@spam.test";
         blocked.Score = 2;
         blocked.Description = "RestApiCalendarSharing fixture";
         blocked.Save();
         try
         {
            // Flagged, delivered, not filed.
            SmtpRaw("promo@spam.test", Reader, "mail.spam.test", Invitation("promo@spam.test", Reader, "You won", "You won", "spam-0001@spam.test", "20261203T100000Z", "20261203T110000Z", 0, "promo@spam.test", Reader));
            MessageIdOf(Reader, Password, "You won");
            StringAssert.Contains("X-hMailServer-Spam: YES", FirstMessageKept(Reader, Password), "The message was flagged as spam on the way in.");
         }
         finally
         {
            antiSpam.BlockedSenders.Clear();
            antiSpam.SpamMarkThreshold = markBefore;
            antiSpam.SpamDeleteThreshold = deleteBefore;
            antiSpam.AddHeaderSpam = headerBefore;
         }

         // The control: the same kind of invitation, not flagged, is filed - and
         // by the time it is there the spam one's delivery, which came first,
         // had its chance.
         SmtpRaw("friend@elsewhere.test", Reader, "mail.elsewhere.test", Invitation("friend@elsewhere.test", Reader, "Invitation: Team drinks", "Team drinks", "drinks-0001@elsewhere.test", "20261203T170000Z", "20261203T180000Z", 0, "friend@elsewhere.test", Reader));
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Team drinks\"", Me("GET", window, null, Reader, Password).body));
         (int status, string body) filed = Me("GET", window, null, Reader, Password);
         StringAssert.DoesNotContain("You won", filed.body, "Spam is not in the calendar. Body: " + filed.body);
         Assert.AreEqual(0, Periods(Me("GET", "/api/v1/me/freebusy?address=" + Reader + "&from=2026-12-03T09:00:00Z&to=2026-12-03T12:00:00Z", null, Owner, Password).body).Count, "And not busy for it either.");

         // A REQUEST that names somebody else: not the account's to keep.
         SmtpRaw("friend@elsewhere.test", Reader, "mail.elsewhere.test", Invitation("friend@elsewhere.test", Reader, "Invitation: Nobody asked", "Nobody asked", "nobody-0001@elsewhere.test", "20261203T120000Z", "20261203T130000Z", 0, "friend@elsewhere.test", Writer));
         MessageIdOf(Reader, Password, "Nobody asked");

         // The control: one naming an alias of the account is the account's.
         Alias alias = _domain.Aliases.Add();
         alias.Name = "reader-alias@" + _domain.Name;
         alias.Value = Reader;
         alias.Active = true;
         alias.Save();
         SmtpRaw("friend@elsewhere.test", Reader, "mail.elsewhere.test", Invitation("friend@elsewhere.test", Reader, "Invitation: Via alias", "Via alias", "alias-0001@elsewhere.test", "20261203T140000Z", "20261203T150000Z", 0, "friend@elsewhere.test", "reader-alias@" + _domain.Name));
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Via alias\"", Me("GET", window, null, Reader, Password).body));
         filed = Me("GET", window, null, Reader, Password);
         StringAssert.DoesNotContain("Nobody asked", filed.body, "An invitation that does not name the account is not filed. Body: " + filed.body);
         StringAssert.Contains("\"in_calendar\":false", Me("GET", "/api/v1/me/messages/" + MessageIdOf(Reader, Password, "Nobody asked") + "/invitation", null, Reader, Password).body);
      }

      [Test]
      [Description("A REQUEST delivered again at the same SEQUENCE keeps the answer the guest already gave, and one at a higher SEQUENCE updates the copy as it arrived")]
      public void AResentRequestKeepsTheAnswerAndANewerOneUpdates()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Reader, Password).status);
         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Budget review\",\"start\":\"2026-12-05T09:00\",\"end\":\"2026-12-05T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + Reader + "\"],\"invite\":true}", Organiser, Password);
         Assert.AreEqual(201, made.status, made.body);
         string uid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
         string window = "/api/v1/me/calendar/events?start=20261205T000000Z&end=20261206T000000Z";
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Budget review\"", Me("GET", window, null, Reader, Password).body));
         long messageId = MessageIdOf(Reader, Password, "Invitation: Budget review");
         Assert.AreEqual(200, Me("POST", "/api/v1/me/messages/" + messageId + "/invitation/reply", "{\"action\":\"accept\"}", Reader, Password).status);
         StringAssert.Contains("\"my_partstat\":\"ACCEPTED\"", Me("GET", window, null, Reader, Password).body);

         // The same REQUEST again, SEQUENCE 0 as the first: the organiser never
         // got the answer, or sent the original again. The answer stays.
         SmtpRaw(Organiser, Reader, "mail." + _domain.Name, Invitation(Organiser, Reader, "Resent: Budget review", "Budget review", uid, "20261205T090000Z", "20261205T100000Z", 0, Organiser, Reader));
         MessageIdOf(Reader, Password, "Resent: Budget review");
         // A newer one after it, so that the resend's delivery is known to be over.
         SmtpRaw(Organiser, Reader, "mail." + _domain.Name, Invitation(Organiser, Reader, "Updated invitation: Budget review", "Budget review (moved)", uid, "20261205T110000Z", "20261205T120000Z", 2, Organiser, Reader));
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Budget review (moved)\"", Me("GET", window, null, Reader, Password).body));
         (int status, string body) after = Me("GET", window, null, Reader, Password);
         Assert.AreEqual(1, Regex.Matches(after.body, "\"uid\":\"" + Regex.Escape(uid) + "\"").Count, "One event through both. Body: " + after.body);
         StringAssert.Contains("\"start\":\"20261205T110000Z\"", after.body, "The newer sequence's time.");
         StringAssert.Contains("\"my_partstat\":\"NEEDS-ACTION\"", after.body, "A newer sequence is a new question, as it arrived.");

         // And the same-sequence resend of THAT, after an answer, keeps the answer.
         long moved = MessageIdOf(Reader, Password, "Updated invitation: Budget review");
         Assert.AreEqual(200, Me("POST", "/api/v1/me/messages/" + moved + "/invitation/reply", "{\"action\":\"tentative\"}", Reader, Password).status);
         SmtpRaw(Organiser, Reader, "mail." + _domain.Name, Invitation(Organiser, Reader, "Resent again: Budget review", "Budget review (moved)", uid, "20261205T110000Z", "20261205T120000Z", 2, Organiser, Reader));
         MessageIdOf(Reader, Password, "Resent again: Budget review");
         SmtpRaw(Organiser, Reader, "mail." + _domain.Name, Invitation(Organiser, Reader, "Marker: Budget review", "Marker", "marker-0001@" + _domain.Name, "20261205T150000Z", "20261205T153000Z", 0, Organiser, Reader));
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Marker\"", Me("GET", window, null, Reader, Password).body));
         after = Me("GET", window, null, Reader, Password);
         string budget = after.body.Split(new[] { "{\"id\":" }, StringSplitOptions.None).FirstOrDefault(item => item.Contains("\"uid\":\"" + uid + "\"")) ?? "";
         StringAssert.Contains("\"my_partstat\":\"TENTATIVE\"", budget, "The answer given survives a resend at the same sequence. Body: " + after.body);
      }

      [Test]
      [Description("Group shares in the round: the owner in a group the calendar is shared with sees it once as their own; an account reached by its own share and by groups has the widest rights of them; leaving while a group still reaches the calendar gives the own share back and says so with the group's name, over REST and CalDAV; a share taken back by name and one that was never made are one 404; a group shared as a delegate lets a member answer for the owner with SENT-BY the member; a member removed is refused on CalDAV PUT, DELETE and sync-collection")]
      public void GroupSharesMergeReachTheOwnerOnceAndAreNotAMembersToLeave()
      {
         hMailServer.Group sharers = _settings.Groups.Add();
         sharers.Name = "Calendar Sharers";
         sharers.Save();
         var sharersMembership = sharers.Members.Add();
         sharersMembership.AccountID = _member.ID;
         sharersMembership.Save();
         hMailServer.Group editors = _settings.Groups.Add();
         editors.Name = "Editors";
         editors.Save();
         var editorsMembership = editors.Members.Add();
         editorsMembership.AccountID = _member.ID;
         editorsMembership.Save();

         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Owner, Password).status);
         long defaultId = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars", null, Owner, Password).body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         (int status, string body) rota = Me("POST", "/api/v1/me/calendars", "{\"display_name\":\"Rota\"}", Owner, Password);
         Assert.AreEqual(201, rota.status, rota.body);
         long rotaId = IdOf(rota.body);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/Calendar%20Sharers", "{\"write\":false}", Owner, Password).status);
         string memberSharedHref = HomeOf(Member) + "shared-" + rotaId + "/";

         // The owner joins the group: their own calendar, once, theirs.
         var ownerMembership = sharers.Members.Add();
         ownerMembership.AccountID = _owner.ID;
         ownerMembership.Save();
         (int status, string body) ownerList = Me("GET", "/api/v1/me/calendars", null, Owner, Password);
         Assert.AreEqual(1, Regex.Matches(ownerList.body, "\"id\":" + rotaId + ",").Count, "Once. Body: " + ownerList.body);
         StringAssert.Contains("\"mine\":true", CalendarEntry(ownerList.body, rotaId), ownerList.body);
         List<string> ownerHome = Hrefs(Propfind(HomeOf(Owner), "1", "<D:displayname/>", Auth(Owner, Password)));
         CollectionAssert.Contains(ownerHome, HomeOf(Owner) + "rota/");
         CollectionAssert.DoesNotContain(ownerHome, HomeOf(Owner) + "shared-" + rotaId + "/", "Not a second time as a share.");
         ownerMembership.Delete();

         // The widest rights of every share that reaches the member: their own
         // read-only row, the read-only group, and a second group that may write.
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/" + Member, "{\"write\":false}", Owner, Password).status);
         Func<string> memberEntry = () => CalendarEntry(Me("GET", "/api/v1/me/calendars", null, Member, Password).body, rotaId);
         StringAssert.Contains("\"writable\":false", memberEntry(), "Two read-only shares are read-only.");
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/Editors", "{\"write\":true}", Owner, Password).status);
         StringAssert.Contains("\"writable\":true", memberEntry(), "The Editors' write reaches the member.");
         Assert.AreEqual(1, Regex.Matches(Me("GET", "/api/v1/me/calendars", null, Member, Password).body, "\"id\":" + rotaId + ",").Count, "Three rows, one calendar.");
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + rotaId + ",\"summary\":\"Written by a member\",\"start\":\"2026-11-10T13:00\",\"end\":\"2026-11-10T14:00\",\"tz\":\"UTC\"}", Member, Password).status);
         editorsMembership.Delete();
         StringAssert.Contains("\"writable\":false", memberEntry(), "Out of the Editors, read-only again.");
         Assert.AreEqual(403, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + rotaId + ",\"summary\":\"Not now\",\"start\":\"2026-11-10T15:00\"}", Member, Password).status);

         // Leaving while a group still reaches the calendar: the own share is
         // given back, the answer says which group keeps it, and it stays listed.
         (int status, string body) left = Me("DELETE", "/api/v1/me/calendars/" + rotaId, null, Member, Password);
         Assert.AreEqual(403, left.status, left.body);
         StringAssert.Contains("\"left\":false,\"group\":\"Calendar Sharers\"", left.body);
         StringAssert.Contains("\"id\":" + rotaId + ",", Me("GET", "/api/v1/me/calendars", null, Member, Password).body, "Still there through the group.");
         StringAssert.DoesNotContain("{\"address\":\"" + Member + "\"", Me("GET", "/api/v1/me/calendars/" + rotaId, null, Owner, Password).body, "The own share was given back.");
         Assert.AreEqual(403, Dav("DELETE", memberSharedHref, Auth(Member, Password)).Status);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/" + Member, "{\"write\":false}", Owner, Password).status);
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/Calendar%20Sharers", null, Owner, Password).status);
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/Editors", null, Owner, Password).status);
         (int status, string body) leftForReal = Me("DELETE", "/api/v1/me/calendars/" + rotaId, null, Member, Password);
         Assert.AreEqual(200, leftForReal.status, leftForReal.body);
         StringAssert.Contains("\"left\":true", leftForReal.body, "With no group reaching it, leaving leaves.");
         StringAssert.DoesNotContain("\"id\":" + rotaId + ",", Me("GET", "/api/v1/me/calendars", null, Member, Password).body);

         // One 404 for a name that is no group and for a group the calendar is
         // not shared with: the group list is not read by a DELETE.
         (int status, string body) noSuch = Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/No%20Such%20Group", null, Owner, Password);
         (int status, string body) notShared = Me("DELETE", "/api/v1/me/calendars/" + rotaId + "/shares/Editors", null, Owner, Password);
         Assert.AreEqual(404, noSuch.status, noSuch.body);
         Assert.AreEqual(404, notShared.status, notShared.body);
         Assert.AreEqual(noSuch.body, notShared.body, "The same words whether the group exists or not.");

         // Shared with the group as a delegate: a member answers for the owner.
         (int status, string body) delegated = Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/Calendar%20Sharers", "{\"delegate\":true}", Owner, Password);
         Assert.AreEqual(200, delegated.status, delegated.body);
         StringAssert.Contains("{\"group\":\"Calendar Sharers\",\"members\":1,\"write\":true,\"delegate\":true}", delegated.body);
         // And a plain writer beside the group, for the refusal that is a refusal
         // and not an absence: the reader, with no share at all, cannot see the
         // event and is answered 404 as for any event that is not theirs.
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/" + Writer, "{\"write\":true}", Owner, Password).status);
         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Offsite\",\"start\":\"2026-11-25T09:00\",\"end\":\"2026-11-25T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + Owner + "\"],\"invite\":true}", Organiser, Password);
         Assert.AreEqual(201, made.status, made.body);
         string window = "/api/v1/me/calendar/events?start=20261125T000000Z&end=20261126T000000Z";
         long eventId = 0;
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
         {
            (int status, string body) events = Me("GET", window, null, Owner, Password);
            RetryableAssert.StringContains("\"summary\":\"Offsite\"", events.body);
            eventId = long.Parse(Regex.Match(events.body.Substring(events.body.IndexOf("\"events\":[", StringComparison.Ordinal)), "\"id\":(\\d+)").Groups[1].Value);
         });
         Assert.AreEqual(403, Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"ACCEPTED\"}", Writer, Password).status, "A writer, not a member of the delegated group: not a delegate.");
         Assert.AreEqual(404, Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"ACCEPTED\"}", Reader, Password).status, "No share at all: the event is not visible to the reader.");
         (int status, string body) answered = Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"ACCEPTED\"}", Member, Password);
         Assert.AreEqual(200, answered.status, answered.body);
         StringAssert.Contains("\"on_behalf_of\":\"" + Owner + "\"", answered.body);
         // Invited by the bare address, the owner's ATTENDEE carries no CN.
         StringAssert.Contains("\"address\":\"" + Owner + "\",\"name\":\"\",\"partstat\":\"ACCEPTED\",\"sent_by\":\"" + Member + "\"", Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Owner, Password).body, "Every member may answer, and the answer says which one did.");
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + defaultId + "/shares/Calendar%20Sharers", null, Owner, Password).status);
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + defaultId + "/shares/" + Writer, null, Owner, Password).status);

         // A member removed is refused wherever CalDAV writes or reads.
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + rotaId + "/shares/Calendar%20Sharers", "{\"write\":false}", Owner, Password).status);
         string sync = "<D:sync-collection xmlns:D=\"DAV:\"><D:sync-token/><D:prop><D:getetag/></D:prop></D:sync-collection>";
         Assert.AreEqual(403, Put(memberSharedHref + "member.ics", Member, Ics("member-0001", "20261110T160000Z", "20261110T170000Z", "Over CalDAV")).Status, "Read-only while a member.");
         Assert.AreEqual(207, Dav("REPORT", memberSharedHref, Auth(Member, Password), sync).Status, "Readable while a member.");
         string objectHref = Hrefs(Propfind(memberSharedHref, "1", "<D:getetag/>", Auth(Member, Password))).First(h => h.EndsWith(".ics"));
         Assert.AreEqual(403, Dav("DELETE", objectHref, Auth(Member, Password)).Status);
         sharersMembership.Delete();
         Assert.AreEqual(404, Put(memberSharedHref + "member.ics", Member, Ics("member-0002", "20261110T160000Z", "20261110T170000Z", "Over CalDAV")).Status, "Not there once out of the group.");
         Assert.AreEqual(404, Dav("REPORT", memberSharedHref, Auth(Member, Password), sync).Status);
         Assert.AreEqual(404, Dav("DELETE", objectHref, Auth(Member, Password)).Status);
         Assert.AreEqual(404, Me("GET", "/api/v1/me/calendars/" + rotaId, null, Member, Password).status);

         editors.Delete();
         sharers.Delete();
      }

      [Test]
      [Description("Free-busy edges: an inactive account is the same 404 and answers again when active; a shared calendar's free-busy-query carries nothing of the owner's other calendar while the route for the owner carries both; a weekly 09:00 in Europe/London is busy at 08:00Z before the October clock change and 09:00Z after; a daily rule fills a 62-day window with 62 periods and 63 days are refused")]
      public void FreeBusyEdgesInactiveOtherCalendarsClockChangeAndADailyRule()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Colleague, Password).status);
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"summary\":\"Dentist\",\"start\":\"2026-11-03T10:00\",\"end\":\"2026-11-03T11:00\",\"tz\":\"UTC\"}", Colleague, Password).status);
         string window = "&from=2026-11-03&to=2026-11-04";
         (int status, string body) unknown = Me("GET", "/api/v1/me/freebusy?address=nobody@" + _domain.Name + window, null, Owner, Password);
         Assert.AreEqual(404, unknown.status);
         _colleague.Active = false;
         _colleague.Save();
         (int status, string body) inactive = Me("GET", "/api/v1/me/freebusy?address=" + Colleague + window, null, Owner, Password);
         Assert.AreEqual(404, inactive.status, inactive.body);
         Assert.AreEqual(unknown.body, inactive.body, "An inactive account is not there to this route either.");
         _colleague.Active = true;
         _colleague.Save();
         Assert.AreEqual(1, Periods(Me("GET", "/api/v1/me/freebusy?address=" + Colleague + window, null, Owner, Password).body).Count, "Active again, answered again.");

         // The shared calendar's report is that one calendar; the owner's own
         // free-busy is every calendar of theirs.
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Owner, Password).status);
         long defaultId = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars", null, Owner, Password).body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         (int status, string body) side = Me("POST", "/api/v1/me/calendars", "{\"display_name\":\"Side\"}", Owner, Password);
         Assert.AreEqual(201, side.status, side.body);
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + defaultId + ",\"summary\":\"Board\",\"start\":\"2026-11-05T09:00\",\"end\":\"2026-11-05T10:00\",\"tz\":\"UTC\"}", Owner, Password).status);
         Assert.AreEqual(201, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + IdOf(side.body) + ",\"summary\":\"Side project\",\"start\":\"2026-11-05T15:00\",\"end\":\"2026-11-05T16:00\",\"tz\":\"UTC\"}", Owner, Password).status);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/" + Reader, "{\"write\":false}", Owner, Password).status);
         string report = "<C:free-busy-query xmlns:C=\"" + NsCalDav + "\"><C:time-range start=\"20261105T000000Z\" end=\"20261106T000000Z\"/></C:free-busy-query>";
         Response shared = Dav("REPORT", HomeOf(Reader) + "shared-" + defaultId + "/", Auth(Reader, Password), report);
         Assert.AreEqual(200, shared.Status, shared.Body);
         StringAssert.Contains("FREEBUSY;FBTYPE=BUSY:20261105T090000Z/20261105T100000Z", shared.Body);
         StringAssert.DoesNotContain("20261105T150000Z", shared.Body, "The owner's other calendar is not in the shared one's report.");
         StringAssert.DoesNotContain("ATTENDEE", shared.Body, "A bare VFREEBUSY, as RFC 4791 section 7.10 answers it.");
         StringAssert.DoesNotContain("ORGANIZER", shared.Body);
         CollectionAssert.AreEqual(new[] { "20261105T090000Z/20261105T100000Z/busy", "20261105T150000Z/20261105T160000Z/busy" },
            Periods(Me("GET", "/api/v1/me/freebusy?address=" + Owner + "&from=2026-11-05&to=2026-11-06", null, Owner, Password).body), "The route is every calendar of the owner's.");

         // Across the clock change, and a rule that fills the window.
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Writer, Password).status);
         Assert.AreEqual(201, Put(HomeOf(Writer) + "calendar/london.ics", Writer, IcsLines("fb-london", "DTSTART;TZID=Europe/London:20261019T090000", "DTEND;TZID=Europe/London:20261019T093000", "SUMMARY:London standup", "RRULE:FREQ=WEEKLY;COUNT=3")).Status);
         CollectionAssert.AreEqual(new[] { "20261019T080000Z/20261019T083000Z/busy", "20261026T090000Z/20261026T093000Z/busy", "20261102T090000Z/20261102T093000Z/busy" },
            Periods(Me("GET", "/api/v1/me/freebusy?address=" + Writer + "&from=2026-10-18&to=2026-11-04", null, Owner, Password).body), "09:00 London is 08:00Z in summer time and 09:00Z after the clocks go back.");
         Assert.AreEqual(201, Put(HomeOf(Writer) + "calendar/daily.ics", Writer, IcsLines("fb-daily", "DTSTART:20261201T100000Z", "DTEND:20261201T103000Z", "SUMMARY:Daily", "RRULE:FREQ=DAILY;COUNT=200")).Status);
         List<string> daily = Periods(Me("GET", "/api/v1/me/freebusy?address=" + Writer + "&from=2026-12-01&to=2027-02-01", null, Owner, Password).body);
         Assert.AreEqual(62, daily.Count, "One period a day for 62 days.");
         Assert.AreEqual("20261201T100000Z/20261201T103000Z/busy", daily.First());
         Assert.AreEqual("20270131T100000Z/20270131T103000Z/busy", daily.Last());
         Assert.AreEqual(400, Me("GET", "/api/v1/me/freebusy?address=" + Writer + "&from=2026-12-01&to=2027-02-02", null, Owner, Password).status, "63 days.");
      }

      [Test]
      [Description("The reply route steps the calendar's sync token and the event's ETag, and refuses an event the owner is not a guest of - one a phone put in with another organiser and other guests - to the owner and to a delegate alike, sending nothing")]
      public void AReplyStepsTheTokenAndNeedsTheOwnerAmongTheGuests()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, Owner, Password).status);
         long defaultId = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars", null, Owner, Password).body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + defaultId + "/shares/" + Deputy, "{\"delegate\":true}", Owner, Password).status);
         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Sync\",\"start\":\"2026-12-08T09:00\",\"end\":\"2026-12-08T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + Owner + "\"],\"invite\":true}", Organiser, Password);
         Assert.AreEqual(201, made.status, made.body);
         string window = "/api/v1/me/calendar/events?start=20261208T000000Z&end=20261209T000000Z";
         long eventId = 0;
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
         {
            (int status, string body) events = Me("GET", window, null, Owner, Password);
            RetryableAssert.StringContains("\"summary\":\"Sync\"", events.body);
            eventId = long.Parse(Regex.Match(events.body.Substring(events.body.IndexOf("\"events\":[", StringComparison.Ordinal)), "\"id\":(\\d+)").Groups[1].Value);
         });

         long tokenBefore = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars/" + defaultId, null, Owner, Password).body, "\"sync_token\":(\\d+)").Groups[1].Value);
         Response listing = Propfind(HomeOf(Owner) + "calendar/", "1", "<D:getetag/>", Auth(Owner, Password));
         string href = Hrefs(listing).First(h => h.EndsWith(".ics") && Dav("GET", h, Auth(Owner, Password)).Body.Contains("SUMMARY:Sync"));
         string etagBefore = PropText(listing, href, "D:getetag");

         Assert.AreEqual(200, Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"DECLINED\"}", Deputy, Password).status);
         long tokenAfter = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars/" + defaultId, null, Owner, Password).body, "\"sync_token\":(\\d+)").Groups[1].Value);
         Assert.Greater(tokenAfter, tokenBefore, "The collection's token steps, so a syncing phone reads the answer.");
         string etagAfter = PropText(Propfind(HomeOf(Owner) + "calendar/", "1", "<D:getetag/>", Auth(Owner, Password)), href, "D:getetag");
         Assert.AreNotEqual(etagBefore, etagAfter, "The object's ETag changes with its bytes.");
         StringAssert.Contains("SENT-BY=\"mailto:" + Deputy + "\"", Dav("GET", href, Auth(Owner, Password)).Body.Replace("\r\n ", ""), "Unfolded: the line is folded at 75 octets on the wire.");

         // An event in the calendar that another organiser runs and the owner is
         // not a guest of: nobody expects the owner's answer.
         Assert.AreEqual(201, Put(HomeOf(Owner) + "calendar/stray.ics", Owner, Ics("stray-0001", "20261208T140000Z", "20261208T150000Z", "Stray", "ORGANIZER:mailto:" + Organiser, "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + Reader)).Status);
         (int status, string body) events = Me("GET", window, null, Owner, Password);
         string stray = events.body.Split(new[] { "{\"id\":" }, StringSplitOptions.None).FirstOrDefault(item => item.Contains("\"summary\":\"Stray\"")) ?? "";
         long strayId = long.Parse(Regex.Match(stray, "^(\\d+)").Groups[1].Value);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Organiser, Password, 1);
         (int status, string body) refusedDelegate = Me("POST", "/api/v1/me/calendar/events/" + strayId + "/reply", "{\"partstat\":\"ACCEPTED\"}", Deputy, Password);
         Assert.AreEqual(400, refusedDelegate.status, refusedDelegate.body);
         StringAssert.Contains("not a guest", refusedDelegate.body);
         Assert.AreEqual(400, Me("POST", "/api/v1/me/calendar/events/" + strayId + "/reply", "{\"partstat\":\"ACCEPTED\"}", Owner, Password).status, "To the owner as well.");
         StringAssert.DoesNotContain("mailto:" + Owner, Dav("GET", HomeOf(Owner) + "calendar/stray.ics", Auth(Owner, Password)).Body, "The owner was not added as a guest.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Organiser, Password, 1);
      }

      // ------------------------------------------------------------- helpers

      private static long IdOf(string json)
      {
         Match match = Regex.Match(json, "\"id\":(\\d+)");
         Assert.IsTrue(match.Success, "No id in: " + json);
         return long.Parse(match.Groups[1].Value);
      }

      // One calendar's entry in a listing, from its opening to its sync token -
      // the last scalar before the shares, whose objects a [^{}] pattern stops
      // at. "" when the calendar is not listed.
      private static string CalendarEntry(string listing, long id)
      {
         return Regex.Match(listing, "\\{\"id\":" + id + ",.*?\"sync_token\":\\d+").Value;
      }

      // One occurrence's entry in a window, from its opening to its all_day flag,
      // the last field; a UID carries braces of its own, so no brace pattern.
      private static string EventEntry(string listing, long id)
      {
         return Regex.Match(listing, "\\{\"id\":" + id + ",.*?\"all_day\":(true|false)").Value;
      }

      // "start/end/kind" per period, in the order answered.
      private static List<string> Periods(string json)
      {
         return Regex.Matches(json, "\\{\"start\":\"([0-9TZ]+)\",\"end\":\"([0-9TZ]+)\",\"kind\":\"(busy|tentative)\"\\}").Cast<Match>()
            .Select(m => m.Groups[1].Value + "/" + m.Groups[2].Value + "/" + m.Groups[3].Value).ToList();
      }

      private static string FirstMessageKept(string address, string password)
      {
         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3.ConnectAndLogon(address, password), "POP3 sign-in as " + address);
         string text = pop3.RETR(1);
         pop3.QUIT();
         return text;
      }

      private static long MessageIdOf(string address, string password, string subjectPart)
      {
         long found = 0;
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
         {
            (int status, string body) search = Me("GET", "/api/v1/me/search?q=" + Uri.EscapeDataString(subjectPart), null, address, password);
            RetryableAssert.AreEqual(search.status, 200);
            foreach (Match item in Regex.Matches(search.body, "\"id\":(\\d+),\"uid\":[^{}]*?\"subject\":\"([^\"]*)\""))
            {
               if (item.Groups[2].Value.Contains(subjectPart))
               {
                  found = long.Parse(item.Groups[1].Value);
                  return;
               }
            }
            throw new Exception("No message with '" + subjectPart + "' in: " + search.body);
         });
         return found;
      }

      private static (int status, string body) Me(string method, string path, string body, string address, string password)
      {
         Response response = Raw(RestPort, method, path, Auth(address, password), body, "application/json", null);
         return (response.Status, response.Body);
      }

      // One VEVENT as a client writes it, with any further lines given.
      private static string Ics(string uid, string dtstart, string dtend, string summary, params string[] more)
      {
         var lines = new List<string> { "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A phone//EN", "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z", "DTSTART:" + dtstart, "DTEND:" + dtend, "SUMMARY:" + summary, "SEQUENCE:0" };
         lines.AddRange(more);
         lines.AddRange(new[] { "END:VEVENT", "END:VCALENDAR", "" });
         return string.Join("\r\n", lines);
      }

      // One VEVENT whose lines are given whole, for a zone or a rule.
      private static string IcsLines(string uid, params string[] lines)
      {
         var all = new List<string> { "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A phone//EN", "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z", "SEQUENCE:0" };
         all.AddRange(lines);
         all.AddRange(new[] { "END:VEVENT", "END:VCALENDAR", "" });
         return string.Join("\r\n", all);
      }

      // A mail carrying a METHOD:REQUEST as its whole body, as a client sends one.
      private static string Invitation(string from, string to, string subject, string summary, string uid, string dtstart, string dtend, int sequence, string organizer, params string[] attendees)
      {
         var lines = new List<string>
         {
            "From: <" + from + ">", "To: <" + to + ">", "Subject: " + subject, "MIME-Version: 1.0", "Content-Type: text/calendar; charset=utf-8; method=REQUEST", "",
            "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A client//EN", "METHOD:REQUEST", "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z",
            "DTSTART:" + dtstart, "DTEND:" + dtend, "SUMMARY:" + summary, "SEQUENCE:" + sequence, "ORGANIZER:mailto:" + organizer
         };
         foreach (string attendee in attendees)
            lines.Add("ATTENDEE;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:" + attendee);
         lines.AddRange(new[] { "END:VEVENT", "END:VCALENDAR", "" });
         return string.Join("\r\n", lines);
      }

      // One SMTP session of the test's own, so the HELO is the test's to choose.
      private static void SmtpRaw(string from, string to, string helo, string message)
      {
         using (var client = new TcpClient("127.0.0.1", SmtpPort))
         using (NetworkStream stream = client.GetStream())
         using (var reader = new StreamReader(stream, Encoding.ASCII))
         using (var writer = new StreamWriter(stream, Encoding.ASCII) { AutoFlush = true, NewLine = "\r\n" })
         {
            ExpectSmtp(reader, "220");
            writer.WriteLine("HELO " + helo);
            ExpectSmtp(reader, "250");
            writer.WriteLine("MAIL FROM:<" + from + ">");
            ExpectSmtp(reader, "250");
            writer.WriteLine("RCPT TO:<" + to + ">");
            ExpectSmtp(reader, "250");
            writer.WriteLine("DATA");
            ExpectSmtp(reader, "354");
            string text = message.Replace("\r\n.", "\r\n..");
            writer.Write(text.EndsWith("\r\n") ? text : text + "\r\n");
            writer.Write(".\r\n");
            ExpectSmtp(reader, "250");
            writer.WriteLine("QUIT");
         }
      }

      private static void ExpectSmtp(StreamReader reader, string code)
      {
         string line = reader.ReadLine();
         while (line != null && line.Length > 3 && line[3] == '-')
            line = reader.ReadLine();
         Assert.IsTrue(line != null && line.StartsWith(code), "SMTP answered " + (line ?? "nothing") + " where " + code + " was expected.");
      }

      private static Response Put(string href, string address, string text)
      {
         return Raw(WebServicesPort, "PUT", href, Auth(address, Password), text, "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\nIf-None-Match: *\r\n");
      }

      private Response Propfind(string path, string depth, string props, string authorization)
      {
         return Dav("PROPFIND", path, authorization,
            "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\"><D:prop>" + props + "</D:prop></D:propfind>",
            "Depth: " + depth + "\r\n");
      }

      private static Response Dav(string method, string path, string authorization, string body = null, string extraHeaders = null)
      {
         return Raw(WebServicesPort, method, path, authorization, body, "application/xml; charset=utf-8",
            "X-Forwarded-Proto: https\r\n" + (extraHeaders ?? ""));
      }

      private static string Auth(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static XmlDocument Parse(Response response)
      {
         var document = new XmlDocument { XmlResolver = null };
         try
         {
            document.LoadXml(response.Body);
         }
         catch (XmlException ex)
         {
            Assert.Fail("Not well-formed XML (" + ex.Message + "): " + response.Body);
         }
         return document;
      }

      private static XmlNamespaceManager Namespaces(XmlDocument document)
      {
         var namespaces = new XmlNamespaceManager(document.NameTable);
         namespaces.AddNamespace("D", NsDav);
         namespaces.AddNamespace("C", NsCalDav);
         return namespaces;
      }

      private static List<string> Hrefs(Response response)
      {
         XmlDocument document = Parse(response);
         return document.SelectNodes("/D:multistatus/D:response/D:href", Namespaces(document)).Cast<XmlNode>().Select(node => node.InnerText).ToList();
      }

      private static XmlNode Prop(Response response, string href, string propertyPath)
      {
         XmlDocument document = Parse(response);
         return document.SelectSingleNode(
            "/D:multistatus/D:response[D:href='" + href + "']/D:propstat[contains(D:status,'200')]/D:prop/" + propertyPath,
            Namespaces(document));
      }

      private static string PropText(Response response, string href, string propertyPath)
      {
         XmlNode node = Prop(response, href, propertyPath);
         Assert.IsNotNull(node, "No " + propertyPath + " for " + href + " in: " + response.Body);
         return node.InnerText;
      }

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public string HeaderText = "";
         public readonly Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
      }

      private static Response Raw(int port, string method, string path, string authorization, string body, string contentType, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", port);
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
               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1:" + port + "\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (extraHeaders != null)
                  head.Append(extraHeaders);

               byte[] bodyBytes = body == null ? new byte[0] : Encoding.UTF8.GetBytes(body);
               if (body != null)
               {
                  head.Append("Content-Type: " + contentType + "\r\n");
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
               var response = new Response();

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               response.HeaderText = separator >= 0 ? raw.Substring(0, separator) : raw;
               response.Body = separator >= 0 ? raw.Substring(separator + 4) : "";

               string[] lines = response.HeaderText.Split(new[] { "\r\n" }, StringSplitOptions.None);
               Match status = Regex.Match(lines.Length > 0 ? lines[0] : "", "^HTTP/\\d\\.\\d (\\d{3})");
               response.Status = status.Success ? int.Parse(status.Groups[1].Value) : 0;
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
