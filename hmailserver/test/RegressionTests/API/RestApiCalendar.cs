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
   ///    The calendar the webmail's pages read and write: several calendars with
   ///    colours and shares, seen the same way over CalDAV; events expanded
   ///    occurrence by occurrence across a clock change, one occurrence moved
   ///    alone; tasks ticked off; and an invitation sent by mail, answered, and
   ///    the answer filed into the organiser's event when it is delivered.
   /// </summary>
   [TestFixture]
   public class RestApiCalendar : TestFixtureBase
   {
      private const int WebServicesPort = 9106;
      private static int RestPort = 9599;
      private const string UserPassword = "Original-Passw0rd!";
      private const string GuestPassword = "Guest-Passw0rd!";
      private const string AdminPassword = "testar";

      private const string NsDav = "DAV:";
      private const string NsCalDav = "urn:ietf:params:xml:ns:caldav";
      private const string NsApple = "http://apple.com/ns/ical/";

      private Account _account;
      private Account _guest;

      private string Address
      {
         get { return "self@" + _domain.Name; }
      }

      private string GuestAddress
      {
         get { return "guest@" + _domain.Name; }
      }

      private string Home
      {
         get { return "/dav/calendars/" + Address + "/"; }
      }

      private string GuestHome
      {
         get { return "/dav/calendars/" + GuestAddress + "/"; }
      }

      [SetUp]
      public void StartListeners()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);
         _account.PersonFirstName = "Sam";
         _account.PersonLastName = "Self";
         _account.Save();
         _guest = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, GuestAddress, GuestPassword);
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

         Response probe = Dav("OPTIONS", "/dav/", Auth(Address, UserPassword));
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
      [Description("Calendars: a second one is made with a colour, renamed, shared read-only and then read-write with another account, which sees it in its listing and its CalDAV home as shared-{id} with the privileges the share grants; MKCALENDAR makes one; the default cannot be deleted and another can")]
      public void CalendarsAreMadeColouredSharedAndSeenOverCalDav()
      {
         (int status, string body) listing = Me("GET", "/api/v1/me/calendars", null, Address, UserPassword);
         Assert.AreEqual(200, listing.status, listing.body);
         StringAssert.Contains("\"name\":\"calendar\"", listing.body, "The default calendar is made on first use.");
         StringAssert.Contains("\"is_default\":true", listing.body);

         (int status, string body) made = Me("POST", "/api/v1/me/calendars", "{\"display_name\":\"Work\",\"color\":\"#1a73e8\"}", Address, UserPassword);
         Assert.AreEqual(201, made.status, made.body);
         StringAssert.Contains("\"name\":\"work\"", made.body, "The URI segment is made from the display name.");
         StringAssert.Contains("\"color\":\"#1a73e8\"", made.body);
         long workId = IdOf(made.body);

         Assert.AreEqual(409, Me("POST", "/api/v1/me/calendars", "{\"display_name\":\"Work\"}", Address, UserPassword).status, "A second calendar of the same name is refused.");
         Assert.AreEqual(400, Me("POST", "/api/v1/me/calendars", "{\"display_name\":\"Odd\",\"color\":\"blue\"}", Address, UserPassword).status, "The colour is #RRGGBB.");

         (int status, string body) renamed = Me("PUT", "/api/v1/me/calendars/" + workId, "{\"display_name\":\"Work (team)\",\"color\":\"#d93025\"}", Address, UserPassword);
         Assert.AreEqual(200, renamed.status, renamed.body);
         StringAssert.Contains("\"display_name\":\"Work (team)\"", renamed.body);
         StringAssert.Contains("\"color\":\"#d93025\"", renamed.body);

         // Shared read-only: the guest lists it, cannot write into it, and CalDAV says so.
         (int status, string body) shared = Me("PUT", "/api/v1/me/calendars/" + workId + "/shares/" + GuestAddress, "{\"write\":false}", Address, UserPassword);
         Assert.AreEqual(200, shared.status, shared.body);
         StringAssert.Contains("\"shares\":[{\"address\":\"" + GuestAddress + "\",\"write\":false}]", shared.body);
         Assert.AreEqual(404, Me("PUT", "/api/v1/me/calendars/" + workId + "/shares/nobody@" + _domain.Name, "{}", Address, UserPassword).status, "A share names an account of this server.");

         (int status, string body) guestList = Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, guestList.status, guestList.body);
         string guestEntry = Regex.Match(guestList.body, "\\{[^{}]*\"id\":" + workId + "[^{}]*\\}").Value;
         StringAssert.Contains("\"mine\":false", guestEntry, guestList.body);
         StringAssert.Contains("\"writable\":false", guestEntry);
         StringAssert.Contains("\"owner\":\"" + Address + "\"", guestEntry);

         Assert.AreEqual(403, Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + workId + ",\"summary\":\"Not mine\",\"start\":\"2026-09-20T10:00\"}", GuestAddress, GuestPassword).status,
            "A read-only share takes no event.");
         Assert.AreEqual(403, Me("PUT", "/api/v1/me/calendars/" + workId, "{\"display_name\":\"Theirs\"}", GuestAddress, GuestPassword).status, "Only the owner renames it.");

         string sharedHref = GuestHome + "shared-" + workId + "/";
         Response guestHome = Propfind(GuestHome, "1", "<D:displayname/><D:owner/><D:current-user-privilege-set/><A:calendar-color xmlns:A=\"" + NsApple + "\"/>", Auth(GuestAddress, GuestPassword));
         Assert.AreEqual(207, guestHome.Status, guestHome.Body);
         CollectionAssert.Contains(Hrefs(guestHome), sharedHref, guestHome.Body);
         Assert.AreEqual("Work (team)", PropText(guestHome, sharedHref, "D:displayname"));
         Assert.AreEqual("#d93025", PropText(guestHome, sharedHref, "A:calendar-color"));
         StringAssert.Contains("/dav/principals/" + Address + "/", PropText(guestHome, sharedHref, "D:owner"), "The owner is the sharer.");
         string privileges = Prop(guestHome, sharedHref, "D:current-user-privilege-set").InnerXml;
         StringAssert.Contains("read", privileges);
         StringAssert.DoesNotContain("write-content", privileges, "Read-only: " + privileges);

         Response refused = Raw(WebServicesPort, "PUT", sharedHref + "guest.ics", Auth(GuestAddress, GuestPassword),
            Event("guest-0001", "20260920T100000Z", "20260920T110000Z", "Not mine"), "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\nIf-None-Match: *\r\n");
         Assert.AreEqual(403, refused.Status, refused.Body);

         // Read-write: the guest may write, and its event is the owner's to see.
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + workId + "/shares/" + GuestAddress, "{\"write\":true}", Address, UserPassword).status);
         (int status, string body) written = Me("POST", "/api/v1/me/calendar/events", "{\"calendar\":" + workId + ",\"summary\":\"Team lunch\",\"start\":\"2026-09-22T12:00\",\"end\":\"2026-09-22T13:00\",\"tz\":\"UTC\"}", GuestAddress, GuestPassword);
         Assert.AreEqual(201, written.status, written.body);
         (int status, string body) ownerEvents = Me("GET", "/api/v1/me/calendar/events?start=20260922T000000Z&end=20260923T000000Z", null, Address, UserPassword);
         Assert.AreEqual(200, ownerEvents.status, ownerEvents.body);
         StringAssert.Contains("\"summary\":\"Team lunch\"", ownerEvents.body, "The owner sees what the guest wrote.");
         Response allowed = Raw(WebServicesPort, "PUT", sharedHref + "guest.ics", Auth(GuestAddress, GuestPassword),
            Event("guest-0001", "20260920T100000Z", "20260920T110000Z", "Over CalDAV"), "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\nIf-None-Match: *\r\n");
         Assert.AreEqual(201, allowed.Status, allowed.Body);

         // The share taken back: gone from the guest's listing and home.
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + workId + "/shares/" + GuestAddress, null, Address, UserPassword).status);
         StringAssert.DoesNotContain("\"id\":" + workId + ",", Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword).body);
         CollectionAssert.DoesNotContain(Hrefs(Propfind(GuestHome, "1", "<D:displayname/>", Auth(GuestAddress, GuestPassword))), sharedHref);
         Assert.AreEqual(404, Dav("PROPFIND", sharedHref, Auth(GuestAddress, GuestPassword), "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:displayname/></D:prop></D:propfind>", "Depth: 0\r\n").Status);

         // MKCALENDAR, as a phone would: listed over REST with the name and colour the body gave.
         Response mk = Dav("MKCALENDAR", Home + "projects/", Auth(Address, UserPassword),
            "<C:mkcalendar xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\" xmlns:A=\"" + NsApple + "\"><D:set><D:prop><D:displayname>Projects</D:displayname><A:calendar-color>#188038</A:calendar-color></D:prop></D:set></C:mkcalendar>");
         Assert.AreEqual(201, mk.Status, mk.Body);
         Assert.AreEqual(405, Dav("MKCALENDAR", Home + "projects/", Auth(Address, UserPassword)).Status, "A name in use is not made twice.");
         (int status, string body) three = Me("GET", "/api/v1/me/calendars", null, Address, UserPassword);
         StringAssert.Contains("\"name\":\"projects\",\"display_name\":\"Projects\",\"color\":\"#188038\"", three.body, three.body);
         Response ownHome = Propfind(Home, "1", "<D:displayname/><A:calendar-color xmlns:A=\"" + NsApple + "\"/>");
         CollectionAssert.Contains(Hrefs(ownHome), Home + "work/", ownHome.Body);
         CollectionAssert.Contains(Hrefs(ownHome), Home + "projects/", ownHome.Body);
         Assert.AreEqual("#d93025", PropText(ownHome, Home + "work/", "A:calendar-color"));

         // PROPPATCH renames and recolours; the default cannot go, another can.
         Response patched = Dav("PROPPATCH", Home + "projects/", Auth(Address, UserPassword),
            "<D:propertyupdate xmlns:D=\"DAV:\" xmlns:A=\"" + NsApple + "\"><D:set><D:prop><D:displayname>Side projects</D:displayname><A:calendar-color>#f9ab00</A:calendar-color></D:prop></D:set></D:propertyupdate>");
         Assert.AreEqual(207, patched.Status, patched.Body);
         StringAssert.Contains("HTTP/1.1 200 OK", patched.Body);
         StringAssert.Contains("\"display_name\":\"Side projects\",\"color\":\"#f9ab00\"", Me("GET", "/api/v1/me/calendars", null, Address, UserPassword).body);

         Assert.AreEqual(403, Dav("DELETE", Home + "calendar/", Auth(Address, UserPassword)).Status, "The default calendar stays.");
         Assert.AreEqual(204, Dav("DELETE", Home + "projects/", Auth(Address, UserPassword)).Status);
         long defaultId = long.Parse(Regex.Match(three.body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         Assert.AreEqual(403, Me("DELETE", "/api/v1/me/calendars/" + defaultId, null, Address, UserPassword).status);
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendars/" + workId, null, Address, UserPassword).status);
         StringAssert.DoesNotContain("\"name\":\"work\"", Me("GET", "/api/v1/me/calendars", null, Address, UserPassword).body);
         StringAssert.DoesNotContain("\"name\":\"projects\"", Me("GET", "/api/v1/me/calendars", null, Address, UserPassword).body);
      }

      [Test]
      [Description("Events: a weekly 09:00 in Europe/London with a reminder is expanded across the October clock change, one occurrence is moved alone and another removed alone, the series is renamed and then deleted; what CalDAV serves carries the TZID, the rule, the override and the exclusion")]
      public void EventsRecurAcrossAClockChangeAndOneOccurrenceMovesAlone()
      {
         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Standup\",\"location\":\"Room 4\",\"start\":\"2026-10-19T09:00\",\"end\":\"2026-10-19T09:30\",\"tz\":\"Europe/London\",\"rrule\":\"FREQ=WEEKLY;COUNT=3\",\"alarm_minutes\":10}",
            Address, UserPassword);
         Assert.AreEqual(201, made.status, made.body);
         long id = IdOf(made.body);

         string window = "/api/v1/me/calendar/events?start=20261018T000000Z&end=20261110T000000Z";
         (int status, string body) events = Me("GET", window, null, Address, UserPassword);
         Assert.AreEqual(200, events.status, events.body);
         List<string> starts = Starts(events.body);
         CollectionAssert.AreEqual(new[] { "20261019T080000Z", "20261026T090000Z", "20261102T090000Z" }, starts,
            "09:00 London is 08:00Z in summer time and 09:00Z after the clocks go back. Body: " + events.body);
         StringAssert.Contains("\"local_start\":\"2026-10-26T09:00\"", events.body, "The wall clock in the event's zone.");
         StringAssert.Contains("\"alarm_minutes\":10", events.body);
         StringAssert.Contains("\"tz\":\"Europe/London\"", events.body);
         StringAssert.Contains("\"recurrence_id\":\"20261026T090000Z\"", events.body);
         StringAssert.Contains("\"location\":\"Room 4\"", events.body);

         // One occurrence moved to the afternoon: an override, the others untouched.
         (int status, string body) moved = Me("PUT", "/api/v1/me/calendar/events/" + id,
            "{\"instance\":\"20261026T090000Z\",\"start\":\"2026-10-26T14:00\",\"end\":\"2026-10-26T14:30\"}", Address, UserPassword);
         Assert.AreEqual(200, moved.status, moved.body);
         events = Me("GET", window, null, Address, UserPassword);
         CollectionAssert.AreEqual(new[] { "20261019T080000Z", "20261026T140000Z", "20261102T090000Z" }, Starts(events.body), events.body);
         // The moved occurrence's own object: split on the objects' opening, as
         // a uid carries braces of its own and a [^{}] pattern stops at them.
         string overridden = events.body.Split(new[] { "{\"id\":" }, StringSplitOptions.None)
            .FirstOrDefault(item => item.Contains("\"start\":\"20261026T140000Z\"")) ?? "";
         StringAssert.Contains("\"overridden\":true", overridden, events.body);
         Assert.AreEqual(404, Me("PUT", "/api/v1/me/calendar/events/" + id, "{\"instance\":\"20261027T090000Z\",\"summary\":\"x\"}", Address, UserPassword).status, "No occurrence starts then.");

         // One occurrence removed alone.
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendar/events/" + id + "?instance=20261102T090000Z", null, Address, UserPassword).status);
         events = Me("GET", window, null, Address, UserPassword);
         CollectionAssert.AreEqual(new[] { "20261019T080000Z", "20261026T140000Z" }, Starts(events.body), events.body);

         // What a phone reads over CalDAV.
         Response listing = Propfind(Home + "calendar/", "1", "<D:getetag/>");
         string href = Hrefs(listing).First(h => h.EndsWith(".ics"));
         Response stored = Dav("GET", href, Auth(Address, UserPassword));
         Assert.AreEqual(200, stored.Status, stored.Body);
         StringAssert.Contains("DTSTART;TZID=Europe/London:20261019T090000", stored.Body);
         StringAssert.Contains("RRULE:FREQ=WEEKLY;COUNT=3", stored.Body);
         StringAssert.Contains("RECURRENCE-ID:20261026T090000Z", stored.Body);
         StringAssert.Contains("DTSTART;TZID=Europe/London:20261026T140000", stored.Body);
         StringAssert.Contains("EXDATE;TZID=Europe/London:20261102T090000", stored.Body);
         StringAssert.Contains("TRIGGER:-PT10M", stored.Body);
         StringAssert.Contains("LOCATION:Room 4", stored.Body);

         // The series renamed, then gone.
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendar/events/" + id, "{\"summary\":\"Daily standup\",\"alarm_minutes\":-1}", Address, UserPassword).status);
         events = Me("GET", window, null, Address, UserPassword);
         Assert.AreEqual(2, Regex.Matches(events.body, "\"summary\":\"Daily standup\"").Count, "The override keeps its own summary when it had one - it had the master's - so both say it. Body: " + events.body);
         StringAssert.DoesNotContain("\"alarm_minutes\":10", events.body);

         (int status, string body) one = Me("GET", "/api/v1/me/calendar/events/" + id, null, Address, UserPassword);
         Assert.AreEqual(200, one.status, one.body);
         StringAssert.Contains("\"recurring\":true", one.body);
         StringAssert.Contains("BEGIN:VCALENDAR", one.body);

         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendar/events/" + id, null, Address, UserPassword).status);
         Assert.AreEqual(404, Me("DELETE", "/api/v1/me/calendar/events/" + id, null, Address, UserPassword).status);
         Assert.AreEqual(0, Starts(Me("GET", window, null, Address, UserPassword).body).Count);

         // An all-day event spans its day, and a bad window is refused.
         (int status, string body) allDay = Me("POST", "/api/v1/me/calendar/events", "{\"summary\":\"Away\",\"start\":\"2026-12-24\",\"all_day\":true}", Address, UserPassword);
         Assert.AreEqual(201, allDay.status, allDay.body);
         (int status, string body) december = Me("GET", "/api/v1/me/calendar/events?start=20261224T000000Z&end=20261225T000000Z", null, Address, UserPassword);
         StringAssert.Contains("\"all_day\":true", december.body, december.body);
         StringAssert.Contains("\"local_start\":\"2026-12-24\",\"local_end\":\"2026-12-25\"", december.body, december.body);
         Assert.AreEqual(400, Me("GET", "/api/v1/me/calendar/events?start=20261224T000000Z", null, Address, UserPassword).status);
         Assert.AreEqual(400, Me("POST", "/api/v1/me/calendar/events", "{\"summary\":\"Nowhere\",\"start\":\"2026-12-24T10:00\",\"tz\":\"Mars/Olympus\"}", Address, UserPassword).status);
      }

      [Test]
      [Description("Tasks: added with a due day, listed soonest first, ticked off as a client writes completion; a VTODO a phone put over CalDAV is in the list, and a task deleted is gone from both")]
      public void TasksAreListedTickedOffAndSharedWithCalDav()
      {
         (int status, string body) milk = Me("POST", "/api/v1/me/tasks", "{\"summary\":\"Buy milk\",\"due\":\"2026-09-20\"}", Address, UserPassword);
         Assert.AreEqual(201, milk.status, milk.body);
         StringAssert.Contains("\"local_due\":\"2026-09-20\"", milk.body);
         StringAssert.Contains("\"all_day\":true", milk.body);
         StringAssert.Contains("\"completed\":false", milk.body);
         long milkId = IdOf(milk.body);

         (int status, string body) report = Me("POST", "/api/v1/me/tasks", "{\"summary\":\"Write the report\",\"due\":\"2026-09-18T17:00\",\"tz\":\"Europe/London\",\"priority\":1,\"description\":\"Two pages.\"}", Address, UserPassword);
         Assert.AreEqual(201, report.status, report.body);
         StringAssert.Contains("\"due\":\"20260918T160000Z\"", report.body, "17:00 London in September is 16:00Z.");
         StringAssert.Contains("\"priority\":1", report.body);

         Response phone = Raw(WebServicesPort, "PUT", Home + "calendar/bank.ics", Auth(Address, UserPassword),
            string.Join("\r\n", new[] { "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A phone//EN", "BEGIN:VTODO", "UID:bank-0001", "DTSTAMP:20260101T000000Z", "DUE:20260921T090000Z", "SUMMARY:Call the bank", "STATUS:NEEDS-ACTION", "END:VTODO", "END:VCALENDAR", "" }),
            "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\nIf-None-Match: *\r\n");
         Assert.AreEqual(201, phone.Status, phone.Body);

         (int status, string body) tasks = Me("GET", "/api/v1/me/tasks", null, Address, UserPassword);
         Assert.AreEqual(200, tasks.status, tasks.body);
         List<string> summaries = Regex.Matches(tasks.body, "\"summary\":\"([^\"]+)\"").Cast<Match>().Select(m => m.Groups[1].Value).ToList();
         CollectionAssert.AreEqual(new[] { "Write the report", "Buy milk", "Call the bank" }, summaries, "Soonest due first. Body: " + tasks.body);

         (int status, string body) done = Me("PUT", "/api/v1/me/tasks/" + milkId, "{\"completed\":true}", Address, UserPassword);
         Assert.AreEqual(200, done.status, done.body);
         StringAssert.Contains("\"completed\":true", done.body);
         StringAssert.Contains("\"status\":\"COMPLETED\"", done.body);

         Response listing = Propfind(Home + "calendar/", "1", "<D:getetag/>");
         string href = Hrefs(listing).First(h => h.EndsWith(".ics") && !h.EndsWith("bank.ics") && Dav("GET", h, Auth(Address, UserPassword)).Body.Contains("Buy milk"));
         string stored = Dav("GET", href, Auth(Address, UserPassword)).Body;
         StringAssert.Contains("STATUS:COMPLETED", stored);
         StringAssert.Contains("PERCENT-COMPLETE:100", stored);
         StringAssert.Contains("COMPLETED:", stored);
         StringAssert.Contains("DUE;VALUE=DATE:20260920", stored);

         Assert.AreEqual(200, Me("PUT", "/api/v1/me/tasks/" + milkId, "{\"completed\":false,\"summary\":\"Buy oat milk\",\"due\":\"\"}", Address, UserPassword).status);
         tasks = Me("GET", "/api/v1/me/tasks", null, Address, UserPassword);
         StringAssert.Contains("\"summary\":\"Buy oat milk\",\"description\":\"\",\"due\":\"\"", tasks.body, tasks.body);

         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/tasks/" + milkId, null, Address, UserPassword).status);
         Assert.AreEqual(404, Me("DELETE", "/api/v1/me/tasks/" + milkId, null, Address, UserPassword).status);
         StringAssert.DoesNotContain("Buy oat milk", Me("GET", "/api/v1/me/tasks", null, Address, UserPassword).body);
         Assert.AreEqual(404, Dav("GET", href, Auth(Address, UserPassword)).Status);
         // Not a task id at all, so no route: an account's credential is refused
         // a path it has no route for, rather than told of a task that is missing.
         Assert.AreEqual(403, Me("PUT", "/api/v1/me/tasks/" + IdOf(report.body) + "x", "{}", Address, UserPassword).status);
      }

      [Test]
      [Description("iMIP: an event with an attendee mails a REQUEST; the attendee's page reads the invitation, accepts it into their calendar and mails a REPLY that the server files into the organiser's event on delivery; a counter-proposal reaches the organiser; a cancellation is mailed, filed as cancelled and removed")]
      public void AnInvitationIsSentAnsweredAndFiled()
      {
         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Planning\",\"start\":\"2026-10-19T09:00\",\"end\":\"2026-10-19T10:00\",\"tz\":\"Europe/London\",\"attendees\":[\"Guest Person <" + GuestAddress + ">\"],\"invite\":true,\"description\":\"Bring numbers.\"}",
            Address, UserPassword);
         Assert.AreEqual(201, made.status, made.body);
         StringAssert.Contains("\"invited\":1", made.body);
         long organiserEventId = IdOf(made.body);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(GuestAddress, GuestPassword, 1);
         string invitationMail = FirstMessageKept(GuestAddress, GuestPassword);
         StringAssert.Contains("Subject: Invitation: Planning @", invitationMail);
         StringAssert.Contains("text/calendar; charset=utf-8; method=REQUEST", invitationMail);
         // A display name is written quoted, as RFC 5322 allows any phrase to be.
         StringAssert.Contains("From: \"Sam Self\" <" + Address + ">", invitationMail);

         long messageId = MessageIdOf(GuestAddress, GuestPassword, "Planning");
         (int status, string body) message = Me("GET", "/api/v1/me/messages/" + messageId, null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, message.status, message.body);
         StringAssert.Contains("\"invitation\":true", message.body, "The message JSON says an invitation is inside.");

         (int status, string body) invitation = Me("GET", "/api/v1/me/messages/" + messageId + "/invitation", null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, invitation.status, invitation.body);
         StringAssert.Contains("\"method\":\"REQUEST\"", invitation.body);
         StringAssert.Contains("\"summary\":\"Planning\"", invitation.body);
         StringAssert.Contains("\"start\":\"20261019T080000Z\"", invitation.body);
         StringAssert.Contains("\"local_start\":\"2026-10-19T09:00\"", invitation.body);
         StringAssert.Contains("\"organizer\":\"" + Address + "\"", invitation.body);
         StringAssert.Contains("\"organizer_name\":\"Sam Self\"", invitation.body);
         StringAssert.Contains("\"in_calendar\":false", invitation.body);
         StringAssert.Contains("\"my_partstat\":\"NEEDS-ACTION\"", invitation.body);
         StringAssert.Contains("\"description\":\"Bring numbers.\"", invitation.body);

         // Accepted: in the guest's calendar with the answer, and a REPLY on its way.
         (int status, string body) accepted = Me("POST", "/api/v1/me/messages/" + messageId + "/invitation/reply", "{\"action\":\"accept\"}", GuestAddress, GuestPassword);
         Assert.AreEqual(200, accepted.status, accepted.body);
         StringAssert.Contains("\"stored\":true", accepted.body);
         StringAssert.Contains("\"partstat\":\"ACCEPTED\",\"replied\":true,\"to\":\"" + Address + "\"", accepted.body);

         (int status, string body) guestEvents = Me("GET", "/api/v1/me/calendar/events?start=20261018T000000Z&end=20261020T000000Z", null, GuestAddress, GuestPassword);
         StringAssert.Contains("\"summary\":\"Planning\"", guestEvents.body, guestEvents.body);
         StringAssert.Contains("\"my_partstat\":\"ACCEPTED\"", guestEvents.body);
         StringAssert.Contains("\"in_calendar\":true", Me("GET", "/api/v1/me/messages/" + messageId + "/invitation", null, GuestAddress, GuestPassword).body);

         // The REPLY delivered to the organiser is filed into their event.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);
         string replyMail = FirstMessageKept(Address, UserPassword);
         StringAssert.Contains("Subject: Accepted: Planning @", replyMail);
         StringAssert.Contains("method=REPLY", replyMail);
         RetryHelper.TryAction(TimeSpan.FromSeconds(15), () =>
         {
            (int status, string body) organiserEvent = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword);
            RetryableAssert.StringContains("\"address\":\"" + GuestAddress + "\",\"name\":\"Guest Person\",\"partstat\":\"ACCEPTED\"", organiserEvent.body);
         });
         long replyId = MessageIdOf(Address, UserPassword, "Accepted: Planning");
         (int status, string body) filed = Me("GET", "/api/v1/me/messages/" + replyId + "/invitation", null, Address, UserPassword);
         Assert.AreEqual(200, filed.status, filed.body);
         StringAssert.Contains("\"method\":\"REPLY\"", filed.body);
         StringAssert.Contains("\"i_am_organizer\":true", filed.body);
         Assert.AreEqual(400, Me("POST", "/api/v1/me/messages/" + replyId + "/invitation/reply", "{\"action\":\"accept\"}", Address, UserPassword).status, "The organiser does not answer their own event.");

         // A counter-proposal from the guest reaches the organiser as METHOD:COUNTER.
         (int status, string body) countered = Me("POST", "/api/v1/me/messages/" + messageId + "/invitation/reply",
            "{\"action\":\"counter\",\"start\":\"2026-10-19T11:00\",\"end\":\"2026-10-19T12:00\",\"tz\":\"Europe/London\",\"comment\":\"Later suits me.\"}", GuestAddress, GuestPassword);
         Assert.AreEqual(200, countered.status, countered.body);
         StringAssert.Contains("\"countered\":true", countered.body);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 2);
         long counterId = MessageIdOf(Address, UserPassword, "New time proposed: Planning");
         (int status, string body) counter = Me("GET", "/api/v1/me/messages/" + counterId + "/invitation", null, Address, UserPassword);
         StringAssert.Contains("\"method\":\"COUNTER\"", counter.body, counter.body);
         StringAssert.Contains("\"start\":\"20261019T100000Z\"", counter.body, "11:00 London is 10:00Z.");

         // Cancelled by the organiser: the guest is told, the stored copy is marked, and remove takes it out.
         Assert.AreEqual(200, Me("DELETE", "/api/v1/me/calendar/events/" + organiserEventId + "?notify=1", null, Address, UserPassword).status);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(GuestAddress, GuestPassword, 2);
         long cancelId = MessageIdOf(GuestAddress, GuestPassword, "Cancelled: Planning");
         RetryHelper.TryAction(TimeSpan.FromSeconds(15), () =>
         {
            (int status, string body) cancelled = Me("GET", "/api/v1/me/messages/" + cancelId + "/invitation", null, GuestAddress, GuestPassword);
            RetryableAssert.StringContains("\"method\":\"CANCEL\"", cancelled.body);
            RetryableAssert.StringContains("\"stored_status\":\"CANCELLED\"", cancelled.body);
         });
         (int status, string body) removed = Me("POST", "/api/v1/me/messages/" + cancelId + "/invitation/reply", "{\"action\":\"remove\"}", GuestAddress, GuestPassword);
         Assert.AreEqual(200, removed.status, removed.body);
         StringAssert.Contains("\"removed\":true", removed.body);
         Assert.AreEqual(0, Starts(Me("GET", "/api/v1/me/calendar/events?start=20261018T000000Z&end=20261020T000000Z", null, GuestAddress, GuestPassword).body).Count);

         Assert.AreEqual(404, Me("GET", "/api/v1/me/messages/" + messageId + "x/invitation", null, GuestAddress, GuestPassword).status);
      }

      // ------------------------------------------------------------- helpers

      private static long IdOf(string json)
      {
         Match match = Regex.Match(json, "\"id\":(\\d+)");
         Assert.IsTrue(match.Success, "No id in: " + json);
         return long.Parse(match.Groups[1].Value);
      }

      private static List<string> Starts(string json)
      {
         // The occurrences' starts, not the window's: the answer opens with the
         // start of the window it was asked for, in the same form.
         int events = json.IndexOf("\"events\":[", StringComparison.Ordinal);
         string occurrences = events >= 0 ? json.Substring(events) : json;
         return Regex.Matches(occurrences, "\"start\":\"([0-9]{8}T[0-9]{6}Z)\"").Cast<Match>().Select(match => match.Groups[1].Value).ToList();
      }

      // The first message's text, left where it is: the helper that waits for a
      // message also deletes it, and the searches that follow look for it there.
      private static string FirstMessageKept(string address, string password)
      {
         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3.ConnectAndLogon(address, password), "POP3 sign-in as " + address);
         string text = pop3.RETR(1);
         pop3.QUIT();
         return text;
      }

      // The id of the first message whose subject carries the text, by the
      // account's own search route; waits for delivery.
      private static long MessageIdOf(string address, string password, string subjectPart)
      {
         long found = 0;
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
         {
            (int status, string body) search = Me("GET", "/api/v1/me/search?q=" + Uri.EscapeDataString(subjectPart), null, address, password);
            RetryableAssert.AreEqual(search.status, 200);
            // A search hit opens with its folder, so the id is found by the uid
            // that follows it rather than by the object's opening brace.
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

      private static string Event(string uid, string dtstart, string dtend, string summary)
      {
         return string.Join("\r\n", new[]
         {
            "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A phone//EN", "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z",
            "DTSTART:" + dtstart, "DTEND:" + dtend, "SUMMARY:" + summary, "SEQUENCE:0", "END:VEVENT", "END:VCALENDAR", ""
         });
      }

      private Response Propfind(string path, string depth, string props, string authorization = null)
      {
         return Dav("PROPFIND", path, authorization ?? Auth(Address, UserPassword),
            "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\" xmlns:CS=\"http://calendarserver.org/ns/\"><D:prop>" + props + "</D:prop></D:propfind>",
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
         namespaces.AddNamespace("A", NsApple);
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
