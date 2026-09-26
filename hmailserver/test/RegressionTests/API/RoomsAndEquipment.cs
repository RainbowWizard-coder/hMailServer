// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Rooms and equipment (Common/Util/ResourceBooking.h,
   ///    docs/RoomsAndEquipment.md): an account whose kind is a room answers the
   ///    invitations it is sent by its booking policy, just after they are
   ///    delivered - accepting a free slot, declining a conflict, a booking
   ///    outside its window, one over its longest booking, one outside its
   ///    working hours, a series with no end and an organiser it does not let
   ///    book, each with the reason - and sends the organiser an iMIP REPLY from
   ///    its own address through the queue. It believes an organiser only when
   ///    the server recorded that the organiser sent the message: here, an SMTP
   ///    session signed in as them. Anything else is neither filed nor answered,
   ///    and the delegate is told once an hour. A CANCEL from the organiser frees
   ///    the slot; a REQUEST with a higher SEQUENCE is decided again; a change to
   ///    one occurrence is decided alone and merged into the series; with
   ///    automatic answers off, or a request sent automatically, the request is
   ///    left for the delegate, who answers for the room from its calendar - and
   ///    only a delegate's answer tells anybody, and only the organiser the room
   ///    believed when it accepted the booking, whatever the calendar says. And
   ///    the room cannot be signed into, is in the directory as a room, and is
   ///    found by a client's room finder over CalDAV. Every REPLY is checked for
   ///    its METHOD, PARTSTAT, UID, SEQUENCE and sender. An organiser from outside
   ///    is believed when DMARC passes for its domain, which the suite's own DNS
   ///    zone (SuiteDns) makes happen with an SPF record for the loopback address.
   ///    A request left for the delegates is never put into the room's calendar.
   ///
   ///    The dates are days ahead of the bench's clock, because the booking window
   ///    is measured from now; every time is UTC. A decline quotes nothing of the
   ///    request, so answers are found by the UID and SEQUENCE their calendar
   ///    part carries.
   /// </summary>
   [TestFixture]
   public class RoomsAndEquipment : TestFixtureBase
   {
      private const int WebServicesPort = 10210;
      private static int RestPort = 10211;
      private const int OutsidePort = 10212;
      private const int SmtpPort = 25;
      private const string Password = "Rooms-Passw0rd!";
      private const string AdminPassword = "testar";

      private const string NsDav = "DAV:";
      private const string NsCalDav = "urn:ietf:params:xml:ns:caldav";

      private string Room { get { return "boardroom@" + _domain.Name; } }
      private string Alice { get { return "alice@" + _domain.Name; } }
      private string Bob { get { return "bob@" + _domain.Name; } }
      private string Deputy { get { return "deputy@" + _domain.Name; } }
      private string Colleague { get { return "colleague@" + _domain.Name; } }

      [SetUp]
      public void StartListeners()
      {
         Account room = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Room, Password);
         room.PersonFirstName = "Board";
         room.PersonLastName = "Room";
         room.Save();
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Alice, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Bob, Password);
         Account deputy = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Deputy, Password);
         deputy.PersonFirstName = "Dee";
         deputy.PersonLastName = "Puty";
         deputy.Save();
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Colleague, Password);
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

         // Made a room over the account's own route, as the Deck makes one.
         (int status, string body) made = Admin("PUT", "/api/v1/accounts/" + Room, "{\"kind\":\"room\"}");
         Assert.AreEqual(200, made.status, made.body);
         StringAssert.Contains("\"kind\":\"room\"", made.body);
      }

      [TearDown]
      public void StopListeners()
      {
         IniFileSetting.Write("WebServicesHttpPort", "0");
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("A request for a free slot from its organiser, signed in, is accepted: the REPLY is METHOD:REPLY, the room's PARTSTAT ACCEPTED, the request's UID and SEQUENCE, From the room's address and marked Auto-Submitted, and the room is busy then to a colleague who asks")]
      public void AFreeSlotIsAcceptedAndTheAnswerComesFromTheRoom()
      {
         string uid = "rooms-free-0001@" + _domain.Name;
         string start = Slot(3, 10, 0);
         string end = Slot(3, 11, 0);

         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Planning", "Planning", uid, start, end, 0, Alice), Alice);

         Reply reply = ReplyTo(Alice, uid, "ACCEPTED", 0);
         StringAssert.Contains("\"method\":\"REPLY\"", reply.Card);
         StringAssert.Contains("\"uid\":\"" + uid + "\"", reply.Card);
         StringAssert.Contains("\"sequence\":0", reply.Card);
         StringAssert.Contains("\"address\":\"" + Room + "\",\"name\":\"Board Room\",\"partstat\":\"ACCEPTED\"", reply.Card, "The room's own answer.");
         StringAssert.Contains("From: \"Board Room\" <" + Room + ">", reply.Source, "From the room's address.");
         StringAssert.Contains("Auto-Submitted: auto-replied", reply.Source);
         StringAssert.Contains("Subject: Accepted: Planning", reply.Source, "An answer to a request that is honoured may repeat it.");
         StringAssert.Contains("METHOD:REPLY", reply.Text);
         StringAssert.Contains("CUTYPE=ROOM", reply.Text.Replace("\n ", ""), "The attendee says what it is.");
         StringAssert.Contains("Board Room <" + Room + "> has accepted this invitation.", reply.Text);

         CollectionAssert.AreEqual(new[] { start + "/" + end + "/busy" }, Periods(FreeBusyOfRoom(3)),
            "Filed as accepted: busy, not tentative.");
      }

      [Test]
      [Description("A request for a slot that is taken is declined with when the room is busy, and never whose booking it is; the booking holding the slot is busy although its organiser sent it as TRANSP:TRANSPARENT, and the decline quotes nothing of the request")]
      public void AConflictIsDeclinedWithTheTimeAndNotWhoseBookingItIs()
      {
         string start = Slot(4, 10, 0);
         string end = Slot(4, 11, 0);
         string first = "rooms-conflict-a@" + _domain.Name;
         string second = "rooms-conflict-b@" + _domain.Name;

         // "Show as free" is the organiser's own setting; to the room the
         // booking is a booking.
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Strategy", "Strategy", first, start, end, 0, Alice, "TRANSP:TRANSPARENT"), Alice);
         ReplyTo(Alice, first, "ACCEPTED", 0);

         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Standup", "Standup", second, Slot(4, 10, 30), Slot(4, 11, 30), 0, Bob, "LOCATION:Somewhere to repeat"), Bob);
         Reply declined = ReplyTo(Bob, second, "DECLINED", 0);
         StringAssert.Contains("\"address\":\"" + Room + "\",\"name\":\"Board Room\",\"partstat\":\"DECLINED\"", declined.Card);
         StringAssert.Contains("Board Room is already booked at that time: ", declined.Text);
         StringAssert.Contains("10:00 - 11:00 UTC", declined.Text, "When the room is busy.");
         StringAssert.DoesNotContain(Alice, declined.Text, "Not whose booking it is.");
         StringAssert.DoesNotContain("Strategy", declined.Text, "Nor what it is.");
         StringAssert.Contains("COMMENT:", declined.Text, "The reason travels in the REPLY as well as in the text.");
         StringAssert.DoesNotContain("Standup", declined.Source + declined.Text, "A decline quotes nothing of the request: not its summary,");
         StringAssert.DoesNotContain("Somewhere to repeat", declined.Text, "not its location,");
         StringAssert.DoesNotContain("SUMMARY", declined.Text, "and the REPLY carries no SUMMARY.");

         CollectionAssert.AreEqual(new[] { start + "/" + end + "/busy" }, Periods(FreeBusyOfRoom(4)), "Alice's booking stands, busy, and Bob's was not filed.");
      }

      [Test]
      [Description("The booking window, working hours and recurrence: a meeting past the window is declined and one inside it accepted; with working hours only, a meeting in the evening is declined; a series with no end is declined as past the window")]
      public void OutsideTheWindowOrTheWorkingHoursIsDeclined()
      {
         (int status, string body) policy = Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy",
            "{\"window_days\":30,\"working_hours_only\":true,\"work_days\":[\"mon\",\"tue\",\"wed\",\"thu\",\"fri\",\"sat\",\"sun\"],\"work_start\":\"08:00\",\"work_end\":\"18:00\",\"time_zone\":\"UTC\"}");
         Assert.AreEqual(200, policy.status, policy.body);

         string far = "rooms-window-far@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Far away", "Far away", far, Slot(45, 10, 0), Slot(45, 11, 0), 0, Alice), Alice);
         StringAssert.Contains("Board Room can be booked at most 30 days ahead", ReplyTo(Alice, far, "DECLINED", 0).Text);

         string near = "rooms-window-near@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Near", "Near", near, Slot(10, 10, 0), Slot(10, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, near, "ACCEPTED", 0);

         string late = "rooms-hours-late@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Late", "Late", late, Slot(11, 19, 0), Slot(11, 20, 0), 0, Alice), Alice);
         StringAssert.Contains("Board Room can be booked in working hours only - 08:00 to 18:00 UTC", ReplyTo(Alice, late, "DECLINED", 0).Text);

         string series = "rooms-series@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Every week", "Every week", series, Slot(12, 9, 0), Slot(12, 10, 0), 0, Alice,
            "RRULE:FREQ=WEEKLY"), Alice);
         StringAssert.Contains("this series has no end", ReplyTo(Alice, series, "DECLINED", 0).Text);
      }

      [Test]
      [Description("The longest booking: two hours is declined where the room allows sixty minutes, and sixty minutes is accepted")]
      public void OverTheLongestBookingIsDeclined()
      {
         (int status, string body) policy = Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"max_duration_minutes\":60}");
         Assert.AreEqual(200, policy.status, policy.body);

         string workshop = "rooms-long@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Workshop", "Workshop", workshop, Slot(6, 9, 0), Slot(6, 11, 0), 0, Alice), Alice);
         StringAssert.Contains("Board Room can be booked for at most 60 minutes at a time, and this meeting lasts 120.", ReplyTo(Alice, workshop, "DECLINED", 0).Text);

         string shorter = "rooms-short@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Short", "Short", shorter, Slot(6, 13, 0), Slot(6, 14, 0), 0, Alice), Alice);
         ReplyTo(Alice, shorter, "ACCEPTED", 0);
      }

      [Test]
      [Description("Who may book: with a list naming Alice, Bob is declined with the reason and nothing is filed, and Alice is accepted for the same slot")]
      public void AnOrganiserNotAllowedToBookIsDeclined()
      {
         (int status, string body) policy = Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"bookers\":[\"" + Alice + "\"]}");
         Assert.AreEqual(200, policy.status, policy.body);

         string unlisted = "rooms-unlisted@" + _domain.Name;
         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Unlisted", "Unlisted", unlisted, Slot(7, 10, 0), Slot(7, 11, 0), 0, Bob), Bob);
         StringAssert.Contains("You are not one of the people allowed to book Board Room.", ReplyTo(Bob, unlisted, "DECLINED", 0).Text);
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(7)), "Never filed.");

         string listed = "rooms-listed@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Listed", "Listed", listed, Slot(7, 10, 0), Slot(7, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, listed, "ACCEPTED", 0);
      }

      [Test]
      [Description("A request whose organiser did not send it - a local organiser's address written by a session that did not sign in as them, or signed in as somebody who may not send as them - is neither filed nor answered, the delegate is told once an hour for that organiser, and the organiser's own request for the same slot is then accepted")]
      public void ARequestNotFromItsOrganiserIsNeitherFiledNorAnswered()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"delegates\":[\"" + Deputy + "\"]}").status);
         string start = Slot(17, 10, 0);
         string end = Slot(17, 11, 0);

         // MAIL FROM and From both Alice's, with no sign-in: what anybody on
         // the network can write.
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Taken", "Taken", "rooms-forged-1@" + _domain.Name, start, end, 0, Alice, "RRULE:FREQ=DAILY;COUNT=5"));
         MessageIdOf(Deputy, Password, "was not honoured");
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(17)), "Not filed: nothing blocks the slot.");

         // Bob, signed in, writing Alice as the organiser: Bob may not send as Alice.
         SmtpRaw(Bob, Invitation("REQUEST", Alice, "Invitation: Taken again", "Taken again", "rooms-forged-2@" + _domain.Name, start, end, 0, Alice), Bob);
         CustomAsserts.AssertFolderMessageCount(RoomInbox(), 2);

         // Alice's own request for the same slot is decided after both, and accepted.
         string own = "rooms-own@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Mine", "Mine", own, start, end, 0, Alice), Alice);
         ReplyTo(Alice, own, "ACCEPTED", 0);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Alice, Password, 1, TimeSpan.FromSeconds(10));
         Assert.AreEqual(1, CountWithSubject(Deputy, "was not honoured"), "Told once an hour for the one organiser the two named.");
         CustomAsserts.AssertFolderMessageCount(RoomInbox(), 3);
      }

      [Test]
      [Description("An organiser from outside is answered only when named among the bookers and DMARC passed for its domain: named, but from a domain that publishes nothing, the request is neither filed nor answered, nothing leaves the queue, and the delegate is told")]
      public void AnOrganiserFromOutsideIsAnsweredOnlyWhenDmarcVouchesForIt()
      {
         const string partner = "partner@rooms-outside.test";
         (int status, string body) policy = Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy",
            "{\"bookers\":[\"@rooms-outside.test\"],\"delegates\":[\"" + Deputy + "\"]}");
         Assert.AreEqual(200, policy.status, policy.body);

         SmtpRaw(partner, Invitation("REQUEST", partner, "Invitation: Visit", "Visit", "rooms-outside@rooms-outside.test", Slot(8, 10, 0), Slot(8, 11, 0), 0, partner));

         MessageIdOf(Deputy, Password, "was not honoured");
         Response notice = Raw(RestPort, "GET", "/api/v1/me/messages/" + MessageIdOf(Deputy, Password, "was not honoured") + "/source", Auth(Deputy, Password), null, null, null);
         StringAssert.Contains("DMARC did not pass for rooms-outside.test", DecodedParts(notice.Body));
         StringAssert.DoesNotContain("Visit", DecodedParts(notice.Body), "The notice of a request not honoured repeats nothing of it.");

         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(8)), "Not filed.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("An organiser from outside named among the bookers is believed when DMARC passes for its domain - SPF from the sending address, aligned with the From, under a published policy - and answered through the queue, MAIL FROM the room; the same pass for a domain the bookers do not name is not believed, and neither is a named domain whose SPF passes with no DMARC policy to align it")]
      public void AnOrganiserFromOutsideVouchedForByDmarcIsAnswered()
      {
         const string partner = "planner@rooms-vouched.test";
         const string stranger = "planner@rooms-unnamed.test";
         const string spfOnly = "planner@rooms-spfonly.test";

         _application.Settings.AntiSpam.DMARCEnabled = true;
         SuiteDns.Zone
            .WithTxt("rooms-vouched.test", "v=spf1 ip4:127.0.0.1 -all")
            .WithTxt("_dmarc.rooms-vouched.test", "v=DMARC1; p=none")
            .WithTxt("rooms-unnamed.test", "v=spf1 ip4:127.0.0.1 -all")
            .WithTxt("_dmarc.rooms-unnamed.test", "v=DMARC1; p=none")
            .WithTxt("rooms-spfonly.test", "v=spf1 ip4:127.0.0.1 -all");
         try
         {
            (int status, string body) policy = Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy",
               "{\"bookers\":[\"@rooms-vouched.test\",\"@rooms-spfonly.test\"],\"delegates\":[\"" + Deputy + "\"]}");
            Assert.AreEqual(200, policy.status, policy.body);

            Route route = _settings.Routes.Add();
            route.DomainName = "rooms-vouched.test";
            route.TargetSMTPHost = "localhost";
            route.TargetSMTPPort = OutsidePort;
            route.NumberOfTries = 1;
            route.MinutesBetweenTry = 5;
            route.Save();

            using (var server = new SmtpServerSimulator(1, OutsidePort))
            {
               server.SecondsToWaitBeforeTerminate = 60;
               server.AddRecipientResult(new Dictionary<string, int> { { partner, 250 } });
               server.StartListen();

               SmtpRaw(partner, Invitation("REQUEST", partner, "Invitation: Visit", "Visit", "rooms-vouched@rooms-vouched.test", Slot(19, 10, 0), Slot(19, 11, 0), 0, partner));

               server.WaitForCompletion();
               StringAssert.Contains("<" + Room + ">", server.MailFromCommand, "Sent from the room's address.");
               string text = DecodedParts(server.MessageData);
               StringAssert.Contains("METHOD:REPLY", text);
               StringAssert.Contains("PARTSTAT=ACCEPTED", text.Replace("\n ", ""));
               StringAssert.Contains("UID:rooms-vouched@rooms-vouched.test", text);
            }
            CollectionAssert.AreEqual(new[] { Slot(19, 10, 0) + "/" + Slot(19, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(19)), "Booked.");

            // The same pass for a domain the bookers do not name.
            SmtpRaw(stranger, Invitation("REQUEST", stranger, "Invitation: Unnamed", "Unnamed", "rooms-unnamed@rooms-unnamed.test", Slot(19, 14, 0), Slot(19, 15, 0), 0, stranger));
            NoticeContaining(Deputy, "was not honoured", "is not named, by address or @domain, among those who may book");
            CollectionAssert.AreEqual(new[] { Slot(19, 10, 0) + "/" + Slot(19, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(19)), "The stranger's request was not filed.");

            // A named domain whose SPF passes with no DMARC policy: SPF alone
            // vouches for nobody's From, so it is not believed and not answered.
            SmtpRaw(spfOnly, Invitation("REQUEST", spfOnly, "Invitation: Spf only", "Spf only", "rooms-spfonly@rooms-spfonly.test", Slot(19, 16, 0), Slot(19, 17, 0), 0, spfOnly));
            NoticeContaining(Deputy, "was not honoured", "DMARC did not pass for rooms-spfonly.test");
            CollectionAssert.AreEqual(new[] { Slot(19, 10, 0) + "/" + Slot(19, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(19)), "Nor was the SPF-only request.");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         }
         finally
         {
            SuiteDns.Reset();
         }
      }

      [Test]
      [Description("A CANCEL from the booking's organiser frees the slot, and a request for it is then accepted; a CANCEL of the same UID written by somebody else - signed in as Bob with Alice's From, or not signed in at all with Alice's MAIL FROM and From - leaves the booking as it was")]
      public void ACancelFromTheOrganiserFreesTheSlot()
      {
         string uid = "rooms-cancel@" + _domain.Name;
         string start = Slot(9, 10, 0);
         string end = Slot(9, 11, 0);
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Board", "Board", uid, start, end, 0, Alice), Alice);
         ReplyTo(Alice, uid, "ACCEPTED", 0);

         SmtpRaw(Bob, Invitation("CANCEL", Alice, "Cancelled: Board", "Board", uid, start, end, 1, Alice), Bob);
         SmtpRaw(Alice, Invitation("CANCEL", Alice, "Cancelled: Board", "Board", uid, start, end, 1, Alice));
         CustomAsserts.AssertFolderMessageCount(RoomInbox(), 3);

         string takeover = "rooms-takeover@" + _domain.Name;
         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Takeover", "Takeover", takeover, start, end, 0, Bob), Bob);
         ReplyTo(Bob, takeover, "DECLINED", 0);
         CollectionAssert.AreEqual(new[] { start + "/" + end + "/busy" }, Periods(FreeBusyOfRoom(9)), "The forged cancellations freed nothing.");

         // Alice cancels: the slot is free, and Bob's second request takes it.
         SmtpRaw(Alice, Invitation("CANCEL", Alice, "Cancelled: Board", "Board", uid, start, end, 1, Alice), Alice);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.AreEqual(Periods(FreeBusyOfRoom(9)).Count, 0));

         string second = "rooms-second@" + _domain.Name;
         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Second try", "Second try", second, start, end, 0, Bob), Bob);
         ReplyTo(Bob, second, "ACCEPTED", 0);
         CollectionAssert.AreEqual(new[] { start + "/" + end + "/busy" }, Periods(FreeBusyOfRoom(9)));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Alice, Password, 1, TimeSpan.FromSeconds(10));
      }

      [Test]
      [Description("An updated REQUEST (a higher SEQUENCE) is decided again: moved onto a slot another booking holds it is declined and its old slot freed, and moved back it is accepted again, the REPLY carrying the new SEQUENCE")]
      public void AnUpdatedRequestIsDecidedAgain()
      {
         string uid = "rooms-update@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Review", "Review first", uid, Slot(13, 10, 0), Slot(13, 11, 0), 0, Alice), Alice);
         StringAssert.Contains("\"sequence\":0", ReplyTo(Alice, uid, "ACCEPTED", 0).Card);

         string bobs = "rooms-update-bob@" + _domain.Name;
         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Bob's hour", "Bob hour", bobs, Slot(13, 14, 0), Slot(13, 15, 0), 0, Bob), Bob);
         ReplyTo(Bob, bobs, "ACCEPTED", 0);

         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Updated invitation: Review", "Review moved", uid, Slot(13, 14, 30), Slot(13, 15, 30), 1, Alice), Alice);
         Reply moved = ReplyTo(Alice, uid, "DECLINED", 1);
         StringAssert.Contains("\"sequence\":1", moved.Card);
         StringAssert.Contains("\"uid\":\"" + uid + "\"", moved.Card);
         CollectionAssert.AreEqual(new[] { Slot(13, 14, 0) + "/" + Slot(13, 15, 0) + "/busy" }, Periods(FreeBusyOfRoom(13)),
            "Declined again, the booking is out of the room's calendar: the ten o'clock slot is free and only Bob's hour is held.");

         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Updated invitation: Review", "Review back", uid, Slot(13, 10, 0), Slot(13, 11, 0), 2, Alice), Alice);
         Reply back = ReplyTo(Alice, uid, "ACCEPTED", 2);
         StringAssert.Contains("\"method\":\"REPLY\"", back.Card);
         CollectionAssert.AreEqual(new[] { Slot(13, 10, 0) + "/" + Slot(13, 11, 0) + "/busy", Slot(13, 14, 0) + "/" + Slot(13, 15, 0) + "/busy" }, Periods(FreeBusyOfRoom(13)));
      }

      [Test]
      [Description("A REQUEST carrying only a RECURRENCE-ID - one occurrence of a series moved, as Google and Apple send it - is decided as that occurrence alone: accepted, it is merged into the series; declined, that occurrence alone leaves the series, whose other occurrences stay booked")]
      public void AChangeToOneOccurrenceIsDecidedAloneAndMergedIntoTheSeries()
      {
         string uid = "rooms-weekly@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Weekly", "Weekly", uid, Slot(20, 10, 0), Slot(20, 11, 0), 0, Alice, "RRULE:FREQ=WEEKLY;COUNT=3"), Alice);
         ReplyTo(Alice, uid, "ACCEPTED", 0);

         string bobs = "rooms-weekly-bob@" + _domain.Name;
         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Bob's afternoon", "Bob afternoon", bobs, Slot(28, 14, 0), Slot(28, 15, 0), 0, Bob), Bob);
         ReplyTo(Bob, bobs, "ACCEPTED", 0);

         // The second occurrence moved to the afternoon: accepted, merged.
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Updated invitation: Weekly", "Weekly", uid, Slot(27, 16, 0), Slot(27, 17, 0), 1, Alice,
            "RECURRENCE-ID:" + Slot(27, 10, 0)), Alice);
         Reply merged = ReplyTo(Alice, uid, "ACCEPTED", 1);
         StringAssert.Contains("RECURRENCE-ID:" + Slot(27, 10, 0), merged.Text.Replace("\n ", ""), "The answer is about that occurrence.");
         CollectionAssert.AreEqual(new[] { Slot(20, 10, 0) + "/" + Slot(20, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(20)), "The first occurrence untouched.");
         CollectionAssert.AreEqual(new[] { Slot(27, 16, 0) + "/" + Slot(27, 17, 0) + "/busy" }, Periods(FreeBusyOfRoom(27)), "The second where it moved to.");
         CollectionAssert.AreEqual(new[] { Slot(34, 10, 0) + "/" + Slot(34, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(34)), "The third untouched.");

         // The third occurrence moved onto Bob's afternoon: declined, and only
         // that occurrence leaves the series.
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Updated invitation: Weekly", "Weekly", uid, Slot(28, 14, 0), Slot(28, 15, 0), 1, Alice,
            "RECURRENCE-ID:" + Slot(34, 10, 0)), Alice);
         Reply refused = ReplyTo(Alice, uid, "DECLINED", 1);
         StringAssert.Contains("RECURRENCE-ID:" + Slot(34, 10, 0), refused.Text.Replace("\n ", ""));
         StringAssert.Contains("Board Room is already booked at that time", refused.Text);
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(34)), "The declined occurrence is out of the series.");
         CollectionAssert.AreEqual(new[] { Slot(20, 10, 0) + "/" + Slot(20, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(20)), "The series stands.");
         CollectionAssert.AreEqual(new[] { Slot(27, 16, 0) + "/" + Slot(27, 17, 0) + "/busy" }, Periods(FreeBusyOfRoom(27)), "And so does the change accepted before.");
         CollectionAssert.AreEqual(new[] { Slot(28, 14, 0) + "/" + Slot(28, 15, 0) + "/busy" }, Periods(FreeBusyOfRoom(28)), "Bob's afternoon is Bob's.");
      }

      [Test]
      [Description("A room invited to occurrences of a series and never to the series holds each at its own SEQUENCE: a change of one is compared with that occurrence alone - accepted at SEQUENCE 2 though another is held at 3 - a CANCEL of the whole booking older than an occurrence it holds leaves the booking, a CANCEL of one occurrence frees that one and leaves the other, and a CANCEL of the whole booking as new as what it holds frees it")]
      public void OccurrencesHeldWithoutTheirSeriesAreComparedOneByOne()
      {
         string uid = "rooms-occurrences@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Standup", "Standup", uid, Slot(40, 10, 0), Slot(40, 11, 0), 3, Alice,
            "RECURRENCE-ID:" + Slot(40, 10, 0)), Alice);
         ReplyTo(Alice, uid, "ACCEPTED", 3);
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Standup", "Standup", uid, Slot(41, 10, 0), Slot(41, 11, 0), 1, Alice,
            "RECURRENCE-ID:" + Slot(41, 10, 0)), Alice);
         ReplyTo(Alice, uid, "ACCEPTED", 1);
         CollectionAssert.AreEqual(new[] { Slot(40, 10, 0) + "/" + Slot(40, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(40)));
         CollectionAssert.AreEqual(new[] { Slot(41, 10, 0) + "/" + Slot(41, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(41)));

         // The second moved at SEQUENCE 2: newer than that occurrence (1),
         // though older than the first (3).
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Updated invitation: Standup", "Standup", uid, Slot(41, 14, 0), Slot(41, 15, 0), 2, Alice,
            "RECURRENCE-ID:" + Slot(41, 10, 0)), Alice);
         ReplyTo(Alice, uid, "ACCEPTED", 2);
         CollectionAssert.AreEqual(new[] { Slot(41, 14, 0) + "/" + Slot(41, 15, 0) + "/busy" }, Periods(FreeBusyOfRoom(41)), "Where it moved to.");
         CollectionAssert.AreEqual(new[] { Slot(40, 10, 0) + "/" + Slot(40, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(40)), "The first untouched.");

         // A CANCEL of the whole booking at SEQUENCE 0 is older than the first
         // occurrence: the booking stands, which Bob's request for that hour,
         // decided after the CANCEL, shows.
         SmtpRaw(Alice, Invitation("CANCEL", Alice, "Cancelled: Standup", "Standup", uid, Slot(40, 10, 0), Slot(40, 11, 0), 0, Alice), Alice);
         CustomAsserts.AssertFolderMessageCount(RoomInbox(), 4);
         string bobs = "rooms-occurrences-bob@" + _domain.Name;
         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Bob's standup", "Bob standup", bobs, Slot(40, 10, 0), Slot(40, 11, 0), 0, Bob), Bob);
         ReplyTo(Bob, bobs, "DECLINED", 0);
         CollectionAssert.AreEqual(new[] { Slot(40, 10, 0) + "/" + Slot(40, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(40)), "The stale CANCEL freed nothing.");
         CollectionAssert.AreEqual(new[] { Slot(41, 14, 0) + "/" + Slot(41, 15, 0) + "/busy" }, Periods(FreeBusyOfRoom(41)));

         // A CANCEL of the second occurrence at its own SEQUENCE frees it alone.
         SmtpRaw(Alice, Invitation("CANCEL", Alice, "Cancelled: Standup", "Standup", uid, Slot(41, 14, 0), Slot(41, 15, 0), 2, Alice,
            "RECURRENCE-ID:" + Slot(41, 10, 0)), Alice);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.AreEqual(Periods(FreeBusyOfRoom(41)).Count, 0));
         CollectionAssert.AreEqual(new[] { Slot(40, 10, 0) + "/" + Slot(40, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(40)), "The first occurrence stands.");

         // And a CANCEL of the whole booking at SEQUENCE 3 frees what is left.
         SmtpRaw(Alice, Invitation("CANCEL", Alice, "Cancelled: Standup", "Standup", uid, Slot(40, 10, 0), Slot(40, 11, 0), 3, Alice), Alice);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.AreEqual(Periods(FreeBusyOfRoom(40)).Count, 0));
      }

      [Test]
      [Description("A room holding one occurrence of a series at SEQUENCE 3, and not the series, is freed by the organiser's CANCEL of the whole series at SEQUENCE 1 that names that occurrence at SEQUENCE 3 too: an occurrence the CANCEL names is compared with the CANCEL's own version of it, never with the series' SEQUENCE")]
      public void ACancelOfTheSeriesNamingTheHeldOccurrenceFreesIt()
      {
         string uid = "rooms-cancel-named@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Retro", "Retro", uid, Slot(47, 10, 0), Slot(47, 11, 0), 3, Alice,
            "RECURRENCE-ID:" + Slot(47, 10, 0)), Alice);
         ReplyTo(Alice, uid, "ACCEPTED", 3);
         CollectionAssert.AreEqual(new[] { Slot(47, 10, 0) + "/" + Slot(47, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(47)));

         // The series at SEQUENCE 1, and the occurrence the room holds at 3.
         string cancel = string.Join("\r\n", new[]
         {
            "From: <" + Alice + ">", "To: <" + Room + ">", "Subject: Cancelled: Retro", "MIME-Version: 1.0", "Content-Type: text/calendar; charset=utf-8; method=CANCEL", "",
            "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A client//EN", "METHOD:CANCEL",
            "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z", "DTSTART:" + Slot(47, 10, 0), "DTEND:" + Slot(47, 11, 0),
            "RRULE:FREQ=WEEKLY;COUNT=3", "SUMMARY:Retro", "SEQUENCE:1", "STATUS:CANCELLED", "ORGANIZER:mailto:" + Alice,
            "ATTENDEE;CUTYPE=ROOM:mailto:" + Room, "END:VEVENT",
            "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z", "RECURRENCE-ID:" + Slot(47, 10, 0), "DTSTART:" + Slot(47, 10, 0), "DTEND:" + Slot(47, 11, 0),
            "SUMMARY:Retro", "SEQUENCE:3", "STATUS:CANCELLED", "ORGANIZER:mailto:" + Alice,
            "ATTENDEE;CUTYPE=ROOM:mailto:" + Room, "END:VEVENT",
            "END:VCALENDAR", ""
         });
         SmtpRaw(Alice, cancel, Alice);
         RetryHelper.TryAction(TimeSpan.FromSeconds(20), () => RetryableAssert.AreEqual(Periods(FreeBusyOfRoom(47)).Count, 0));
      }

      [Test]
      [Description("An invitation whose summary is outside the Basic Multilingual Plane - an emoji, four bytes of UTF-8 - is queued, decided and answered with its summary whole: on MySQL a queue table of three-byte utf8 would refuse the row, and nothing would decide it")]
      public void AnInvitationWithAnEmojiIsAnswered()
      {
         string uid = "rooms-emoji@" + _domain.Name;
         string summary = "Launch \U0001F680";
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Launch", summary, uid, Slot(42, 10, 0), Slot(42, 11, 0), 0, Alice), Alice);
         Reply reply = ReplyTo(Alice, uid, "ACCEPTED", 0);
         StringAssert.Contains("SUMMARY:" + summary, reply.Text.Replace("\n ", ""), "The answer carries the summary as it was sent.");
         CollectionAssert.AreEqual(new[] { Slot(42, 10, 0) + "/" + Slot(42, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(42)));
      }

      [Test]
      [Description("With automatic answers off a request is left for the delegate: no REPLY is sent, nothing is put into the room's calendar, the request is in the room's mailbox and the delegate is told by mail. A booking the room holds the delegate declines on its behalf - over REST, From the room with Sender the delegate, and from a CalDAV client - and each leaves the calendar")]
      public void WithAutomaticAnswersOffTheRequestIsLeftForTheDelegate()
      {
         (int status, string body) policy = Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy",
            "{\"auto_accept\":false,\"delegates\":[\"" + Deputy + "\"]}");
         Assert.AreEqual(200, policy.status, policy.body);
         StringAssert.Contains("\"auto_accept\":false", policy.body);
         StringAssert.Contains("\"delegates\":[\"" + Deputy + "\"]", policy.body);

         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Sync", "Sync", "rooms-manual@" + _domain.Name, Slot(14, 10, 0), Slot(14, 11, 0), 0, Alice), Alice);

         long notice = MessageIdOf(Deputy, Password, "Booking request for Board Room: Sync");
         Response noticeSource = Raw(RestPort, "GET", "/api/v1/me/messages/" + notice + "/source", Auth(Deputy, Password), null, null, null);
         string noticeText = DecodedParts(noticeSource.Body);
         StringAssert.Contains("it answers no invitation automatically", noticeText);
         StringAssert.Contains("It is in the mailbox of Board Room, and not in its calendar", noticeText);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Alice, Password, 0);
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(14)), "Nothing the room did not decide is booked.");
         CustomAsserts.AssertFolderMessageCount(RoomInbox(), 1);

         // With automatic answers on, a booking the room accepted, which the
         // delegate then declines on its behalf.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"auto_accept\":true}").status);
         string retro = "rooms-delegate-rest@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Retro", "Retro", retro, Slot(15, 10, 0), Slot(15, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, retro, "ACCEPTED", 0);

         (int status, string body) seen = Me("GET", "/api/v1/me/calendar/events?start=" + Slot(15, 0, 0) + "&end=" + Slot(16, 0, 0), null, Deputy, Password);
         Assert.AreEqual(200, seen.status, seen.body);
         StringAssert.Contains("\"summary\":\"Retro\"", seen.body, "The delegate sees the room's booking.");
         StringAssert.Contains("\"delegate\":true", seen.body);
         long eventId = long.Parse(Regex.Match(seen.body.Substring(seen.body.IndexOf("\"events\":[", StringComparison.Ordinal)), "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) answered = Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"DECLINED\"}", Deputy, Password);
         Assert.AreEqual(200, answered.status, answered.body);

         Reply reply = ReplyIn(Alice, "Declined: Retro");
         StringAssert.Contains("\"partstat\":\"DECLINED\",\"sent_by\":\"" + Deputy + "\"", reply.Card, "The delegate answered for the room.");
         StringAssert.Contains("From: \"Board Room\" <" + Room + ">", reply.Source);
         StringAssert.Contains("Sender: \"Dee Puty\" <" + Deputy + ">", reply.Source);
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(15)), "A booking the delegate declined holds nothing.");

         // And from a CalDAV client: the delegate writes the room's decline back
         // into the object in the room's calendar, shared with them.
         string planning = "rooms-delegate-caldav@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Planning", "Planning", planning, Slot(16, 10, 0), Slot(16, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, planning, "ACCEPTED", 0);
         CollectionAssert.AreEqual(new[] { Slot(16, 10, 0) + "/" + Slot(16, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(16)));

         (int status, string body) calendars = Me("GET", "/api/v1/me/calendars", null, Deputy, Password);
         string roomCalendarId = Regex.Match(calendars.body, "\\{\"id\":(\\d+),\"name\":\"[^\"]*\",\"display_name\":\"[^\"]*\",\"color\":\"[^\"]*\",\"owner\":\"" + Regex.Escape(Room) + "\"").Groups[1].Value;
         Assert.IsNotEmpty(roomCalendarId, calendars.body);
         string objectHref = "/dav/calendars/" + Deputy + "/shared-" + roomCalendarId + "/" + planning + ".ics";
         string declined = string.Join("\r\n", new[]
         {
            "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A client//EN", "BEGIN:VEVENT", "UID:" + planning, "DTSTAMP:20260101T000000Z",
            "DTSTART:" + Slot(16, 10, 0), "DTEND:" + Slot(16, 11, 0), "SUMMARY:Planning", "SEQUENCE:0", "ORGANIZER:mailto:" + Alice,
            "ATTENDEE;ROLE=CHAIR;PARTSTAT=ACCEPTED:mailto:" + Alice,
            "ATTENDEE;CUTYPE=ROOM;PARTSTAT=DECLINED:mailto:" + Room,
            "END:VEVENT", "END:VCALENDAR", ""
         });
         Response put = Raw(WebServicesPort, "PUT", objectHref, Auth(Deputy, Password), declined, "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\n");
         Assert.AreEqual(204, put.Status, put.Body);
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(16)), "Declined from a CalDAV client, the booking left the calendar.");

         // And the organiser was told, as the REST decline tells them.
         Reply told = ReplyIn(Alice, "Declined: Planning");
         StringAssert.Contains("\"partstat\":\"DECLINED\",\"sent_by\":\"" + Deputy + "\"", told.Card, "The delegate declined for the room.");
         StringAssert.Contains("From: \"Board Room\" <" + Room + ">", told.Source);
         StringAssert.Contains("Sender: \"Dee Puty\" <" + Deputy + ">", told.Source);
      }

      [Test]
      [Description("The organiser the room believed is kept per UID exactly: a booking by Bob whose UID differs from Alice's only in a character the database's collation does not tell apart takes nothing of Alice's, and a delegate's decline of Alice's booking tells Alice")]
      public void ABookingWhoseUidTheCollationTakesForAnothersLeavesTheOtherOrganiserBelieved()
      {
         // Until 26 September 2026 the room's record of whom it accepted a booking
         // from was written, read and removed with "bduid = @UID". On SQL Server
         // and Compact that compares under a collation that ignores case and width,
         // so Bob's acceptance - his UID starting with a full-width 'r', which the
         // calendar keeps apart (its file name is another) - removed Alice's record
         // and wrote his own, and the delegate's decline of Alice's booking then
         // read Bob as the organiser, did not match the booking's ORGANIZER and
         // told nobody. This test failed against that code there; MySQL and
         // PostgreSQL compared the UID exactly already.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"delegates\":[\"" + Deputy + "\"]}").status);

         string alices = "rooms-twin-0001@" + _domain.Name;
         string bobs = "ｒooms-twin-0001@" + _domain.Name;

         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Tandem", "Tandem", alices, Slot(52, 10, 0), Slot(52, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, alices, "ACCEPTED", 0);

         SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Relay", "Relay", bobs, Slot(53, 10, 0), Slot(53, 11, 0), 0, Bob), Bob);
         ReplyIn(Bob, "Accepted: Relay");

         (int status, string body) seen = Me("GET", "/api/v1/me/calendar/events?start=" + Slot(52, 0, 0) + "&end=" + Slot(53, 0, 0), null, Deputy, Password);
         Assert.AreEqual(200, seen.status, seen.body);
         StringAssert.Contains("\"summary\":\"Tandem\"", seen.body, "The delegate sees Alice's booking.");
         long eventId = long.Parse(Regex.Match(seen.body.Substring(seen.body.IndexOf("\"events\":[", StringComparison.Ordinal)), "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) answered = Me("POST", "/api/v1/me/calendar/events/" + eventId + "/reply", "{\"partstat\":\"DECLINED\"}", Deputy, Password);
         Assert.AreEqual(200, answered.status, answered.body);

         Reply told = ReplyIn(Alice, "Declined: Tandem");
         StringAssert.Contains("\"partstat\":\"DECLINED\",\"sent_by\":\"" + Deputy + "\"", told.Card, "Alice, whom the room believed, is told.");
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(52)), "Alice's booking left the calendar.");
         CollectionAssert.AreEqual(new[] { Slot(53, 10, 0) + "/" + Slot(53, 11, 0) + "/busy" }, Periods(FreeBusyOfRoom(53)), "Bob's stays.");

         // Bob has his acceptance and nothing about Alice's booking.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Bob, Password, 1);
      }

      [Test]
      [Description("A decline written from a CalDAV client tells somebody only when a delegate wrote it, and only the organiser the room believed when it accepted the booking: a colleague with a plain write share of the room's calendar who declines for it takes the booking out and mails nobody, nor after writing Bob in as the ORGANIZER first; a delegate's decline of a booking whose ORGANIZER that colleague rewrote mails nobody; and a delegate whose own PUT names Bob tells Alice, whom the room believed, and not Bob")]
      public void ACalDavDeclineTellsOnlyTheOrganiserTheRoomBelievedAndOnlyForADelegate()
      {
         // A plain write share of the room's calendar, made while it was a
         // person - a room cannot sign in to make one - and a delegate named on
         // its policy.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room, "{\"kind\":\"person\"}").status);
         long calendarId = long.Parse(Regex.Match(Me("GET", "/api/v1/me/calendars", null, Room, Password).body, "\"id\":(\\d+),\"name\":\"calendar\"").Groups[1].Value);
         Assert.AreEqual(200, Me("PUT", "/api/v1/me/calendars/" + calendarId + "/shares/" + Colleague, "{\"write\":true}", Room, Password).status);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room, "{\"kind\":\"room\"}").status);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"delegates\":[\"" + Deputy + "\"]}").status);

         // The plain writer declines a booking Alice made, as it stands.
         string plain = "rooms-writer@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Offsite", "Offsite", plain, Slot(43, 10, 0), Slot(43, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, plain, "ACCEPTED", 0);
         PutIntoRoomCalendar(Colleague, calendarId, plain, Booking(plain, 43, "Offsite", Alice, "DECLINED"));
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(43)), "Anybody who may write to the calendar may take a booking out of it, as a DELETE does.");

         // And the two PUTs of the re-check: Bob written in as the organiser,
         // then the room's decline.
         string forged = "rooms-forged@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Workshop", "Workshop", forged, Slot(44, 10, 0), Slot(44, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, forged, "ACCEPTED", 0);
         PutIntoRoomCalendar(Colleague, calendarId, forged, Booking(forged, 44, "Workshop", Bob, "ACCEPTED"));
         PutIntoRoomCalendar(Colleague, calendarId, forged, Booking(forged, 44, "Workshop", Bob, "DECLINED"));
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(44)));

         // A delegate declines a booking whose ORGANIZER the writer rewrote:
         // the room did not accept it from Bob, and it no longer names Alice.
         string rewritten = "rooms-rewritten@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Clinic", "Clinic", rewritten, Slot(45, 10, 0), Slot(45, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, rewritten, "ACCEPTED", 0);
         PutIntoRoomCalendar(Colleague, calendarId, rewritten, Booking(rewritten, 45, "Clinic", Bob, "ACCEPTED"));
         PutIntoRoomCalendar(Deputy, calendarId, rewritten, Booking(rewritten, 45, "Clinic", Bob, "DECLINED"));
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(45)));

         // A delegate's own PUT naming Bob: Alice, whom the room believed, is
         // told, from the booking as the room held it.
         string believed = "rooms-believed@" + _domain.Name;
         SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Review", "Review", believed, Slot(46, 10, 0), Slot(46, 11, 0), 0, Alice), Alice);
         ReplyTo(Alice, believed, "ACCEPTED", 0);
         PutIntoRoomCalendar(Deputy, calendarId, believed, Booking(believed, 46, "Review", Bob, "DECLINED"));
         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(46)));
         Reply told = ReplyIn(Alice, "Declined: Review");
         StringAssert.Contains("\"partstat\":\"DECLINED\",\"sent_by\":\"" + Deputy + "\"", told.Card, "The delegate declined for the room.");
         StringAssert.Contains("\"organizer\":\"" + Alice + "\"", told.Card, "About the booking as the room held it.");

         // Nothing else left: Bob was never mailed, and Alice has her four
         // acceptances and the one decline.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Bob, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(Alice, Password, 5);
      }

      [Test]
      [Description("A request sent automatically - Auto-Submitted other than no - is not answered automatically (RFC 3834): it is left for the delegate, who is told, and not put into the room's calendar")]
      public void AnAutomaticRequestIsNotAnsweredAutomatically()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"delegates\":[\"" + Deputy + "\"]}").status);

         SmtpRaw(Alice, "Auto-Submitted: auto-generated\r\n" +
            Invitation("REQUEST", Alice, "Invitation: Robot", "Robot", "rooms-robot@" + _domain.Name, Slot(18, 10, 0), Slot(18, 11, 0), 0, Alice), Alice);

         long notice = MessageIdOf(Deputy, Password, "Booking request for Board Room: Robot");
         Response noticeSource = Raw(RestPort, "GET", "/api/v1/me/messages/" + notice + "/source", Auth(Deputy, Password), null, null, null);
         StringAssert.Contains("RFC 3834", DecodedParts(noticeSource.Body));

         CollectionAssert.IsEmpty(Periods(FreeBusyOfRoom(18)), "Nothing the room did not decide is booked.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Alice, Password, 0);
      }

      [Test]
      [Description("Two requests for the same slot delivered at once end with one booking: one accepted, the other declined. The decisions for one room are made one after the other on its own queue; this checks the outcome, and cannot tell whether two decisions would have raced without that queue")]
      public void TwoRequestsForOneSlotEndWithOneBooking()
      {
         string start = Slot(16, 9, 0);
         string end = Slot(16, 10, 0);
         string clashA = "rooms-clash-a@" + _domain.Name;
         string clashB = "rooms-clash-b@" + _domain.Name;
         Task first = Task.Run(() => SmtpRaw(Alice, Invitation("REQUEST", Alice, "Invitation: Clash A", "Clash A", clashA, start, end, 0, Alice), Alice));
         Task second = Task.Run(() => SmtpRaw(Bob, Invitation("REQUEST", Bob, "Invitation: Clash B", "Clash B", clashB, start, end, 0, Bob), Bob));
         Task.WaitAll(first, second);

         Reply a = ReplyTo(Alice, clashA);
         Reply b = ReplyTo(Bob, clashB);
         bool aAccepted = a.Card.Contains("\"partstat\":\"ACCEPTED\"");
         bool bAccepted = b.Card.Contains("\"partstat\":\"ACCEPTED\"");
         Assert.AreEqual(1, (aAccepted ? 1 : 0) + (bAccepted ? 1 : 0), "Exactly one accepted. A: " + a.Card + " B: " + b.Card);
         StringAssert.Contains("\"partstat\":\"DECLINED\"", aAccepted ? b.Card : a.Card, "And the other declined.");
         CollectionAssert.AreEqual(new[] { start + "/" + end + "/busy" }, Periods(FreeBusyOfRoom(16)), "One booking.");
      }

      [Test]
      [Description("A room cannot be signed into - REST, POP3 and IMAP all refuse its password - and it signs in again once it is a person; COM reads and writes Kind and refuses a value that is none of the three")]
      public void ARoomCannotBeSignedInto()
      {
         Assert.AreEqual(401, Me("GET", "/api/v1/me", null, Room, Password).status, "REST refuses the room.");
         Assert.IsFalse(new Pop3ClientSimulator().ConnectAndLogon(Room, Password), "POP3 refuses the room.");
         Assert.IsFalse(new ImapClientSimulator().ConnectAndLogon(Room, Password), "IMAP refuses the room.");

         // dynamic because the test project's interop predates the property.
         dynamic account = _application.Domains.ItemByName[_domain.Name].Accounts.ItemByAddress[Room];
         Assert.AreEqual(1, (int) account.Kind, "COM reads what REST wrote.");
         Assert.Throws<COMException>(() => { account.Kind = 5; }, "Not one of the three.");
         account.Kind = 0;
         account.Save();

         Assert.AreEqual(200, Me("GET", "/api/v1/me", null, Room, Password).status, "A person again, it signs in.");
         Assert.IsTrue(new Pop3ClientSimulator().ConnectAndLogon(Room, Password), "POP3 too.");
         StringAssert.Contains("\"kind\":\"person\"", Admin("GET", "/api/v1/accounts/" + Room, null).body);
      }

      [Test]
      [Description("The room is in the directory as a room with its capacity; over CalDAV its principal says calendar-user-type ROOM, the principal-property-search a room finder sends finds it and no person, and a person's principal stays that person's")]
      public void TheDirectoryAndTheRoomFinderShowTheRoom()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + Room + "/booking-policy", "{\"capacity\":12}").status);

         (int status, string body) directory = Me("GET", "/api/v1/me/directory", null, Colleague, Password);
         Assert.AreEqual(200, directory.status, directory.body);
         string entry = Regex.Match(directory.body, "\\{[^{}]*\"address\":\"" + Regex.Escape(Room) + "\"[^{}]*\\}").Value;
         StringAssert.Contains("\"kind\":\"room\"", entry, directory.body);
         StringAssert.Contains("\"capacity\":12", entry, directory.body);
         StringAssert.Contains("\"kind\":\"person\"", Regex.Match(directory.body, "\\{[^{}]*\"address\":\"" + Regex.Escape(Alice) + "\"[^{}]*\\}").Value, "A person is a person.");

         string search = "<D:principal-property-search xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">" +
            "<D:property-search><D:prop><C:calendar-user-type/></D:prop><D:match>ROOM</D:match></D:property-search>" +
            "<D:prop><D:displayname/><C:calendar-user-type/><C:calendar-user-address-set/></D:prop></D:principal-property-search>";
         Response found = Dav("REPORT", "/dav/principals/", Auth(Colleague, Password), search, "Depth: 0\r\n");
         Assert.AreEqual(207, found.Status, found.Body);
         StringAssert.Contains("/dav/principals/" + Room + "/", found.Body);
         StringAssert.Contains("ROOM", found.Body);
         StringAssert.Contains("mailto:" + Room, found.Body);
         StringAssert.Contains("Board Room", found.Body);
         StringAssert.DoesNotContain(Alice, found.Body, "People are not in the room finder.");

         Response principal = Dav("PROPFIND", "/dav/principals/" + Room + "/", Auth(Colleague, Password),
            "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\"><D:prop><C:calendar-user-type/><D:displayname/></D:prop></D:propfind>", "Depth: 0\r\n");
         Assert.AreEqual(207, principal.Status, principal.Body);
         StringAssert.Contains("ROOM</", principal.Body);

         Response own = Dav("PROPFIND", "/dav/principals/" + Colleague + "/", Auth(Colleague, Password),
            "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\"><D:prop><C:calendar-user-type/></D:prop></D:propfind>", "Depth: 0\r\n");
         Assert.AreEqual(207, own.Status, own.Body);
         StringAssert.Contains("INDIVIDUAL</", own.Body);

         Assert.AreEqual(404, Dav("PROPFIND", "/dav/principals/" + Alice + "/", Auth(Colleague, Password),
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:displayname/></D:prop></D:propfind>", "Depth: 0\r\n").Status, "Another person's principal is not there.");
      }

      [Test]
      [Description("The booking policy route: a person's policy is read and not written (409); each field refused by name when it is wrong; a whole policy round-trips with the bookers in their stored form and the delegate made a delegate of the room's calendar, and taken away again; a group is kept by its own name, and one deleted since does not refuse a later change")]
      public void TheBookingPolicyRouteKeepsWhatItIsGivenAndRefusesTheRest()
      {
         (int status, string body) person = Admin("GET", "/api/v1/accounts/" + Alice + "/booking-policy", null);
         Assert.AreEqual(200, person.status, person.body);
         StringAssert.Contains("\"kind\":\"person\",\"stored\":false", person.body);
         Assert.AreEqual(409, Admin("PUT", "/api/v1/accounts/" + Alice + "/booking-policy", "{\"capacity\":4}").status, "A person has no policy to set.");

         string route = "/api/v1/accounts/" + Room + "/booking-policy";
         Assert.AreEqual(400, Admin("PUT", route, "{\"window_days\":0}").status, "A window of no days.");
         Assert.AreEqual(400, Admin("PUT", route, "{\"window_days\":1081}").status, "Past the ceiling.");
         Assert.AreEqual(400, Admin("PUT", route, "{\"time_zone\":\"Mars/Olympus_Mons\"}").status, "A zone this server does not know.");
         Assert.AreEqual(400, Admin("PUT", route, "{\"work_start\":\"25:00\"}").status);
         Assert.AreEqual(400, Admin("PUT", route, "{\"work_start\":\"17:00\",\"work_end\":\"09:00\"}").status, "The end before the start.");
         Assert.AreEqual(400, Admin("PUT", route, "{\"work_days\":[\"mon\",\"funday\"]}").status);
         Assert.AreEqual(400, Admin("PUT", route, "{\"bookers\":[\"No Such Group\"]}").status);
         Assert.AreEqual(400, Admin("PUT", route, "{\"delegates\":[\"nobody@" + _domain.Name + "\"]}").status);
         Assert.AreEqual(400, Admin("PUT", route, "{\"colour\":\"red\"}").status, "An unknown field.");

         (int status, string body) saved = Admin("PUT", route,
            "{\"window_days\":90,\"max_duration_minutes\":120,\"working_hours_only\":true,\"work_days\":[\"wed\",\"mon\"],\"work_start\":\"09:30\",\"work_end\":\"16:00\"," +
            "\"time_zone\":\"Europe/London\",\"allow_conflicts\":true,\"allow_recurring\":false,\"bookers\":[\"@Partner.Example\",\"" + Alice.ToUpperInvariant() + "\",\"" + Alice + "\"]," +
            "\"capacity\":12,\"delegates\":[\"" + Deputy + "\"]}");
         Assert.AreEqual(200, saved.status, saved.body);
         StringAssert.Contains("\"stored\":true,\"auto_accept\":true,\"window_days\":90,\"max_duration_minutes\":120,\"working_hours_only\":true,\"work_days\":[\"mon\",\"wed\"]," +
            "\"work_start\":\"09:30\",\"work_end\":\"16:00\",\"time_zone\":\"Europe/London\",\"allow_conflicts\":true,\"allow_recurring\":false," +
            "\"bookers\":[\"@partner.example\",\"" + Alice + "\"],\"capacity\":12,\"delegates\":[\"" + Deputy + "\"]", saved.body, "Lower-cased once each, in its stored form.");
         Assert.AreEqual(saved.body, Admin("GET", route, null).body, "Read back as saved.");

         StringAssert.Contains("\"delegate\":true", Me("GET", "/api/v1/me/calendars", null, Deputy, Password).body, "The delegate has the room's calendar as a delegate.");
         (int status, string body) cleared = Admin("PUT", route, "{\"delegates\":[]}");
         Assert.AreEqual(200, cleared.status, cleared.body);
         StringAssert.Contains("\"delegates\":[]", cleared.body);
         StringAssert.DoesNotContain("\"delegate\":true", Me("GET", "/api/v1/me/calendars", null, Deputy, Password).body, "And no longer.");

         // A group named in any case is kept by its own name; deleted since, it
         // matches nobody and refuses no later change.
         hMailServer.Group bookers = _settings.Groups.Add();
         bookers.Name = "Room Bookers";
         bookers.Save();
         (int status, string body) grouped = Admin("PUT", route, "{\"bookers\":[\"room bookers\"]}");
         Assert.AreEqual(200, grouped.status, grouped.body);
         StringAssert.Contains("\"bookers\":[\"Room Bookers\"]", grouped.body);
         bookers.Delete();
         Assert.AreEqual(200, Admin("PUT", route, "{\"capacity\":5}").status, "A group deleted since it was named refuses nothing.");
      }

      // ------------------------------------------------------------- helpers

      private sealed class Reply
      {
         public string Card = "";
         public string Source = "";
         public string Text = "";
      }

      // The answer the organiser received, by a part of its subject: its card
      // (what the calendar part says), its source and its parts decoded. For
      // an answer whose subject repeats the request - a delegate's.
      private Reply ReplyIn(string address, string subjectPart)
      {
         return ReplyById(address, MessageIdOf(address, Password, subjectPart));
      }

      // The room's answer to one invitation, found by what its calendar part
      // says - the UID, and when given the room's PARTSTAT and the SEQUENCE -
      // since a decline's subject quotes nothing of the request.
      private Reply ReplyTo(string address, string uid, string partstat = null, int? sequence = null)
      {
         long found = 0;
         string[] words = partstat == null ? new[] { "Accepted", "Declined" } : new[] { partstat == "ACCEPTED" ? "Accepted" : "Declined" };
         RetryHelper.TryAction(TimeSpan.FromSeconds(30), () =>
         {
            foreach (string word in words)
            {
               (int status, string body) search = Me("GET", "/api/v1/me/search?q=" + Uri.EscapeDataString(word), null, address, Password);
               RetryableAssert.AreEqual(search.status, 200);
               foreach (Match item in Regex.Matches(search.body, "\"id\":(\\d+),\"uid\":[^{}]*?\"subject\":\"([^\"]*)\""))
               {
                  if (!item.Groups[2].Value.StartsWith(word + ":", StringComparison.Ordinal))
                     continue;
                  long id = long.Parse(item.Groups[1].Value);
                  (int status, string body) card = Me("GET", "/api/v1/me/messages/" + id + "/invitation", null, address, Password);
                  if (card.status != 200 || !card.body.Contains("\"uid\":\"" + uid + "\""))
                     continue;
                  if (partstat != null && !card.body.Contains("\"partstat\":\"" + partstat + "\""))
                     continue;
                  if (sequence.HasValue && !Regex.IsMatch(card.body, "\"sequence\":" + sequence.Value + "[,}]"))
                     continue;
                  found = id;
                  return;
               }
            }
            throw new Exception("No answer to " + uid + (partstat == null ? "" : " " + partstat) + (sequence.HasValue ? " at SEQUENCE " + sequence.Value : "") + " for " + address + ".");
         });
         return ReplyById(address, found);
      }

      private Reply ReplyById(string address, long id)
      {
         (int status, string body) card = Me("GET", "/api/v1/me/messages/" + id + "/invitation", null, address, Password);
         Assert.AreEqual(200, card.status, card.body);
         Response source = Raw(RestPort, "GET", "/api/v1/me/messages/" + id + "/source", Auth(address, Password), null, null, null);
         Assert.AreEqual(200, source.Status, source.Body);
         return new Reply { Card = card.body, Source = source.Body, Text = DecodedParts(source.Body) };
      }

      // The decoded text of a message whose subject contains subjectPart and
      // whose parts contain phrase: several notices share a subject.
      private static string NoticeContaining(string address, string subjectPart, string phrase)
      {
         string found = null;
         RetryHelper.TryAction(TimeSpan.FromSeconds(30), () =>
         {
            (int status, string body) search = Me("GET", "/api/v1/me/search?q=" + Uri.EscapeDataString(subjectPart), null, address, Password);
            RetryableAssert.AreEqual(search.status, 200);
            foreach (Match item in Regex.Matches(search.body, "\"id\":(\\d+),\"uid\":[^{}]*?\"subject\":\"([^\"]*)\""))
            {
               if (!item.Groups[2].Value.Contains(subjectPart))
                  continue;
               Response source = Raw(RestPort, "GET", "/api/v1/me/messages/" + item.Groups[1].Value + "/source", Auth(address, Password), null, null, null);
               string text = DecodedParts(source.Body);
               if (text.Contains(phrase))
               {
                  found = text;
                  return;
               }
            }
            throw new Exception("No message about '" + subjectPart + "' saying '" + phrase + "' for " + address + ".");
         });
         return found;
      }

      // How many messages an account holds whose subject contains the text.
      private static int CountWithSubject(string address, string subjectPart)
      {
         (int status, string body) search = Me("GET", "/api/v1/me/search?q=" + Uri.EscapeDataString(subjectPart), null, address, Password);
         Assert.AreEqual(200, search.status, search.body);
         return Regex.Matches(search.body, "\"id\":(\\d+),\"uid\":[^{}]*?\"subject\":\"([^\"]*)\"").Cast<Match>()
            .Count(m => m.Groups[2].Value.Contains(subjectPart));
      }

      // The room's own INBOX, read over COM: the room cannot be signed into.
      private IMAPFolder RoomInbox()
      {
         return _application.Domains.ItemByName[_domain.Name].Accounts.ItemByAddress[Room].IMAPFolders.get_ItemByName("INBOX");
      }

      // The room's busy periods over one day, as a colleague reads them.
      private string FreeBusyOfRoom(int daysAhead)
      {
         (int status, string body) answer = Me("GET", "/api/v1/me/freebusy?address=" + Room + "&from=" + Slot(daysAhead, 0, 0) + "&to=" + Slot(daysAhead + 1, 0, 0), null, Colleague, Password);
         Assert.AreEqual(200, answer.status, answer.body);
         return answer.body;
      }

      // "start/end/kind" per period, in the order answered.
      private static List<string> Periods(string json)
      {
         return Regex.Matches(json, "\\{\"start\":\"([0-9TZ]+)\",\"end\":\"([0-9TZ]+)\",\"kind\":\"(busy|tentative)\"\\}").Cast<Match>()
            .Select(m => m.Groups[1].Value + "/" + m.Groups[2].Value + "/" + m.Groups[3].Value).ToList();
      }

      // A UTC instant this many days ahead of the bench's clock, as iCalendar
      // writes it.
      private static string Slot(int daysAhead, int hour, int minute)
      {
         return DateTime.UtcNow.Date.AddDays(daysAhead).AddHours(hour).AddMinutes(minute).ToString("yyyyMMdd'T'HHmmss'Z'", CultureInfo.InvariantCulture);
      }

      // A mail carrying a METHOD:REQUEST or METHOD:CANCEL as its whole body, as a
      // client sends one: to the room, with the organiser in the chair and the
      // room as a guest.
      private string Invitation(string method, string from, string subject, string summary, string uid, string dtstart, string dtend, int sequence, string organizer, params string[] extra)
      {
         var lines = new List<string>
         {
            "From: <" + from + ">", "To: <" + Room + ">", "Subject: " + subject, "MIME-Version: 1.0", "Content-Type: text/calendar; charset=utf-8; method=" + method, "",
            "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A client//EN", "METHOD:" + method, "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z",
            "DTSTART:" + dtstart, "DTEND:" + dtend, "SUMMARY:" + summary, "SEQUENCE:" + sequence, "ORGANIZER:mailto:" + organizer,
            "ATTENDEE;ROLE=CHAIR;PARTSTAT=ACCEPTED:mailto:" + organizer,
            "ATTENDEE;CUTYPE=ROOM;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:" + Room
         };
         lines.AddRange(extra);
         lines.AddRange(new[] { "END:VEVENT", "END:VCALENDAR", "" });
         return string.Join("\r\n", lines);
      }

      // A booking in the room's calendar as a CalDAV client writes it back: an
      // hour at ten, the organiser in the chair, the room with the PARTSTAT
      // given.
      private string Booking(string uid, int daysAhead, string summary, string organizer, string roomPartstat)
      {
         return string.Join("\r\n", new[]
         {
            "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//A client//EN", "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:20260101T000000Z",
            "DTSTART:" + Slot(daysAhead, 10, 0), "DTEND:" + Slot(daysAhead, 11, 0), "SUMMARY:" + summary, "SEQUENCE:0", "ORGANIZER:mailto:" + organizer,
            "ATTENDEE;ROLE=CHAIR;PARTSTAT=ACCEPTED:mailto:" + organizer,
            "ATTENDEE;CUTYPE=ROOM;PARTSTAT=" + roomPartstat + ":mailto:" + Room,
            "END:VEVENT", "END:VCALENDAR", ""
         });
      }

      // A PUT over CalDAV, as address, of the object the room filed under the
      // UID, in the room's calendar shared with them; answered 204.
      private static void PutIntoRoomCalendar(string address, long calendarId, string uid, string body)
      {
         Response put = Raw(WebServicesPort, "PUT", "/dav/calendars/" + address + "/shared-" + calendarId + "/" + uid + ".ics", Auth(address, Password), body,
            "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\n");
         Assert.AreEqual(204, put.Status, put.Body);
      }

      // One SMTP session of the test's own, to the room: signed in as login,
      // with the fixture's password, when one is given - the organiser's own
      // submission, which is what a room believes - and not signed in at all
      // otherwise.
      private void SmtpRaw(string from, string message, string login = null)
      {
         using (var client = new TcpClient("127.0.0.1", SmtpPort))
         using (NetworkStream stream = client.GetStream())
         using (var reader = new StreamReader(stream, Encoding.ASCII))
         using (var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\r\n" })
         {
            // UTF-8 without a byte-order mark: a summary outside ASCII goes as a
            // client writes it, and the commands are ASCII either way.
            ExpectSmtp(reader, "220");
            if (login != null)
            {
               writer.WriteLine("EHLO mail.rooms.test");
               ExpectSmtp(reader, "250");
               writer.WriteLine("AUTH PLAIN " + Convert.ToBase64String(Encoding.UTF8.GetBytes("\0" + login + "\0" + Password)));
               ExpectSmtp(reader, "235");
            }
            else
            {
               writer.WriteLine("HELO mail.rooms.test");
               ExpectSmtp(reader, "250");
            }
            writer.WriteLine("MAIL FROM:<" + from + ">");
            ExpectSmtp(reader, "250");
            writer.WriteLine("RCPT TO:<" + Room + ">");
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

      // Every base64 part of a message, decoded and joined: the text a person
      // reads and the calendar object, the latter still folded at 75 octets.
      private static string DecodedParts(string source)
      {
         var decoded = new StringBuilder();
         var block = new StringBuilder();
         bool base64Part = false;
         bool inBody = false;
         foreach (string line in source.Replace("\r\n", "\n").Split('\n'))
         {
            if (line.StartsWith("--"))
            {
               if (block.Length > 0)
               {
                  decoded.Append(Encoding.UTF8.GetString(Convert.FromBase64String(block.ToString()))).Append("\n");
                  block.Clear();
               }
               base64Part = false;
               inBody = false;
               continue;
            }
            if (!inBody)
            {
               if (line.StartsWith("Content-Transfer-Encoding: base64", StringComparison.OrdinalIgnoreCase))
                  base64Part = true;
               if (line.Length == 0)
                  inBody = true;
               continue;
            }
            if (base64Part)
               block.Append(line.Trim());
         }
         if (block.Length > 0)
            decoded.Append(Encoding.UTF8.GetString(Convert.FromBase64String(block.ToString())));
         return decoded.ToString().Replace("\r\n", "\n");
      }

      private static long MessageIdOf(string address, string password, string subjectPart)
      {
         long found = 0;
         RetryHelper.TryAction(TimeSpan.FromSeconds(30), () =>
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

      private static (int status, string body) Admin(string method, string path, string body)
      {
         Response response = Raw(RestPort, method, path, Auth("Administrator", AdminPassword), body, "application/json", null);
         return (response.Status, response.Body);
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

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public string HeaderText = "";
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
               return response;
            }
         }
      }
   }
}
