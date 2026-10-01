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
      private const string CoGuestPassword = "CoGuest-Passw0rd!";
      private const string AdminPassword = "testar";
      private const int SmtpPort = 25;

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
         StringAssert.Contains("\"shares\":[{\"address\":\"" + GuestAddress + "\",\"write\":false,\"delegate\":false}]", shared.body);
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

         // Not a message id, so not a route: 403 to an account's credential, as for a task path.
         Assert.AreEqual(403, Me("GET", "/api/v1/me/messages/" + messageId + "x/invitation", null, GuestAddress, GuestPassword).status);
      }

      [Test]
      [Description("iMIP on delivery changes a stored event only for the sender the server verified: a CANCEL in the organiser's name from nobody signed in, the same from a co-guest signed in as itself, and an updated REQUEST in the organiser's name from nobody signed in that would move the event and mark it cancelled, all leave the guest's copy as it was; a REPLY in the guest's name from nobody signed in, and a co-guest's signed-in REPLY answering for the guest - plainly, and as a delegate it is not - and for an address the event does not list, change nothing but the co-guest's own answer, and an account the event does not list, signed in, is not added by answering for itself; the guest's own answers, from the message's card and from a mail program signed in, and the organiser's own CANCEL signed in, are filed")]
      public void OnlyTheVerifiedSenderChangesAStoredEvent()
      {
         string coGuest = "coguest@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, coGuest, CoGuestPassword);
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword).status, "The guest's calendar exists before the invitation arrives, so it is filed on delivery.");

         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Board\",\"start\":\"2026-10-26T09:00\",\"end\":\"2026-10-26T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + GuestAddress + "\",\"" + coGuest + "\"],\"invite\":true}",
            Address, UserPassword);
         Assert.AreEqual(201, made.status, made.body);
         StringAssert.Contains("\"invited\":2", made.body);
         long organiserEventId = IdOf(made.body);
         string uid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
         Assert.IsTrue(uid.Length > 0, "No uid in: " + made.body);
         string window = "/api/v1/me/calendar/events?start=20261026T000000Z&end=20261027T000000Z";
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Board\"", Me("GET", window, null, GuestAddress, GuestPassword).body));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // At the guest: a CANCEL in the organiser's name - MAIL FROM, From and
         // ORGANIZER - from nobody signed in; the same from the co-guest, signed
         // in as itself; and an updated REQUEST in the organiser's name from
         // nobody signed in, at a higher SEQUENCE, moving the event and marking
         // it cancelled.
         string[] guests = { "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + GuestAddress, "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + coGuest };
         SmtpRaw(Address, GuestAddress, CalendarMail("CANCEL", Address, GuestAddress, "Cancelled: Board forged one", "Board", uid, "20261026T090000Z", "20261026T100000Z", 1, Address, guests));
         SmtpRaw(coGuest, GuestAddress, CalendarMail("CANCEL", Address, GuestAddress, "Cancelled: Board forged two", "Board", uid, "20261026T090000Z", "20261026T100000Z", 1, Address, guests), coGuest, CoGuestPassword);
         SmtpRaw(Address, GuestAddress, CalendarMail("REQUEST", Address, GuestAddress, "Updated invitation: Board forged three", "Board", uid, "20261026T150000Z", "20261026T160000Z", 7, Address,
            guests.Concat(new[] { "STATUS:CANCELLED" }).ToArray()));
         long forgedCancel = MessageIdOf(GuestAddress, GuestPassword, "Board forged one");
         MessageIdOf(GuestAddress, GuestPassword, "Board forged two");
         long forgedUpdate = MessageIdOf(GuestAddress, GuestPassword, "Board forged three");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         (int status, string body) guestCopy = Me("GET", window, null, GuestAddress, GuestPassword);
         Assert.AreEqual(1, Regex.Matches(guestCopy.body, "\"uid\":\"" + Regex.Escape(uid) + "\"").Count, "One event. Body: " + guestCopy.body);
         StringAssert.DoesNotContain("\"status\":\"CANCELLED\"", guestCopy.body, "Nobody the server verified as the organiser cancelled it. Body: " + guestCopy.body);
         StringAssert.Contains("\"start\":\"20261026T090000Z\"", guestCopy.body, "Nor moved it.");
         (int status, string body) forgedCard = Me("GET", "/api/v1/me/messages/" + forgedCancel + "/invitation", null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, forgedCard.status, forgedCard.body);
         StringAssert.Contains("\"in_calendar\":true", forgedCard.body, "The forged cancellation is an ordinary message: the card finds the event...");
         StringAssert.DoesNotContain("\"stored_status\":\"CANCELLED\"", forgedCard.body, "...as it was.");
         StringAssert.Contains("\"calendar_agrees\":false", forgedCard.body, "And says the calendar does not hold the cancellation, so the page does not say it does.");
         (int status, string body) forgedUpdateCard = Me("GET", "/api/v1/me/messages/" + forgedUpdate + "/invitation", null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, forgedUpdateCard.status, forgedUpdateCard.body);
         StringAssert.Contains("\"calendar_agrees\":false", forgedUpdateCard.body, "The calendar does not have the forged update's version of the event.");

         // At the organiser: a REPLY in the guest's name from nobody signed in;
         // and the co-guest, signed in, declining for the guest - plainly, and
         // with itself as SENT-BY, a delegate the guest never named - accepting
         // for itself, and accepting for an address the event does not list.
         SmtpRaw(GuestAddress, Address, CalendarMail("REPLY", GuestAddress, Address, "Declined: Board forged four", "Board", uid, "20261026T090000Z", "20261026T100000Z", 0, Address,
            "ATTENDEE;PARTSTAT=DECLINED:mailto:" + GuestAddress));
         SmtpRaw(coGuest, Address, CalendarMail("REPLY", coGuest, Address, "Answers: Board forged five", "Board", uid, "20261026T090000Z", "20261026T100000Z", 0, Address,
            "ATTENDEE;PARTSTAT=DECLINED:mailto:" + GuestAddress,
            "ATTENDEE;SENT-BY=\"mailto:" + coGuest + "\";PARTSTAT=DECLINED:mailto:" + GuestAddress,
            "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + coGuest,
            "ATTENDEE;PARTSTAT=ACCEPTED:mailto:stranger@elsewhere.test"), coGuest, CoGuestPassword);
         // And an account the event does not list, signed in, accepting for itself.
         string uninvited = "uninvited@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, uninvited, CoGuestPassword);
         SmtpRaw(uninvited, Address, CalendarMail("REPLY", uninvited, Address, "Accepted: Board forged six", "Board", uid, "20261026T090000Z", "20261026T100000Z", 0, Address,
            "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + uninvited), uninvited, CoGuestPassword);
         long forgedReply = MessageIdOf(Address, UserPassword, "Board forged four");
         MessageIdOf(Address, UserPassword, "Board forged five");
         MessageIdOf(Address, UserPassword, "Board forged six");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         RetryHelper.TryAction(TimeSpan.FromSeconds(15), () =>
            RetryableAssert.StringContains("ACCEPTED", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword).body, coGuest)));
         (int status, string body) organiserEvent = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword);
         Assert.AreEqual("NEEDS-ACTION", PartstatOf(organiserEvent.body, GuestAddress), "Nobody the server verified as the guest, or as a delegate of the guest's, answered for the guest. Body: " + organiserEvent.body);
         Assert.AreEqual("", SentByOf(organiserEvent.body, GuestAddress), "No SENT-BY was taken from a sender that is not the guest's delegate.");
         StringAssert.DoesNotContain("stranger@elsewhere.test", organiserEvent.body, "A REPLY does not add a guest the event does not list.");
         StringAssert.DoesNotContain(uninvited, organiserEvent.body, "Nor does one from that address itself, signed in.");
         StringAssert.Contains("\"calendar_agrees\":false", Me("GET", "/api/v1/me/messages/" + forgedReply + "/invitation", null, Address, UserPassword).body,
            "The forged answer's card does not say the event shows it.");

         // The guest's own answers: from the message's card, which the page
         // sends as the guest's submission, and from a mail program signed in.
         long invitationId = MessageIdOf(GuestAddress, GuestPassword, "Invitation: Board");
         Assert.AreEqual(200, Me("POST", "/api/v1/me/messages/" + invitationId + "/invitation/reply", "{\"action\":\"tentative\"}", GuestAddress, GuestPassword).status);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
            RetryableAssert.StringContains("TENTATIVE", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword).body, GuestAddress)));
         SmtpRaw(GuestAddress, Address, CalendarMail("REPLY", GuestAddress, Address, "Accepted: Board from a mail program", "Board", uid, "20261026T090000Z", "20261026T100000Z", 0, Address,
            "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + GuestAddress), GuestAddress, GuestPassword);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
            RetryableAssert.StringContains("ACCEPTED", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword).body, GuestAddress)));
         StringAssert.Contains("\"calendar_agrees\":true", Me("GET", "/api/v1/me/messages/" + MessageIdOf(Address, UserPassword, "Board from a mail program") + "/invitation", null, Address, UserPassword).body,
            "The guest's own answer's card says the event shows it.");

         // The organiser's own CANCEL, signed in: the guest's copy is marked cancelled.
         SmtpRaw(Address, GuestAddress, CalendarMail("CANCEL", Address, GuestAddress, "Cancelled: Board, signed in", "Board", uid, "20261026T090000Z", "20261026T100000Z", 1, Address, guests), Address, UserPassword);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"status\":\"CANCELLED\"", Me("GET", window, null, GuestAddress, GuestPassword).body));
         StringAssert.Contains("\"calendar_agrees\":true", Me("GET", "/api/v1/me/messages/" + MessageIdOf(GuestAddress, GuestPassword, "Board, signed in") + "/invitation", null, GuestAddress, GuestPassword).body,
            "The organiser's own cancellation's card says the calendar holds it.");
      }

      [Test]
      [Description("An answer from the account an alias the event lists points at is the attendee's: the organiser invites the guest by an alias, the guest answers from the message's card - as the account's own address - and the organiser's event shows the answer on the alias it lists, adding nobody; a co-guest's answer naming the guest's address changes nothing")]
      public void AnAnswerFromTheAccountOfAnInvitedAliasIsFiled()
      {
         string alias = "g.smith@" + _domain.Name;
         Alias guestAlias = _domain.Aliases.Add();
         guestAlias.Name = alias;
         guestAlias.Value = GuestAddress;
         guestAlias.Active = true;
         guestAlias.Save();
         string coGuest = "coguest@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, coGuest, CoGuestPassword);
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword).status, "The guest's calendar exists before the invitation arrives.");

         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Budget by alias\",\"start\":\"2026-10-27T09:00\",\"end\":\"2026-10-27T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + alias + "\",\"" + coGuest + "\"],\"invite\":true}",
            Address, UserPassword);
         Assert.AreEqual(201, made.status, made.body);
         long organiserEventId = IdOf(made.body);
         string uid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
         Assert.IsTrue(uid.Length > 0, "No uid in: " + made.body);

         // A co-guest, signed in, answering in the guest's own address: the
         // event lists the alias, which is not the co-guest's to answer for.
         SmtpRaw(coGuest, Address, CalendarMail("REPLY", coGuest, Address, "Declined: Budget by alias, forged", "Budget by alias", uid, "20261027T090000Z", "20261027T100000Z", 0, Address,
            "ATTENDEE;PARTSTAT=DECLINED:mailto:" + GuestAddress), coGuest, CoGuestPassword);
         MessageIdOf(Address, UserPassword, "Budget by alias, forged");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         (int status, string body) untouched = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword);
         Assert.AreEqual("NEEDS-ACTION", PartstatOf(untouched.body, alias), "Nobody the server verified as the guest answered. Body: " + untouched.body);
         Assert.AreEqual("", PartstatOf(untouched.body, GuestAddress), "And nobody was added. Body: " + untouched.body);

         // The guest answers from the invitation's card: the REPLY names the
         // account's own address, and the event lists the alias.
         long invitationId = MessageIdOf(GuestAddress, GuestPassword, "Invitation: Budget by alias");
         Assert.AreEqual(200, Me("POST", "/api/v1/me/messages/" + invitationId + "/invitation/reply", "{\"action\":\"accept\"}", GuestAddress, GuestPassword).status);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
            RetryableAssert.StringContains("ACCEPTED", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword).body, alias)));
         (int status, string body) answered = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword);
         Assert.AreEqual("", PartstatOf(answered.body, GuestAddress), "The answer is on the alias the event lists, and the account's own address is not added. Body: " + answered.body);
         StringAssert.Contains("\"calendar_agrees\":true", Me("GET", "/api/v1/me/messages/" + MessageIdOf(Address, UserPassword, "Accepted: Budget by alias") + "/invitation", null, Address, UserPassword).body,
            "The answer's card says the event shows it.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("An answer from a member of a distribution list the event lists is filed beside the list: the organiser invites a list of this server's, a member answers from the message's card, and the organiser's event shows the member's answer; a co-guest's answer in the member's name, and an account that is not in the list answering for itself, signed in, add nobody")]
      public void AnAnswerFromAMemberOfAnInvitedListIsFiled()
      {
         string team = "team@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, team, new List<string> { GuestAddress });
         string coGuest = "coguest@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, coGuest, CoGuestPassword);
         string outsider = "outsider@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, outsider, CoGuestPassword);

         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Team review\",\"start\":\"2026-10-30T09:00\",\"end\":\"2026-10-30T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + team + "\",\"" + coGuest + "\"],\"invite\":true}",
            Address, UserPassword);
         Assert.AreEqual(201, made.status, made.body);
         long organiserEventId = IdOf(made.body);
         string uid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
         Assert.IsTrue(uid.Length > 0, "No uid in: " + made.body);
         long invitationId = MessageIdOf(GuestAddress, GuestPassword, "Invitation: Team review");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // A co-guest, signed in, answering in the member's name; an account
         // that is not in the list, signed in, answering for itself.
         SmtpRaw(coGuest, Address, CalendarMail("REPLY", coGuest, Address, "Declined: Team review, forged", "Team review", uid, "20261030T090000Z", "20261030T100000Z", 0, Address,
            "ATTENDEE;PARTSTAT=DECLINED:mailto:" + GuestAddress), coGuest, CoGuestPassword);
         SmtpRaw(outsider, Address, CalendarMail("REPLY", outsider, Address, "Accepted: Team review, not in the list", "Team review", uid, "20261030T090000Z", "20261030T100000Z", 0, Address,
            "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + outsider), outsider, CoGuestPassword);
         MessageIdOf(Address, UserPassword, "Team review, forged");
         MessageIdOf(Address, UserPassword, "Team review, not in the list");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         (int status, string body) untouched = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword);
         Assert.AreEqual("", PartstatOf(untouched.body, GuestAddress), "Nobody the server verified as the member answered. Body: " + untouched.body);
         StringAssert.DoesNotContain(outsider, untouched.body, "An account that is not in the list is not added by answering. Body: " + untouched.body);

         // The member answers from the invitation's card, in the account's own address.
         Assert.AreEqual(200, Me("POST", "/api/v1/me/messages/" + invitationId + "/invitation/reply", "{\"action\":\"accept\"}", GuestAddress, GuestPassword).status);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
            RetryableAssert.StringContains("ACCEPTED", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword).body, GuestAddress)));
         (int status, string body) answered = Me("GET", "/api/v1/me/calendar/events/" + organiserEventId, null, Address, UserPassword);
         Assert.AreEqual("NEEDS-ACTION", PartstatOf(answered.body, team), "The list's own entry is as it was. Body: " + answered.body);
         StringAssert.Contains("\"calendar_agrees\":true", Me("GET", "/api/v1/me/messages/" + MessageIdOf(Address, UserPassword, "Accepted: Team review @") + "/invitation", null, Address, UserPassword).body,
            "The member's answer's card says the event shows it.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("A copy filed from an invitation nobody verified does not hold off the organiser's own by the SEQUENCE its writer gave it: a REQUEST in the organiser's name from nobody signed in, at SEQUENCE 40 and another time, reaches a guest before the organiser invites them and is filed; the organiser's own invitation, signed in, at SEQUENCE 0, replaces it - the planted message's card then not agreeing with the calendar, the organiser's agreeing - and another such REQUEST, at SEQUENCE 60, changes nothing")]
      public void AnUnverifiedCopyDoesNotHoldOffTheOrganisersInvitation()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword).status, "The guest's calendar exists before the invitations arrive.");
         string uid = "planted-0001@" + _domain.Name;
         string window = "/api/v1/me/calendar/events?start=20261031T000000Z&end=20261101T000000Z";
         string guestLine = "ATTENDEE;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:" + GuestAddress;

         // Planted: the organiser's name as ORGANIZER, From and MAIL FROM, by nobody signed in.
         SmtpRaw(Address, GuestAddress, CalendarMail("REQUEST", Address, GuestAddress, "Invitation: Planted review", "Planted", uid, "20261031T160000Z", "20261031T170000Z", 40, Address, guestLine));
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Planted\"", Me("GET", window, null, GuestAddress, GuestPassword).body));

         // The organiser's own invitation, signed in, at SEQUENCE 0.
         SmtpRaw(Address, GuestAddress, CalendarMail("REQUEST", Address, GuestAddress, "Invitation: Quarterly review", "Quarterly review", uid, "20261031T090000Z", "20261031T100000Z", 0, Address, guestLine),
            Address, UserPassword);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Quarterly review\"", Me("GET", window, null, GuestAddress, GuestPassword).body));
         (int status, string body) guestCopy = Me("GET", window, null, GuestAddress, GuestPassword);
         Assert.AreEqual(1, Regex.Matches(guestCopy.body, "\"uid\":\"" + Regex.Escape(uid) + "\"").Count, "One event, replaced, never a second. Body: " + guestCopy.body);
         StringAssert.Contains("\"start\":\"20261031T090000Z\"", guestCopy.body);
         StringAssert.DoesNotContain("Planted", guestCopy.body);
         StringAssert.Contains("\"calendar_agrees\":false", Me("GET", "/api/v1/me/messages/" + MessageIdOf(GuestAddress, GuestPassword, "Planted review") + "/invitation", null, GuestAddress, GuestPassword).body,
            "The planted message's card does not say the calendar holds it.");
         StringAssert.Contains("\"calendar_agrees\":true", Me("GET", "/api/v1/me/messages/" + MessageIdOf(GuestAddress, GuestPassword, "Quarterly review") + "/invitation", null, GuestAddress, GuestPassword).body,
            "The organiser's card does.");

         // Planted again, at SEQUENCE 60: the copy is the organiser's now.
         SmtpRaw(Address, GuestAddress, CalendarMail("REQUEST", Address, GuestAddress, "Invitation: Planted again", "Planted again", uid, "20261031T180000Z", "20261031T190000Z", 60, Address, guestLine));
         MessageIdOf(GuestAddress, GuestPassword, "Planted again");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         (int status, string body) after = Me("GET", window, null, GuestAddress, GuestPassword);
         StringAssert.Contains("\"summary\":\"Quarterly review\"", after.body, "Body: " + after.body);
         StringAssert.DoesNotContain("Planted", after.body);
      }

      [Test]
      [Description("A calendar message an application sends through the send API, with a key that may send as any address of the domain and the organiser's address as its sender, changes no stored event: the organiser's CANCEL sent that way leaves the guest's copy as it was, and its card says so; the same CANCEL from the organiser signed in cancels it")]
      public void ACalendarMessageSentThroughTheSendApiChangesNoStoredEvent()
      {
         Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword).status, "The guest's calendar exists before the invitation arrives.");
         (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Offsite\",\"start\":\"2026-10-29T09:00\",\"end\":\"2026-10-29T10:00\",\"tz\":\"UTC\",\"attendees\":[\"" + GuestAddress + "\"],\"invite\":true}",
            Address, UserPassword);
         Assert.AreEqual(201, made.status, made.body);
         string uid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
         Assert.IsTrue(uid.Length > 0, "No uid in: " + made.body);
         string window = "/api/v1/me/calendar/events?start=20261029T000000Z&end=20261030T000000Z";
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Offsite\"", Me("GET", window, null, GuestAddress, GuestPassword).body));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // A key an application holds, that may send as any address of the domain.
         Response created = Raw(RestPort, "POST", "/api/v1/apikeys", Auth("Administrator", AdminPassword),
            "{\"label\":\"the calendar app\",\"scope\":\"full\",\"send_from\":\"*@" + _domain.Name + "\",\"send_per_day\":10}", "application/json", null);
         Assert.AreEqual(201, created.Status, created.Body);
         Match keyId = Regex.Match(created.Body, "\"id\"\\s*:\\s*(?:\"([^\"]*)\"|(\\d+))");
         string key = Regex.Match(created.Body, "\"key\"\\s*:\\s*\"([^\"]+)\"").Groups[1].Value;
         Assert.IsTrue(keyId.Success && key.Length > 0, created.Body);
         string id = keyId.Groups[1].Success ? keyId.Groups[1].Value : keyId.Groups[2].Value;
         try
         {
            string mail = CalendarMail("CANCEL", Address, GuestAddress, "unused", "Offsite", uid, "20261029T090000Z", "20261029T100000Z", 1, Address,
               "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + GuestAddress);
            string ics = mail.Substring(mail.IndexOf("\r\n\r\n", StringComparison.Ordinal) + 4);
            string body = "{\"from\":\"" + Address + "\",\"to\":[\"" + GuestAddress + "\"],\"subject\":\"Cancelled: Offsite, through the application\",\"text\":\"This event has been cancelled.\"," +
               "\"attachments\":[{\"filename\":\"invite.ics\",\"content_type\":\"text/calendar; method=CANCEL\",\"data\":\"" + Convert.ToBase64String(Encoding.UTF8.GetBytes(ics)) + "\"}]}";
            Response sent = Raw(RestPort, "POST", "/api/v1/send", "Bearer " + key, body, "application/json", null);
            Assert.AreEqual(202, sent.Status, sent.Body);
            long throughKey = MessageIdOf(GuestAddress, GuestPassword, "Offsite, through the application");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            StringAssert.DoesNotContain("\"status\":\"CANCELLED\"", Me("GET", window, null, GuestAddress, GuestPassword).body,
               "A key allowed to send as the organiser's address is no session of the organiser's.");
            (int status, string body) card = Me("GET", "/api/v1/me/messages/" + throughKey + "/invitation", null, GuestAddress, GuestPassword);
            Assert.AreEqual(200, card.status, card.body);
            StringAssert.Contains("\"method\":\"CANCEL\"", card.body);
            StringAssert.Contains("\"calendar_agrees\":false", card.body);

            // The organiser's own CANCEL, signed in, is filed.
            SmtpRaw(Address, GuestAddress, CalendarMail("CANCEL", Address, GuestAddress, "Cancelled: Offsite, signed in", "Offsite", uid, "20261029T090000Z", "20261029T100000Z", 1, Address,
               "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + GuestAddress), Address, UserPassword);
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"status\":\"CANCELLED\"", Me("GET", window, null, GuestAddress, GuestPassword).body));
         }
         finally
         {
            // The key goes; what it sent is left in hm_apisends, which RestApiSend
            // empties before each of its own tests. Not through the database
            // object: the Linux suite has none, and its ignore from a finally
            // would hide this test's result there.
            Raw(RestPort, "DELETE", "/api/v1/apikeys/" + id, Auth("Administrator", AdminPassword), null, "application/json", null);
         }
      }

      [Test]
      [Description("iMIP from outside is believed only for the From that DMARC vouched for: with DMARC passing for the partner's domain, a CANCEL in the organiser's name from another sender at that domain leaves the guest's copy and the organiser's own cancels it; a REPLY in a guest's name from another sender at that domain, and one from a guest whose domain publishes no DMARC policy, change nothing, and the guest's own is filed")]
      public void AnOutsideSenderIsBelievedForItsOwnAddressWhenDmarcPasses()
      {
         const string olga = "olga@imip-vouched.test";
         const string mallory = "mallory@imip-vouched.test";
         const string nina = "nina@imip-nodmarc.test";

         _application.Settings.AntiSpam.DMARCEnabled = true;
         SuiteDns.Zone
            .WithTxt("imip-vouched.test", "v=spf1 ip4:127.0.0.1 -all")
            .WithTxt("_dmarc.imip-vouched.test", "v=DMARC1; p=none")
            .WithTxt("imip-nodmarc.test", "v=spf1 ip4:127.0.0.1 -all");
         try
         {
            Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword).status);
            string window = "/api/v1/me/calendar/events?start=20261028T000000Z&end=20261029T000000Z";

            // Olga's invitation from outside is filed as it arrived.
            const string partnerUid = "partner-review-0001@imip-vouched.test";
            SmtpRaw(olga, GuestAddress, CalendarMail("REQUEST", olga, GuestAddress, "Invitation: Partner review", "Partner review", partnerUid, "20261028T090000Z", "20261028T100000Z", 0, olga,
               "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + GuestAddress));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Partner review\"", Me("GET", window, null, GuestAddress, GuestPassword).body));

            // Mallory, at the same domain - DMARC passes for it - cancels in Olga's name.
            SmtpRaw(mallory, GuestAddress, CalendarMail("CANCEL", mallory, GuestAddress, "Cancelled: Partner review forged", "Partner review", partnerUid, "20261028T090000Z", "20261028T100000Z", 1, olga,
               "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + GuestAddress));
            MessageIdOf(GuestAddress, GuestPassword, "Partner review forged");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            StringAssert.DoesNotContain("\"status\":\"CANCELLED\"", Me("GET", window, null, GuestAddress, GuestPassword).body,
               "DMARC vouches for the domain; the From is Mallory, not the organiser.");

            // Olga's own CANCEL is filed.
            SmtpRaw(olga, GuestAddress, CalendarMail("CANCEL", olga, GuestAddress, "Cancelled: Partner review", "Partner review", partnerUid, "20261028T090000Z", "20261028T100000Z", 1, olga,
               "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + GuestAddress));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"status\":\"CANCELLED\"", Me("GET", window, null, GuestAddress, GuestPassword).body));

            // The organiser's event with Olga and Nina as guests, no invitations mailed.
            (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
               "{\"summary\":\"Partner sync\",\"start\":\"2026-10-28T14:00\",\"end\":\"2026-10-28T15:00\",\"tz\":\"UTC\",\"attendees\":[\"" + olga + "\",\"" + nina + "\"]}",
               Address, UserPassword);
            Assert.AreEqual(201, made.status, made.body);
            long eventId = IdOf(made.body);
            string uid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
            Assert.IsTrue(uid.Length > 0, "No uid in: " + made.body);

            // Mallory declines in Olga's name; Nina, whose domain publishes no
            // DMARC policy (SPF alone vouches for nobody's From), accepts.
            SmtpRaw(mallory, Address, CalendarMail("REPLY", mallory, Address, "Declined: Partner sync forged", "Partner sync", uid, "20261028T140000Z", "20261028T150000Z", 0, Address,
               "ATTENDEE;PARTSTAT=DECLINED:mailto:" + olga));
            SmtpRaw(nina, Address, CalendarMail("REPLY", nina, Address, "Accepted: Partner sync unvouched", "Partner sync", uid, "20261028T140000Z", "20261028T150000Z", 0, Address,
               "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + nina));
            MessageIdOf(Address, UserPassword, "Partner sync forged");
            MessageIdOf(Address, UserPassword, "Partner sync unvouched");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            (int status, string body) organiserEvent = Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Address, UserPassword);
            Assert.AreEqual("NEEDS-ACTION", PartstatOf(organiserEvent.body, olga), "Mallory's answer for Olga. Body: " + organiserEvent.body);
            Assert.AreEqual("NEEDS-ACTION", PartstatOf(organiserEvent.body, nina), "Nina's answer, which nothing vouched for. Body: " + organiserEvent.body);

            // Olga's own answer is filed.
            SmtpRaw(olga, Address, CalendarMail("REPLY", olga, Address, "Accepted: Partner sync", "Partner sync", uid, "20261028T140000Z", "20261028T150000Z", 0, Address,
               "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + olga));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
               RetryableAssert.StringContains("ACCEPTED", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Address, UserPassword).body, olga)));
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         }
         finally
         {
            SuiteDns.Reset();
         }
      }

      [Test]
      [Description("Calendar messages from outside shaped as Outlook and Gmail send them, from a domain DMARC passes for, are applied as before: an organiser's invitation, its update and its cancellation as Outlook writes them (a display name on the From, a base64 text/calendar alternative) reach the guest's copy; a guest's answer as Gmail writes it (a multipart/mixed with the calendar in an alternative and again as an attachment) and an answer Outlook sends for a guest by their assistant (From the guest, Sender the assistant, SENT-BY on the ATTENDEE) reach the organiser's event")]
      public void OutlookAndGmailShapedMessagesFromOutsideAreApplied()
      {
         const string olga = "olga@imip-shapes.test";
         const string nina = "nina@imip-shapes.test";
         const string boss = "boss@imip-shapes.test";
         const string assistant = "assistant@imip-shapes.test";

         _application.Settings.AntiSpam.DMARCEnabled = true;
         SuiteDns.Zone
            .WithTxt("imip-shapes.test", "v=spf1 ip4:127.0.0.1 -all")
            .WithTxt("_dmarc.imip-shapes.test", "v=DMARC1; p=none");
         try
         {
            Assert.AreEqual(200, Me("GET", "/api/v1/me/calendars", null, GuestAddress, GuestPassword).status);
            string window = "/api/v1/me/calendar/events?start=20261102T000000Z&end=20261103T000000Z";
            const string uid = "040000008200E00074C5B7101A82E00800000000imip-shapes";

            // Outlook: the invitation, then an update, then the cancellation.
            SmtpRaw(olga, GuestAddress, OutlookMail(olga, "Olga Organiser", GuestAddress, "Shapes review", "REQUEST", uid, 0, "20261102T090000Z", "20261102T100000Z", ""));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"summary\":\"Shapes review\"", Me("GET", window, null, GuestAddress, GuestPassword).body));
            SmtpRaw(olga, GuestAddress, OutlookMail(olga, "Olga Organiser", GuestAddress, "Shapes review", "REQUEST", uid, 1, "20261102T140000Z", "20261102T150000Z", ""));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"start\":\"20261102T140000Z\"", Me("GET", window, null, GuestAddress, GuestPassword).body));
            SmtpRaw(olga, GuestAddress, OutlookMail(olga, "Olga Organiser", GuestAddress, "Shapes review", "CANCEL", uid, 2, "20261102T140000Z", "20261102T150000Z", "STATUS:CANCELLED"));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.StringContains("\"status\":\"CANCELLED\"", Me("GET", window, null, GuestAddress, GuestPassword).body));

            // The organiser's event with Nina and the boss as guests, no invitations mailed.
            (int status, string body) made = Me("POST", "/api/v1/me/calendar/events",
               "{\"summary\":\"Shapes sync\",\"start\":\"2026-11-02T16:00\",\"end\":\"2026-11-02T17:00\",\"tz\":\"UTC\",\"attendees\":[\"" + nina + "\",\"" + boss + "\"]}",
               Address, UserPassword);
            Assert.AreEqual(201, made.status, made.body);
            long eventId = IdOf(made.body);
            string eventUid = Regex.Match(made.body, "\"uid\":\"([^\"]+)\"").Groups[1].Value;
            Assert.IsTrue(eventUid.Length > 0, "No uid in: " + made.body);

            // Gmail: Nina accepts.
            SmtpRaw(nina, Address, GmailReply(nina, "Nina Guest", Address, "Shapes sync", eventUid, "ACCEPTED", "20261102T160000Z", "20261102T170000Z"));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
               RetryableAssert.StringContains("ACCEPTED", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Address, UserPassword).body, nina)));

            // Outlook: the boss's assistant answers for the boss.
            string reply = string.Join("\r\n", new[]
            {
               "BEGIN:VCALENDAR", "METHOD:REPLY", "PRODID:Microsoft Exchange Server 2010", "VERSION:2.0", "BEGIN:VEVENT",
               "ATTENDEE;PARTSTAT=TENTATIVE;SENT-BY=\"mailto:" + assistant.ToUpperInvariant() + "\";CN=Bea Boss:mailto:" + boss.ToUpperInvariant(),
               "ORGANIZER:mailto:" + Address, "UID:" + eventUid, "SEQUENCE:0", "DTSTART:20261102T160000Z", "DTEND:20261102T170000Z",
               "SUMMARY;LANGUAGE=en-GB:Tentative: Shapes sync", "DTSTAMP:20260928T080000Z", "END:VEVENT", "END:VCALENDAR", ""
            });
            SmtpRaw(boss, Address, string.Join("\r\n", new[]
            {
               "From: Bea Boss <" + boss + ">", "Sender: Alex Assistant <" + assistant + ">", "To: Sam Self <" + Address + ">",
               "Subject: Tentative: Shapes sync", "Date: Mon, 28 Sep 2026 08:00:00 +0000", "MIME-Version: 1.0",
               "Content-Type: text/calendar; charset=\"utf-8\"; method=REPLY", "Content-Transfer-Encoding: base64", "",
               Convert.ToBase64String(Encoding.UTF8.GetBytes(reply), Base64FormattingOptions.InsertLineBreaks), ""
            }));
            RetryHelper.TryAction(TimeSpan.FromSeconds(20), () =>
               RetryableAssert.StringContains("TENTATIVE", PartstatOf(Me("GET", "/api/v1/me/calendar/events/" + eventId, null, Address, UserPassword).body, boss)));
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         }
         finally
         {
            SuiteDns.Reset();
         }
      }

      // An invitation, an update or a cancellation as Outlook writes one: a
      // display name on the From, and the calendar as a base64 text/calendar
      // alternative beside the text, with Exchange's PRODID and a UID of its shape.
      private static string OutlookMail(string organizer, string name, string to, string summary, string method, string uid, int sequence, string dtstart, string dtend, string extraLine)
      {
         var lines = new List<string>
         {
            "BEGIN:VCALENDAR", "METHOD:" + method, "PRODID:Microsoft Exchange Server 2010", "VERSION:2.0", "BEGIN:VEVENT",
            "ORGANIZER;CN=\"" + name + "\":mailto:" + organizer,
            "ATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE;CN=" + to + ":mailto:" + to,
            "DESCRIPTION;LANGUAGE=en-GB:\\n", "UID:" + uid, "SUMMARY;LANGUAGE=en-GB:" + summary,
            "DTSTART:" + dtstart, "DTEND:" + dtend, "CLASS:PUBLIC", "PRIORITY:5", "DTSTAMP:20260928T080000Z", "TRANSP:OPAQUE",
            "SEQUENCE:" + sequence, "X-MICROSOFT-CDO-BUSYSTATUS:TENTATIVE"
         };
         if (extraLine.Length > 0)
            lines.Add(extraLine);
         lines.AddRange(new[] { "END:VEVENT", "END:VCALENDAR", "" });
         string calendar = string.Join("\r\n", lines);
         string boundary = "_000_outlook_" + method + sequence + "_";
         return string.Join("\r\n", new[]
         {
            "From: " + name + " <" + organizer + ">", "To: \"" + to + "\" <" + to + ">",
            "Subject: " + (method == "CANCEL" ? "Canceled: " : "") + summary, "Date: Mon, 28 Sep 2026 08:00:00 +0000",
            "Message-ID: <" + method + sequence + "-imip-shapes@imip-shapes.test>", "Content-Language: en-GB", "MIME-Version: 1.0",
            "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"", "",
            "--" + boundary, "Content-Type: text/plain; charset=\"us-ascii\"", "Content-Transfer-Encoding: quoted-printable", "", summary, "",
            "--" + boundary, "Content-Type: text/calendar; charset=\"utf-8\"; method=" + method, "Content-Transfer-Encoding: base64", "",
            Convert.ToBase64String(Encoding.UTF8.GetBytes(calendar), Base64FormattingOptions.InsertLineBreaks), "",
            "--" + boundary + "--", ""
         });
      }

      // An answer as Gmail writes one: a multipart/mixed holding the text, the
      // HTML and the calendar as alternatives, and the calendar again as an
      // invite.ics attachment.
      private static string GmailReply(string attendee, string name, string organizer, string summary, string uid, string partstat, string dtstart, string dtend)
      {
         string calendar = string.Join("\r\n", new[]
         {
            "BEGIN:VCALENDAR", "PRODID:-//Google Inc//Google Calendar 70.9054//EN", "VERSION:2.0", "CALSCALE:GREGORIAN", "METHOD:REPLY", "BEGIN:VEVENT",
            "DTSTART:" + dtstart, "DTEND:" + dtend, "DTSTAMP:20260928T080000Z", "ORGANIZER:mailto:" + organizer, "UID:" + uid,
            "ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=" + partstat + ";CN=" + name + ";X-NUM-GUESTS=0:mailto:" + attendee,
            "CREATED:20260928T080000Z", "LAST-MODIFIED:20260928T080000Z", "LOCATION:", "SEQUENCE:0", "STATUS:CONFIRMED",
            "SUMMARY:" + summary, "TRANSP:OPAQUE", "END:VEVENT", "END:VCALENDAR", ""
         });
         return string.Join("\r\n", new[]
         {
            "From: " + name + " <" + attendee + ">", "To: " + organizer, "Subject: Accepted: " + summary,
            "Date: Mon, 28 Sep 2026 08:00:00 +0000", "Message-ID: <gmail-shaped-reply@imip-shapes.test>", "MIME-Version: 1.0",
            "Content-Type: multipart/mixed; boundary=\"000000000000mixed\"", "",
            "--000000000000mixed", "Content-Type: multipart/alternative; boundary=\"000000000000alt\"", "",
            "--000000000000alt", "Content-Type: text/plain; charset=\"UTF-8\"", "", name + " has accepted this invitation.", "",
            "--000000000000alt", "Content-Type: text/html; charset=\"UTF-8\"", "", "<p>" + name + " has accepted this invitation.</p>", "",
            "--000000000000alt", "Content-Type: text/calendar; charset=\"UTF-8\"; method=REPLY", "Content-Transfer-Encoding: 7bit", "", calendar, "",
            "--000000000000alt--", "",
            "--000000000000mixed", "Content-Type: application/ics; name=\"invite.ics\"", "Content-Disposition: attachment; filename=\"invite.ics\"",
            "Content-Transfer-Encoding: base64", "", Convert.ToBase64String(Encoding.UTF8.GetBytes(calendar), Base64FormattingOptions.InsertLineBreaks), "",
            "--000000000000mixed--", ""
         });
      }

      [Test]
      [Description("An invitation shaped as Outlook sends one - a text/calendar alternative whose Content-Type carries method and charset, and no .ics name - is marked as one in the message JSON, and a message with no calendar part is not")]
      public void AnOutlookShapedInvitationIsMarkedAsOne()
      {
         string calendar = string.Join("\r\n", new[]
         {
            "BEGIN:VCALENDAR", "METHOD:REQUEST", "PRODID:Microsoft Exchange Server 2010", "VERSION:2.0", "BEGIN:VEVENT",
            "ORGANIZER;CN=Olga Organiser:mailto:olga@example.org",
            "ATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:" + GuestAddress,
            "SUMMARY:Outlook planning", "DTSTART:20261021T090000Z", "DTEND:20261021T100000Z",
            "UID:040000008200E00074C5B7101A82E00800000000outlook-shaped", "DTSTAMP:20260927T080000Z", "SEQUENCE:0",
            "END:VEVENT", "END:VCALENDAR"
         });

         // No name on the calendar part, and its Content-Type in Outlook's order:
         // the part is recognised by its media type alone.
         SmtpClientSimulator.StaticSendRaw("olga@example.org", GuestAddress, string.Join("\r\n", new[]
         {
            "From: Olga Organiser <olga@example.org>", "To: " + GuestAddress, "Subject: Outlook planning",
            "Date: Sun, 27 Sep 2026 08:00:00 +0000", "Message-ID: <outlook-shaped-invitation@example.org>", "MIME-Version: 1.0",
            "Content-Type: multipart/alternative; boundary=\"outlook-boundary\"", "",
            "--outlook-boundary", "Content-Type: text/plain; charset=\"utf-8\"", "", "Planning, as Outlook sends it.", "",
            "--outlook-boundary", "Content-Type: text/calendar; method=REQUEST; charset=utf-8", "Content-Transfer-Encoding: 7bit", "",
            calendar, "",
            "--outlook-boundary--", ""
         }));

         SmtpClientSimulator.StaticSendRaw("olga@example.org", GuestAddress, string.Join("\r\n", new[]
         {
            "From: Olga Organiser <olga@example.org>", "To: " + GuestAddress, "Subject: Outlook plain note",
            "Date: Sun, 27 Sep 2026 08:01:00 +0000", "MIME-Version: 1.0", "Content-Type: text/plain; charset=utf-8", "",
            "No calendar here.", ""
         }));

         long invitationId = MessageIdOf(GuestAddress, GuestPassword, "Outlook planning");
         (int status, string body) message = Me("GET", "/api/v1/me/messages/" + invitationId, null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, message.status, message.body);
         StringAssert.Contains("\"invitation\":true", message.body,
            "A text/calendar part whose Content-Type carries parameters is an invitation. Body: " + message.body);

         (int status, string body) invitation = Me("GET", "/api/v1/me/messages/" + invitationId + "/invitation", null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, invitation.status, invitation.body);
         StringAssert.Contains("\"method\":\"REQUEST\"", invitation.body);
         StringAssert.Contains("\"summary\":\"Outlook planning\"", invitation.body);

         long noteId = MessageIdOf(GuestAddress, GuestPassword, "Outlook plain note");
         (int status, string body) note = Me("GET", "/api/v1/me/messages/" + noteId, null, GuestAddress, GuestPassword);
         Assert.AreEqual(200, note.status, note.body);
         StringAssert.Contains("\"invitation\":false", note.body, "A message with no calendar part is no invitation.");
      }

      [Test]
      [Description("SEQUENCE and PRIORITY held to their range: an edit of a task or an event at SEQUENCE 2147483647 keeps it rather than wrapping to -2147483648, which made the edit older than what it replaced; a SEQUENCE past an int reads as 2147483647 on both platforms; a PRIORITY another client wrote outside 0-9 lists as none; a priority or an alarm past an int's range is refused rather than converted")]
      public void SequenceAndPriorityAreHeldToTheirRange()
      {
         // Tasks a phone put over CalDAV, each as it wrote it.
         PutTask("seq-max", "Seq max", "SEQUENCE:2147483647");
         PutTask("seq-huge", "Seq huge", "SEQUENCE:99999999999");
         PutTask("seq-five", "Seq five", "SEQUENCE:5");
         PutTask("prio-fifteen", "Prio fifteen", "PRIORITY:15");
         PutTask("prio-three", "Prio three", "PRIORITY:3");

         string tasks = Me("GET", "/api/v1/me/tasks", null, Address, UserPassword).body;
         Assert.AreEqual("2147483647", TaskNumber(tasks, "seq-huge", "sequence"),
            "A SEQUENCE past an int reads as the largest one on both platforms; Linux read its low 32 bits. Body: " + tasks);
         Assert.AreEqual("0", TaskNumber(tasks, "prio-fifteen", "priority"),
            "A PRIORITY outside 0-9 is listed as none, as the task routes would refuse it. Body: " + tasks);
         Assert.AreEqual("3", TaskNumber(tasks, "prio-three", "priority"), tasks);

         // An edit through the REST route writes one more, held at the largest.
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/tasks/" + TaskId(tasks, "seq-max"), "{\"summary\":\"Seq max, edited\"}", Address, UserPassword).status);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/tasks/" + TaskId(tasks, "seq-five"), "{\"summary\":\"Seq five, edited\"}", Address, UserPassword).status);
         tasks = Me("GET", "/api/v1/me/tasks", null, Address, UserPassword).body;
         Assert.AreEqual("2147483647", TaskNumber(tasks, "seq-max", "sequence"),
            "An edit at SEQUENCE 2147483647 keeps it; adding one wrapped to -2147483648. Body: " + tasks);
         Assert.AreEqual("6", TaskNumber(tasks, "seq-five", "sequence"), "An ordinary edit adds one. Body: " + tasks);
         string storedTask = Dav("GET", Home + "calendar/seq-max.ics", Auth(Address, UserPassword)).Body;
         StringAssert.Contains("SEQUENCE:2147483647", storedTask, storedTask);

         // An event with an attendee, whose edit adds one to its SEQUENCE too.
         Response putEvent = Raw(WebServicesPort, "PUT", Home + "calendar/seq-max-event.ics", Auth(Address, UserPassword),
            string.Join("\r\n", new[]
            {
               "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A phone//EN", "BEGIN:VEVENT", "UID:seq-max-event", "DTSTAMP:20260101T000000Z",
               "DTSTART:20261102T090000Z", "DTEND:20261102T100000Z", "SUMMARY:Seq max meeting",
               "ORGANIZER:mailto:" + Address, "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + GuestAddress,
               "SEQUENCE:2147483647", "END:VEVENT", "END:VCALENDAR", ""
            }),
            "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\nIf-None-Match: *\r\n");
         Assert.AreEqual(201, putEvent.Status, putEvent.Body);
         (int status, string body) events = Me("GET", "/api/v1/me/calendar/events?start=20261102T000000Z&end=20261103T000000Z", null, Address, UserPassword);
         Assert.AreEqual(200, events.status, events.body);
         (int status, string body) edited = Me("PUT", "/api/v1/me/calendar/events/" + TaskId(events.body, "seq-max-event"), "{\"summary\":\"Seq max meeting edited\"}", Address, UserPassword);
         Assert.AreEqual(200, edited.status, edited.body);
         string storedEvent = Dav("GET", Home + "calendar/seq-max-event.ics", Auth(Address, UserPassword)).Body;
         StringAssert.Contains("SUMMARY:Seq max meeting edited", storedEvent, storedEvent);
         StringAssert.Contains("SEQUENCE:2147483647", storedEvent,
            "An edit of an event at SEQUENCE 2147483647 keeps it. Stored: " + storedEvent);

         // Through the REST route a priority is 0-9 and an alarm at most a year,
         // each held to that as the number it is: one past an int's range was
         // converted first, which is undefined - on x64 an alarm of 1e20 minutes
         // became INT_MIN and was taken as none.
         (int status, string body) nine = Me("POST", "/api/v1/me/tasks", "{\"summary\":\"Nine\",\"priority\":9}", Address, UserPassword);
         Assert.AreEqual(201, nine.status, nine.body);
         StringAssert.Contains("\"priority\":9", nine.body);
         (int status, string body) hugePriority = Me("POST", "/api/v1/me/tasks", "{\"summary\":\"Huge\",\"priority\":1e20}", Address, UserPassword);
         Assert.AreEqual(400, hugePriority.status, hugePriority.body);
         (int status, string body) hugeAlarm = Me("POST", "/api/v1/me/calendar/events",
            "{\"summary\":\"Alarm\",\"start\":\"2026-11-03T09:00\",\"end\":\"2026-11-03T10:00\",\"tz\":\"Europe/London\",\"alarm_minutes\":1e20}",
            Address, UserPassword);
         Assert.AreEqual(400, hugeAlarm.status, "An alarm of 1e20 minutes is more than a year. Body: " + hugeAlarm.body);
         StringAssert.Contains("at most a year", hugeAlarm.body);
      }

      [Test]
      [Description("An alarm's TRIGGER is any calendar client's text, and an invitation's is its sender's: a duration past an int's range - its digits, a week's product of them, or the sum of its parts - lists as 2147483647 minutes, held to its end as SEQUENCE is, where each overflowed an int; an ordinary one is what it says")]
      public void AnAlarmPastAnIntIsHeldToItsEnd()
      {
         PutEventWithAlarm("alarm-digits", "-PT99999999999M");
         PutEventWithAlarm("alarm-weeks", "-P999999W");
         PutEventWithAlarm("alarm-sum", "-P200000W200000W");
         PutEventWithAlarm("alarm-ordinary", "-P1DT2H");

         (int status, string body) events = Me("GET", "/api/v1/me/calendar/events?start=20261104T000000Z&end=20261105T000000Z", null, Address, UserPassword);
         Assert.AreEqual(200, events.status, events.body);
         Assert.AreEqual("2147483647", TaskNumber(events.body, "alarm-digits", "alarm_minutes"),
            "A run of digits past an int is held to the largest. Body: " + events.body);
         Assert.AreEqual("2147483647", TaskNumber(events.body, "alarm-weeks", "alarm_minutes"),
            "Weeks whose minutes are past an int are held to the largest. Body: " + events.body);
         Assert.AreEqual("2147483647", TaskNumber(events.body, "alarm-sum", "alarm_minutes"),
            "Parts that fit an int each and not together are held to the largest. Body: " + events.body);
         Assert.AreEqual("1560", TaskNumber(events.body, "alarm-ordinary", "alarm_minutes"),
            "A day and two hours is 1560 minutes. Body: " + events.body);
      }

      // A VEVENT with one display alarm, as a phone puts it over CalDAV.
      private void PutEventWithAlarm(string uid, string trigger)
      {
         Response put = Raw(WebServicesPort, "PUT", Home + "calendar/" + uid + ".ics", Auth(Address, UserPassword),
            string.Join("\r\n", new[]
            {
               "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A phone//EN", "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z",
               "DTSTART:20261104T090000Z", "DTEND:20261104T100000Z", "SUMMARY:" + uid,
               "BEGIN:VALARM", "ACTION:DISPLAY", "DESCRIPTION:Reminder", "TRIGGER:" + trigger, "END:VALARM",
               "END:VEVENT", "END:VCALENDAR", ""
            }),
            "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\nIf-None-Match: *\r\n");
         Assert.AreEqual(201, put.Status, put.Body);
      }

      // A VTODO as a phone puts it over CalDAV, with one more property line.
      private void PutTask(string uid, string summary, string extra)
      {
         Response put = Raw(WebServicesPort, "PUT", Home + "calendar/" + uid + ".ics", Auth(Address, UserPassword),
            string.Join("\r\n", new[]
            {
               "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A phone//EN", "BEGIN:VTODO", "UID:" + uid, "DTSTAMP:20260101T000000Z",
               "DUE:20261101T090000Z", "SUMMARY:" + summary, extra, "STATUS:NEEDS-ACTION", "END:VTODO", "END:VCALENDAR", ""
            }),
            "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\nIf-None-Match: *\r\n");
         Assert.AreEqual(201, put.Status, put.Body);
      }

      // The id of the task or event with this UID in a listing.
      private static string TaskId(string listing, string uid)
      {
         Match match = Regex.Match(listing, "\\{\"id\":(\\d+),\"calendar\":\\d+,\"uid\":\"" + Regex.Escape(uid) + "\"");
         Assert.IsTrue(match.Success, "No " + uid + " in: " + listing);
         return match.Groups[1].Value;
      }

      // A number field of the task with this UID in the task listing.
      private static string TaskNumber(string listing, string uid, string field)
      {
         Match match = Regex.Match(listing, "\\{\"id\":\\d+,\"calendar\":\\d+,\"uid\":\"" + Regex.Escape(uid) + "\"[^}]*?\"" + field + "\":(-?\\d+)");
         Assert.IsTrue(match.Success, "No " + field + " for " + uid + " in: " + listing);
         return match.Groups[1].Value;
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

      // The PARTSTAT, and the SENT-BY, an event's JSON gives the address; ""
      // when the event does not list it.
      private static string PartstatOf(string eventJson, string address)
      {
         Match match = Regex.Match(eventJson, "\"address\":\"" + Regex.Escape(address) + "\",\"name\":\"[^\"]*\",\"partstat\":\"([^\"]*)\"");
         return match.Success ? match.Groups[1].Value : "";
      }

      private static string SentByOf(string eventJson, string address)
      {
         Match match = Regex.Match(eventJson, "\"address\":\"" + Regex.Escape(address) + "\",\"name\":\"[^\"]*\",\"partstat\":\"[^\"]*\",\"sent_by\":\"([^\"]*)\"");
         return match.Success ? match.Groups[1].Value : "";
      }

      // A mail whose whole body is a calendar object with the method, as a mail
      // program sends one: From the given address, the event's summary and
      // organiser, and the lines given (ATTENDEEs, and anything else) inside
      // the VEVENT.
      private static string CalendarMail(string method, string from, string to, string subject, string summary, string uid, string dtstart, string dtend, int sequence, string organizer, params string[] lines)
      {
         var text = new List<string>
         {
            "From: <" + from + ">", "To: <" + to + ">", "Subject: " + subject, "Date: Mon, 28 Sep 2026 08:00:00 +0000", "MIME-Version: 1.0",
            "Content-Type: text/calendar; charset=utf-8; method=" + method, "",
            "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A mail program//EN", "METHOD:" + method, "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260928T080000Z",
            "DTSTART:" + dtstart, "DTEND:" + dtend, "SUMMARY:" + summary, "SEQUENCE:" + sequence, "ORGANIZER:mailto:" + organizer
         };
         text.AddRange(lines);
         text.AddRange(new[] { "END:VEVENT", "END:VCALENDAR", "" });
         return string.Join("\r\n", text);
      }

      // One SMTP session of the test's own, to one recipient: signed in as
      // login when one is given - the sender's own submission - and not signed
      // in at all otherwise. MAIL FROM is from; the message's From is its own.
      // The server's SMTP port as the suite has it (TestPorts): 25 on a
      // Windows bench, the Linux suite's own elsewhere.
      private static void SmtpRaw(string from, string to, string message, string login = null, string password = null)
      {
         using (var client = new TcpClient(TestPorts.Host, TestPorts.Actual(SmtpPort)))
         using (NetworkStream stream = client.GetStream())
         using (var reader = new StreamReader(stream, Encoding.ASCII))
         using (var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\r\n" })
         {
            ExpectSmtp(reader, "220");
            if (login != null)
            {
               writer.WriteLine("EHLO mail.calendar.test");
               ExpectSmtp(reader, "250");
               writer.WriteLine("AUTH PLAIN " + Convert.ToBase64String(Encoding.UTF8.GetBytes("\0" + login + "\0" + password)));
               ExpectSmtp(reader, "235");
            }
            else
            {
               writer.WriteLine("HELO mail.calendar.test");
               ExpectSmtp(reader, "250");
            }
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
