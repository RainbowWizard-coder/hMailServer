// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    The list manager (Common/Util/ListManager.h, docs/MailingLists.md): a
   ///    distribution list with its manager switched on, which a person joins and
   ///    leaves by mail and by page, and which sends each member a copy of their own
   ///    with the RFC 2369 and RFC 8058 headers filled in, keeps an archive, sends a
   ///    daily digest and counts its members' bounces.
   ///
   ///    Every test has its negative control, and two of them are the attacks a list
   ///    has to withstand: a request to join made in somebody else's name writes to
   ///    them once and subscribes nobody, and a delivery report forged to the bounce
   ///    address - which anybody can write to - counts for nothing unless it came
   ///    back from a copy the list really sent that member.
   ///
   ///    The pages the list's mail links to are served by the REST listener this
   ///    fixture starts (port 9966), and list_base_url is pointed at it, so the links
   ///    in the copies are links a test can follow. The members are accounts on this
   ///    server, whose copies are read over POP3; the outside member is the SMTP
   ///    simulator behind a route. The fixture is the Windows suite's: it is not
   ///    linked into the Linux one.
   /// </summary>
   [TestFixture]
   public class MailingLists : TestFixtureBase
   {
      private static int RestPort = 9966;
      private const string AdminPassword = "testar";
      private const string Password = "test";

      private const string ListAddress = "team@example.test";
      private const string Member1 = "member1@example.test";
      private const string Member2 = "member2@example.test";
      private const string Member3 = "member3@example.test";
      private const string Poster = "poster@example.test";
      private const string Joiner = "joiner@example.test";
      private const string Owner = "owner@example.test";

      private string baseUrlBefore_ = "";
      private long hardBefore_ = 1;
      private long softBefore_ = 5;
      private long windowBefore_ = 30;

      private static string BaseUrl
      {
         get { return "http://127.0.0.1:" + RestPort; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         try
         {
            _application.Reinitialize();

            (int status, string body) probe = Http("GET", "/api/v1/status");
            ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

            Dictionary<string, object> before = Settings();
            baseUrlBefore_ = MiniJson.Text(before, "list_base_url") ?? "";
            hardBefore_ = MiniJson.Number(before, "list_hard_bounces_to_unsubscribe");
            softBefore_ = MiniJson.Number(before, "list_soft_bounces_to_unsubscribe");
            windowBefore_ = MiniJson.Number(before, "list_bounce_window_days");

            PutSettings("{\"list_base_url\":\"" + BaseUrl + "\",\"list_hard_bounces_to_unsubscribe\":1," +
                        "\"list_soft_bounces_to_unsubscribe\":5,\"list_bounce_window_days\":30}");
         }
         catch
         {
            // NUnit runs no TearDown for a SetUp that failed: the listener and
            // the port are put back here instead.
            RestListener.Stop();
            _application.Reinitialize();
            throw;
         }
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            PutSettings("{\"list_base_url\":\"" + baseUrlBefore_ + "\",\"list_hard_bounces_to_unsubscribe\":" + hardBefore_ + "," +
                        "\"list_soft_bounces_to_unsubscribe\":" + softBefore_ + ",\"list_bounce_window_days\":" + windowBefore_ + "}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------ joining

      [Test]
      [Description("Policy confirm: a request by mail is answered with a confirmation carrying the token; a posting before the reply does not reach the address, and one after it does.")]
      public void ASubscriptionByMailIsConfirmedByTheReply()
      {
         AddAccounts(Member1, Joiner, Poster);
         MakeList(1, Member1);

         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test", "subscribe", "Please add me.");

         string confirmation = Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password);
         StringAssert.Contains("Confirm your subscription to " + ListAddress, confirmation);
         StringAssert.Contains("Auto-Submitted: auto-replied", confirmation);
         string token = Token(confirmation);
         StringAssert.Contains(BaseUrl + "/lists/confirm?t=" + token, confirmation);
         ClassicAssert.AreEqual("pending", MiniJson.Text(Member(Joiner), "state"));

         // The negative control: asked, not confirmed, and so not sent the list's mail.
         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Before", "Before the reply.");
         StringAssert.Contains("Subject: Before", Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));
         Pop3ClientSimulator.AssertMessageCount(Joiner, Password, 0);

         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test",
            "Re: Confirm your subscription to " + ListAddress + " [" + token + "]", "Yes.");
         WaitFor(() => MiniJson.Text(Member(Joiner), "state") == "member", "The reply did not confirm the subscription.");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "After", "After the reply.");
         StringAssert.Contains("Subject: After", Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("Policy closed: a request by mail is answered with a refusal that names the list's owner address and never the owner's own, carries no token, and subscribes nobody.")]
      public void AClosedListRefusesARequestToJoin()
      {
         // An owner whose address is not part of team-owner@'s, so that the
         // absence of the one says something beside the other.
         const string boss = "boss@example.test";
         AddAccounts(Member1, Joiner, boss);
         DistributionList list = MakeList(0, Member1);
         list.Owner = boss;
         list.Save();

         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test", "subscribe", "Please add me.");

         string refusal = Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password);
         StringAssert.Contains("does not take subscriptions", refusal);
         StringAssert.Contains("team-owner@example.test", refusal, "The refusal does not say where to write.");
         StringAssert.DoesNotContain(boss, refusal, "A stranger was given the owner's own address.");
         ClassicAssert.IsFalse(Regex.IsMatch(refusal, "[0-9a-f]{64}"), "A refusal carries no token to confirm with: " + refusal);

         ClassicAssert.IsNull(Member(Joiner), "A closed list took a member by mail.");
         ClassicAssert.IsNotNull(Member(Member1), "The administrator's member is still there.");

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("Policy moderated: after the address confirms, the owner is sent a link. A reply by mail approves nothing, even signed in; the page's button approves nothing without a session, or with another account's; the owner's own webmail session approves.")]
      public void AModeratedListSubscribesOnTheOwnersSignedInApproval()
      {
         AddAccounts(Member1, Joiner, Owner);
         DistributionList list = MakeList(2, Member1);
         list.Owner = Owner;
         list.Save();

         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test", "subscribe", "Please add me.");
         string token = Token(Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password));

         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test", "Re: confirm [" + token + "]", "Yes.");

         string request = Pop3ClientSimulator.AssertGetFirstMessageText(Owner, Password);
         StringAssert.Contains(Joiner + " asks to join " + ListAddress, request);
         string approval = Token(request);
         ClassicAssert.AreNotEqual(token, approval, "The token that confirmed is spent; the owner is sent a new one.");
         StringAssert.Contains(BaseUrl + "/lists/approve?t=" + approval, request);
         ClassicAssert.AreEqual("awaiting_owner", MiniJson.Text(Member(Joiner), "state"));

         // A reply by mail, from the owner's address and signed in: this
         // server's Received field says a session signed in, not as whom, so a
         // reply approves nothing.
         var smtp = new SmtpClientSimulator();
         string error;
         smtp.Send(false, Owner, Password, Owner, "team-request@example.test", "Re: approve [" + approval + "]", "Let them in.", out error);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ClassicAssert.AreEqual("awaiting_owner", MiniJson.Text(Member(Joiner), "state"), "A reply by mail approved the request.");

         // The page: GET asks and acts on nothing.
         (int status, string body) page = Public("GET", "/lists/approve?t=" + approval);
         ClassicAssert.AreEqual(200, page.status, page.body);
         StringAssert.Contains("Let them in", page.body);
         ClassicAssert.AreEqual("awaiting_owner", MiniJson.Text(Member(Joiner), "state"), "The GET acted.");

         // Its button, pressed by nobody signed in, and by an account that is
         // not the owner's.
         (int status, string headers, string body) nobody = SendRaw("POST", "/lists/approve", null, "t=" + approval, null);
         ClassicAssert.AreEqual(403, nobody.status, nobody.body);

         string memberSession = SignIn(Member1);
         (int status, string headers, string body) stranger = SendRaw("POST", "/lists/approve", null, "t=" + approval, "Cookie: hmailsession=" + memberSession + "\r\n");
         ClassicAssert.AreEqual(403, stranger.status, stranger.body);
         StringAssert.Contains("not the owner", stranger.body);
         ClassicAssert.AreEqual("awaiting_owner", MiniJson.Text(Member(Joiner), "state"), "Somebody other than the owner approved.");

         // The owner's own account, signed in to the webmail.
         string ownerSession = SignIn(Owner);
         (int status, string headers, string body) approved = SendRaw("POST", "/lists/approve", null, "t=" + approval, "Cookie: hmailsession=" + ownerSession + "\r\n");
         ClassicAssert.AreEqual(200, approved.status, approved.body);
         StringAssert.Contains("has joined", approved.body);
         ClassicAssert.AreEqual("member", MiniJson.Text(Member(Joiner), "state"));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The page: the form asks for an address and sends a confirmation; the confirmation link's GET only asks, its POST confirms, and a spent or unknown token answers 410.")]
      public void TheSubscribePageAndTheConfirmationLinkSubscribe()
      {
         AddAccounts(Member1, Joiner);
         MakeList(1, Member1);

         (int status, string body) form = Public("GET", "/lists/" + ListAddress + "/subscribe");
         ClassicAssert.AreEqual(200, form.status, form.body);
         StringAssert.Contains("Send me a confirmation", form.body);

         (int status, string body) asked = Public("POST", "/lists/" + ListAddress + "/subscribe", "address=joiner%40example.test");
         ClassicAssert.AreEqual(200, asked.status, asked.body);
         StringAssert.Contains("Check your mail", asked.body);

         string token = Token(Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password));

         (int status, string body) page = Public("GET", "/lists/confirm?t=" + token);
         ClassicAssert.AreEqual(200, page.status, page.body);
         StringAssert.Contains(">Confirm</button>", page.body);
         ClassicAssert.AreEqual("pending", MiniJson.Text(Member(Joiner), "state"), "The GET acted: a link checker would have subscribed the address.");

         (int status, string body) confirmed = Public("POST", "/lists/confirm", "t=" + token);
         ClassicAssert.AreEqual(200, confirmed.status, confirmed.body);
         StringAssert.Contains("You have joined", confirmed.body);
         ClassicAssert.AreEqual("member", MiniJson.Text(Member(Joiner), "state"));

         ClassicAssert.AreEqual(410, Public("POST", "/lists/confirm", "t=" + token).status, "A spent token confirmed again.");
         ClassicAssert.AreEqual(410, Public("POST", "/lists/confirm", "t=" + new string('0', 64)).status, "An unknown token was answered as a real one.");

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A request made in somebody else's name, twice, sends them one confirmation, subscribes them to nothing, and sends them none of the list's mail.")]
      public void ARequestForgedInSomebodyElsesNameWritesToThemOnceAndSubscribesNobody()
      {
         AddAccounts(Member1, Joiner, Poster);
         MakeList(1, Member1);

         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test", "subscribe", "Forged.");
         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test", "subscribe", "Forged again.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         Pop3ClientSimulator.AssertMessageCount(Joiner, Password, 1);
         ClassicAssert.AreEqual("pending", MiniJson.Text(Member(Joiner), "state"));

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "A posting", "Body.");
         StringAssert.Contains("Subject: A posting", Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         // Every copy is a message of its own: the queue is drained before the
         // count, or a copy wrongly queued for the request could still be on
         // its way when the count is read.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Joiner, Password, 1);

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ leaving

      [Test]
      [Description("Unsubscribe by mail answers the member's own address with a link and a token and changes nothing; the reply carrying the token leaves, and the member is told, in words that name them. A stranger writing to the same address is not answered, and a request in a member's name only writes to that member.")]
      public void AMemberLeavesByMailFromTheirOwnAddress()
      {
         AddAccounts(Member1, Member2, Joiner);
         MakeList(1, Member1, Member2);

         SmtpClientSimulator.StaticSend(Joiner, "team-unsubscribe@example.test", "unsubscribe", "Not a member.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ClassicAssert.IsNotNull(Member(Member1));
         ClassicAssert.IsNotNull(Member(Member2));
         Pop3ClientSimulator.AssertMessageCount(Joiner, Password, 0);

         // The request, which anybody who knew Member1's address could send: it
         // is answered to Member1, and takes nobody off.
         SmtpClientSimulator.StaticSend(Member1, "team-unsubscribe@example.test", "unsubscribe", "Take me off.");

         string asked = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Confirm that you want to leave " + ListAddress, asked);
         StringAssert.Contains(BaseUrl + "/lists/unsubscribe?t=", asked);
         string token = Token(asked);
         ClassicAssert.AreEqual("member", MiniJson.Text(Member(Member1), "state"), "The request alone took the member off.");

         // The reply carrying the token: the member leaves, and is told.
         SmtpClientSimulator.StaticSend(Member1, "team-unsubscribe@example.test", "Re: Confirm that you want to leave " + ListAddress + " [" + token + "]", "Yes.");

         string told = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("You have left " + ListAddress, told);
         StringAssert.Contains(Member1 + " has been taken off the list", told, "The notice does not name the member.");
         StringAssert.DoesNotContain("%MACRO_", told, "A macro was left in the notice.");

         ClassicAssert.IsNull(Member(Member1), "The member is still on the list.");
         ClassicAssert.IsNotNull(Member(Member2), "Another member was removed too.");

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The token link: GET shows the page and changes nothing; POST leaves; a wrong token changes nothing and says so.")]
      public void TheTokenLinkShowsAPageAndItsPostLeaves()
      {
         AddAccounts(Member1, Member2, Poster);
         MakeList(1, Member1, Member2);

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Hello", "Body.");
         string token = UnsubscribeToken(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         (int status, string body) wrong = Public("POST", "/lists/unsubscribe", "t=" + new string('a', 64) + "&action=leave");
         ClassicAssert.AreEqual(410, wrong.status, wrong.body);
         StringAssert.Contains("no longer works", wrong.body);

         // A request's token - which no copy of the list's mail carries - is
         // not a member's, and leaves nothing.
         AddAccounts(Joiner);
         SmtpClientSimulator.StaticSend(Joiner, "team-subscribe@example.test", "subscribe", "Please add me.");
         string request = Token(Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password));
         ClassicAssert.AreEqual(410, Public("POST", "/lists/unsubscribe", "t=" + request + "&action=leave").status);
         ClassicAssert.AreEqual("pending", MiniJson.Text(Member(Joiner), "state"), "A request's token took the request off the list.");

         (int status, string body) page = Public("GET", "/lists/unsubscribe?t=" + token);
         ClassicAssert.AreEqual(200, page.status, page.body);
         StringAssert.Contains("Leave the list", page.body);
         ClassicAssert.IsNotNull(Member(Member1), "The GET acted.");

         (int status, string body) left = Public("POST", "/lists/unsubscribe", "t=" + token + "&action=leave");
         ClassicAssert.AreEqual(200, left.status, left.body);
         StringAssert.Contains("You have left", left.body);
         ClassicAssert.IsNull(Member(Member1));
         ClassicAssert.IsNotNull(Member(Member2));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("RFC 8058: the one-click POST leaves with no session and no page; a POST with the token and any other body leaves nobody.")]
      public void TheOneClickPostLeavesWithoutASession()
      {
         AddAccounts(Member1, Poster);
         MakeList(1, Member1);

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Hello", "Body.");
         string token = UnsubscribeToken(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         (int status, string body) nothing = Public("POST", "/lists/unsubscribe?t=" + token, "");
         ClassicAssert.AreEqual(400, nothing.status, nothing.body);
         ClassicAssert.IsNotNull(Member(Member1), "A POST that asked for nothing unsubscribed the member.");

         (int status, string body) oneClick = Public("POST", "/lists/unsubscribe?t=" + token, "List-Unsubscribe=One-Click");
         ClassicAssert.AreEqual(200, oneClick.status, oneClick.body);
         ClassicAssert.IsNull(Member(Member1), "The one-click POST did not unsubscribe.");

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ what a copy carries

      [Test]
      [Description("A member's copy carries every List-* field with the right values, and RFC 8058's one-click over an https base URL only - over http the link, without the one-click; without a base URL it carries the mailto addresses alone.")]
      public void AMemberCopyCarriesEveryListHeader()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.Archive = 2;
         list.Save();

         // The header is written and never fetched here, so the base can be any
         // https address.
         const string httpsBase = "https://lists.example.test";
         PutSettings("{\"list_base_url\":\"" + httpsBase + "\"}");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Headers", "Body.");
         string copy = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         string token = UnsubscribeToken(copy);

         StringAssert.Contains("List-Id: <team.example.test>" + Environment.NewLine, copy);
         StringAssert.Contains("List-Post: <mailto:team@example.test>" + Environment.NewLine, copy);
         StringAssert.Contains("List-Help: <mailto:team-request@example.test?subject=help>" + Environment.NewLine, copy);
         StringAssert.Contains("List-Subscribe: <mailto:team-subscribe@example.test?subject=subscribe>, <" + httpsBase + "/lists/team@example.test/subscribe>" + Environment.NewLine, copy);
         StringAssert.Contains("List-Unsubscribe: <mailto:team-unsubscribe@example.test?subject=unsubscribe>, <" + httpsBase + "/lists/unsubscribe?t=" + token + ">" + Environment.NewLine, copy);
         StringAssert.Contains("List-Unsubscribe-Post: List-Unsubscribe=One-Click" + Environment.NewLine, copy);
         StringAssert.Contains("List-Owner: <mailto:team-owner@example.test>" + Environment.NewLine, copy);
         StringAssert.Contains("List-Archive: <" + httpsBase + "/lists/team@example.test/archive>" + Environment.NewLine, copy);
         StringAssert.Contains("Precedence: list" + Environment.NewLine, copy);
         ClassicAssert.IsTrue(Regex.IsMatch(copy, "X-hMailServer-List-Member: [0-9a-f]{32}"), copy);
         StringAssert.Contains("Return-Path: <team-bounces@example.test>", copy);

         // Over http: the link for a person, and no one-click - RFC 8058's is HTTPS.
         PutSettings("{\"list_base_url\":\"" + BaseUrl + "\"}");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Over http", "Body.");
         string overHttp = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("List-Unsubscribe: <mailto:team-unsubscribe@example.test?subject=unsubscribe>, <" + BaseUrl + "/lists/unsubscribe?t=" + token + ">" + Environment.NewLine, overHttp);
         StringAssert.DoesNotContain("List-Unsubscribe-Post", overHttp, "An http link was offered as a one-click.");

         // No page to point at: the mailto addresses alone, and no one-click.
         PutSettings("{\"list_base_url\":\"\"}");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Headers again", "Body.");
         string second = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("List-Unsubscribe: <mailto:team-unsubscribe@example.test?subject=unsubscribe>" + Environment.NewLine, second);
         StringAssert.DoesNotContain("List-Unsubscribe-Post", second);
         StringAssert.DoesNotContain("List-Archive", second);
         StringAssert.DoesNotContain("http://", second);

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The subject prefix is added once and not again on a reply; the footer goes on the text and the HTML part; a list with neither changes neither.")]
      public void ThePrefixIsAddedOnceAndTheFooterToBothParts()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.SubjectPrefix = "[team]";
         list.Footer = "Sent through the team list.";
         list.Save();

         DistributionList quiet = SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "quiet@example.test", new List<string> { Member1 });
         quiet.Managed = true;
         quiet.Save();

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Hello", "Plain words.");
         string first = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Subject: [team] Hello" + Environment.NewLine, first);
         StringAssert.Contains("Sent through the team list.", first);

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Re: [team] Hello", "A reply.");
         string reply = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Subject: Re: [team] Hello" + Environment.NewLine, reply);
         StringAssert.DoesNotContain("[team] [team]", reply);

         string raw =
            "From: " + Poster + "\r\n" +
            "To: " + ListAddress + "\r\n" +
            "Subject: Both parts\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/alternative; boundary=\"alt-boundary\"\r\n" +
            "\r\n" +
            "--alt-boundary\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "Plain words.\r\n" +
            "\r\n" +
            "--alt-boundary\r\n" +
            "Content-Type: text/html; charset=us-ascii\r\n" +
            "\r\n" +
            "<p>Html words.</p>\r\n" +
            "\r\n" +
            "--alt-boundary--\r\n";

         SmtpClientSimulator.StaticSendRaw(Poster, ListAddress, raw);
         string both = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         ClassicAssert.AreEqual(2, Count(both, "Sent through the team list."), "The footer is not on both parts: " + both);

         // The negative control: a managed list with neither.
         SmtpClientSimulator.StaticSend(Poster, "quiet@example.test", "Quiet", "No footer here.");
         string plain = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Subject: Quiet" + Environment.NewLine, plain);
         StringAssert.DoesNotContain("Sent through the team list.", plain);

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("Reply-To: with the list's setting on, exactly one, the list's, in place of the posting's own; with it off, the posting's own is kept as it was.")]
      public void ReplyToFollowsTheListsSetting()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.ReplyToList = true;
         list.Save();

         Func<string, string> posting = subject =>
            "From: " + Poster + "\r\n" +
            "To: " + ListAddress + "\r\n" +
            "Reply-To: someone@elsewhere.example\r\n" +
            "Subject: " + subject + "\r\n" +
            "\r\n" +
            "Body.\r\n";

         SmtpClientSimulator.StaticSendRaw(Poster, ListAddress, posting("To the list"));
         string toList = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Reply-To: team@example.test" + Environment.NewLine, toList);
         ClassicAssert.AreEqual(1, Regex.Matches(HeaderBlock(toList), "^Reply-To:", RegexOptions.Multiline).Count, "Not exactly one Reply-To: " + toList);
         StringAssert.DoesNotContain("someone@elsewhere.example", toList, "The posting's own Reply-To was kept beside the list's.");

         list.ReplyToList = false;
         list.Save();

         SmtpClientSimulator.StaticSendRaw(Poster, ListAddress, posting("To the sender"));
         string toSender = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Reply-To: someone@elsewhere.example" + Environment.NewLine, toSender, "The posting's own Reply-To was not kept.");
         StringAssert.DoesNotContain("Reply-To: team@example.test", toSender);

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("With VERP each copy's envelope sender encodes its member - an outside member's server is sent it in MAIL FROM; without, every copy is from the list's bounce address.")]
      public void VerpEncodesTheMemberInTheEnvelopeSender()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1, "outside@dummy-example.com");
         list.Verp = true;
         list.Save();

         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { ["outside@dummy-example.com"] = 250 });
            server.StartListen();
            BounceTestSupport.RouteTo(port);

            SmtpClientSimulator.StaticSend(Poster, ListAddress, "Verp", "Body.");

            server.WaitForCompletion();
            StringAssert.Contains("team-bounces+outside=dummy-example.com@example.test", server.MailFromCommand);
         }

         StringAssert.Contains("Return-Path: <team-bounces+member1=example.test@example.test>", Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         list.Verp = false;
         list.Save();
         SingletonProvider<TestSetup>.Instance.RemoveAllRoutes();

         var local = SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "local@example.test", new List<string> { Member1 });
         local.Managed = true;
         local.Save();

         SmtpClientSimulator.StaticSend(Poster, "local@example.test", "No verp", "Body.");
         StringAssert.Contains("Return-Path: <local-bounces@example.test>", Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ delivery modes

      [Test]
      [Description("Two postings: the member on each message gets both, the member on the digest gets one digest holding both as parts when the digest runs, and the member on none gets nothing and is still listed.")]
      public void TheDigestHoldsTheDaysPostingsAndTheOthersGetWhatTheyChose()
      {
         AddAccounts(Member1, Member2, Member3, Poster);
         DistributionList list = MakeList(1);
         list.Digest = true;
         list.Save();

         AddMember(list, Member1, 0);
         AddMember(list, Member2, 1);
         AddMember(list, Member3, 2);

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "First posting", "One.");
         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Second posting", "Two.");

         List<string> each = Messages(Member1, 2);
         ClassicAssert.IsTrue(each.Exists(m => m.Contains("Subject: First posting")));
         ClassicAssert.IsTrue(each.Exists(m => m.Contains("Subject: Second posting")));

         Pop3ClientSimulator.AssertMessageCount(Member2, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(Member3, Password, 0);

         // The daily task, driven as the quarantine digest's tests drive theirs.
         ClassicAssert.AreEqual(1, list.SendDigest(), "One member takes the digest.");

         string digest = Pop3ClientSimulator.AssertGetFirstMessageText(Member2, Password);
         StringAssert.Contains("multipart/digest", digest);
         StringAssert.Contains("Subject: First posting", digest);
         StringAssert.Contains("Subject: Second posting", digest);
         StringAssert.Contains("List-Id: <team.example.test>", digest);

         Pop3ClientSimulator.AssertMessageCount(Member3, Password, 0);
         ClassicAssert.AreEqual("none", MiniJson.Text(Member(Member3), "delivery"), "The paused member is still listed.");

         // Nothing new: no second digest, and no empty one.
         ClassicAssert.AreEqual(0, list.SendDigest());

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A member with an account here reads and changes their own delivery in the webmail, and leaves; a value that is not a delivery, or a list they are not on, changes nothing.")]
      public void AMemberChoosesTheirOwnDeliveryInTheWebmail()
      {
         AddAccounts(Member1, Member2);
         DistributionList list = MakeList(1, Member1);
         list.Digest = true;
         list.Save();

         (int status, string body) mine = As(Member1, "GET", "/api/v1/me/lists");
         ClassicAssert.AreEqual(200, mine.status, mine.body);
         StringAssert.Contains("\"address\":\"team@example.test\"", mine.body);
         StringAssert.Contains("\"delivery\":\"each\"", mine.body);

         (int status, string body) notMine = As(Member2, "GET", "/api/v1/me/lists");
         ClassicAssert.AreEqual(200, notMine.status, notMine.body);
         StringAssert.DoesNotContain("team@example.test", notMine.body);

         ClassicAssert.AreEqual(200, As(Member1, "PUT", "/api/v1/me/lists/" + ListAddress, "{\"delivery\":\"digest\"}").status);
         ClassicAssert.AreEqual("digest", MiniJson.Text(Member(Member1), "delivery"));

         ClassicAssert.AreEqual(400, As(Member1, "PUT", "/api/v1/me/lists/" + ListAddress, "{\"delivery\":\"weekly\"}").status);
         ClassicAssert.AreEqual(404, As(Member2, "PUT", "/api/v1/me/lists/" + ListAddress, "{\"delivery\":\"none\"}").status);
         ClassicAssert.AreEqual("digest", MiniJson.Text(Member(Member1), "delivery"));

         ClassicAssert.AreEqual(200, As(Member1, "DELETE", "/api/v1/me/lists/" + ListAddress).status);
         ClassicAssert.IsNull(Member(Member1));

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ the archive

      [Test]
      [Description("The archive: postings by month on the public page, one message shown as text with its HTML's script gone, the REST route, members-only refused without a token and shown with one, and the files deleted with the list.")]
      public void TheArchiveIsListedByMonthShownAsTextAndDeletedWithTheList()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.Archive = 2;
         list.Save();

         string raw =
            "From: " + Poster + "\r\n" +
            "To: " + ListAddress + "\r\n" +
            "Subject: Archived\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: text/html; charset=us-ascii\r\n" +
            "\r\n" +
            "<p>Hello <b>archive</b></p><script>alert('taken')</script>\r\n";

         SmtpClientSimulator.StaticSendRaw(Poster, ListAddress, raw);
         string token = UnsubscribeToken(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         string month = DateTime.UtcNow.ToString("yyyy-MM");

         (int status, string body) months = Public("GET", "/lists/" + ListAddress + "/archive");
         ClassicAssert.AreEqual(200, months.status, months.body);
         StringAssert.Contains(month, months.body);
         StringAssert.Contains("Archived", months.body);

         // The sender - the one address the archive shows - as "name at domain",
         // never as an address a harvester could take.
         StringAssert.Contains("poster at example.test", months.body);
         StringAssert.DoesNotContain(Poster, months.body, "The archive page shows the sender's address.");

         ClassicAssert.AreEqual(200, Public("GET", "/lists/" + ListAddress + "/archive?month=" + month).status);

         (int status, string body) listed = Http("GET", "/api/v1/lists/" + ListAddress + "/archive");
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         List<object> messages = MiniJson.List(MiniJson.Object(listed.body), "messages");
         ClassicAssert.AreEqual(1, messages.Count, listed.body);
         long id = MiniJson.Number((Dictionary<string, object>) messages[0], "id");

         (int status, string body) shown = Public("GET", "/lists/" + ListAddress + "/archive?id=" + id);
         ClassicAssert.AreEqual(200, shown.status, shown.body);
         StringAssert.Contains("Hello", shown.body);
         StringAssert.DoesNotContain("<script", shown.body);
         StringAssert.DoesNotContain("alert(", shown.body);
         StringAssert.Contains("poster at example.test", shown.body);
         StringAssert.DoesNotContain(Poster, shown.body, "The message page shows the sender's address.");
         StringAssert.DoesNotContain("is not shown here", shown.body, "A short message was cut.");

         (int status, string body) text = Http("GET", "/api/v1/lists/" + ListAddress + "/archive/" + id);
         ClassicAssert.AreEqual(200, text.status, text.body);
         StringAssert.Contains("Hello", MiniJson.Text(MiniJson.Object(text.body), "text"));
         StringAssert.DoesNotContain("<script", text.body);

         // Switched off: the page goes, and what it held stays until its days
         // run out - switched on again, the message is still there.
         list.Archive = 0;
         list.Save();
         ClassicAssert.AreEqual(404, Public("GET", "/lists/" + ListAddress + "/archive").status);

         // For members only: refused without a member's token, shown with one -
         // and with the token of a member of another list, refused.
         list.Archive = 1;
         list.Save();

         ClassicAssert.AreEqual(403, Public("GET", "/lists/" + ListAddress + "/archive").status);
         (int status, string body) back = Public("GET", "/lists/" + ListAddress + "/archive?t=" + token);
         ClassicAssert.AreEqual(200, back.status, back.body);
         StringAssert.Contains("Archived", back.body, "The archive did not keep its message while it was off.");

         DistributionList other = SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "other@example.test", new List<string> { Member1 });
         other.Managed = true;
         other.Archive = 1;
         other.Save();

         SmtpClientSimulator.StaticSend(Poster, "other@example.test", "Other list", "Body.");
         string otherToken = UnsubscribeToken(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));
         ClassicAssert.AreNotEqual(token, otherToken);
         ClassicAssert.AreEqual(403, Public("GET", "/lists/" + ListAddress + "/archive?t=" + otherToken).status, "A member of another list read this one's members-only archive.");
         ClassicAssert.AreEqual(200, Public("GET", "/lists/other@example.test/archive?t=" + otherToken).status);

         string directory = Path.Combine(_settings.Directories.DataDirectory, "Lists", list.ID.ToString());
         ClassicAssert.IsTrue(Directory.Exists(directory) && Directory.GetFiles(directory).Length == 1, "The archived copy is not under " + directory);

         list.Delete();
         ClassicAssert.IsFalse(Directory.Exists(directory), "The archive outlived its list.");

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ bounces

      [Test]
      [Description("A hard delivery report to a member's VERP address stops their delivery, records why, and tells the owner once; a report forged without the member's tag counts for nothing.")]
      public void AHardBounceStopsTheMemberAndTellsTheOwnerOnce()
      {
         AddAccounts(Member1, Member2, Poster, Owner);
         DistributionList list = MakeList(1, Member1, Member2);
         list.Verp = true;
         list.Owner = Owner;
         list.Save();

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Bounce me", "Body.");
         string copy1 = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         string copy2 = Pop3ClientSimulator.AssertGetFirstMessageText(Member2, Password);

         // The forgery: a well-formed report about member2, to member2's VERP
         // address, whose returned header is not a copy the list sent.
         SmtpClientSimulator.StaticSendRaw("", "team-bounces+member2=example.test@example.test",
            Dsn(Member2, "5.1.1", "From: somebody@elsewhere.example\r\nSubject: Not a copy\r\n"));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ClassicAssert.AreEqual(0, MiniJson.Number(Member(Member2), "hard_bounces"), "A forged report was counted.");
         ClassicAssert.AreEqual("each", MiniJson.Text(Member(Member2), "delivery"));
         ClassicAssert.IsNotNull(copy2);

         SmtpClientSimulator.StaticSendRaw("", "team-bounces+member1=example.test@example.test", Dsn(Member1, "5.1.1", HeaderBlock(copy1)));
         WaitFor(() => MiniJson.Number(Member(Member1), "hard_bounces") == 1, "The hard bounce was not counted.");

         Dictionary<string, object> stopped = Member(Member1);
         ClassicAssert.AreEqual("none", MiniJson.Text(stopped, "delivery"));
         StringAssert.Contains("hard bounce", MiniJson.Text(stopped, "reason"));
         ClassicAssert.AreEqual("member", MiniJson.Text(stopped, "state"), "A bounce unsubscribed the member.");

         // The same report again: counted, and the owner is not told twice. The
         // queue is drained before the count, which a second notice still on its
         // way would otherwise slip past.
         SmtpClientSimulator.StaticSendRaw("", "team-bounces+member1=example.test@example.test", Dsn(Member1, "5.1.1", HeaderBlock(copy1)));
         WaitFor(() => MiniJson.Number(Member(Member1), "hard_bounces") == 2, "The second hard bounce was not counted.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Owner, Password, 1);
         StringAssert.Contains("has stopped mailing " + Member1, Pop3ClientSimulator.AssertGetFirstMessageText(Owner, Password));

         // And a stopped member is sent nothing.
         SmtpClientSimulator.StaticSend(Poster, ListAddress, "After the bounce", "Body.");
         StringAssert.Contains("Subject: After the bounce", Pop3ClientSimulator.AssertGetFirstMessageText(Member2, Password));
         Pop3ClientSimulator.AssertMessageCount(Member1, Password, 0);

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("Soft bounces are counted: a delay warning is not one; four leave the member receiving, the fifth within the window stops them.")]
      public void FiveSoftBouncesWithinTheWindowStopTheMember()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.Verp = true;
         list.Save();

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Soft", "Body.");
         string headers = HeaderBlock(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         // The "still trying" warning many servers send after a few hours: its
         // Action is delayed, the message may yet arrive, and it counts for nothing.
         SmtpClientSimulator.StaticSendRaw("", "team-bounces+member1=example.test@example.test", Dsn(Member1, "4.2.2", headers, "delayed"));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         WaitFor(() => LogHandler.DefaultLogContains("is a delay warning"), "The delay warning was not logged as one.");
         ClassicAssert.AreEqual(0, MiniJson.Number(Member(Member1), "soft_bounces"), "A delay warning was counted as a bounce.");

         for (int i = 1; i <= 4; i++)
         {
            SmtpClientSimulator.StaticSendRaw("", "team-bounces+member1=example.test@example.test", Dsn(Member1, "4.2.2", headers));
            int expected = i;
            WaitFor(() => MiniJson.Number(Member(Member1), "soft_bounces") == expected, "Soft bounce " + i + " was not counted.");
         }

         ClassicAssert.AreEqual("each", MiniJson.Text(Member(Member1), "delivery"), "Four soft bounces stopped the member.");

         SmtpClientSimulator.StaticSendRaw("", "team-bounces+member1=example.test@example.test", Dsn(Member1, "4.2.2", headers));
         WaitFor(() => MiniJson.Text(Member(Member1), "delivery") == "none", "The fifth soft bounce did not stop the member.");
         StringAssert.Contains("soft bounce", MiniJson.Text(Member(Member1), "reason"));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A bounce about somebody who is not a member is logged and dropped, and nobody's counts change.")]
      public void ABounceForANonMemberIsLoggedAndDropped()
      {
         AddAccounts(Member1);
         MakeList(1, Member1);

         SmtpClientSimulator.StaticSendRaw("", "team-bounces+stranger=example.test@example.test",
            Dsn("stranger@example.test", "5.1.1", "Subject: whatever\r\n"));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         ClassicAssert.AreEqual(0, MiniJson.Number(Member(Member1), "hard_bounces"));
         WaitFor(() => LogHandler.DefaultLogContains("who is not a member"), "The dropped bounce was not logged.");

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The owner's reset restores each message and clears the counts; resetting somebody who is not a member is a 404.")]
      public void ResetBouncesRestoresEachMessage()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.Verp = true;
         list.Save();

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Reset", "Body.");
         string headers = HeaderBlock(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         SmtpClientSimulator.StaticSendRaw("", "team-bounces+member1=example.test@example.test", Dsn(Member1, "5.1.1", headers));
         WaitFor(() => MiniJson.Text(Member(Member1), "delivery") == "none", "The hard bounce did not stop the member.");

         ClassicAssert.AreEqual(404, Http("POST", "/api/v1/lists/" + ListAddress + "/members/nobody@example.test/reset-bounces").status);

         (int status, string body) reset = Http("POST", "/api/v1/lists/" + ListAddress + "/members/" + Member1 + "/reset-bounces");
         ClassicAssert.AreEqual(200, reset.status, reset.body);

         Dictionary<string, object> member = Member(Member1);
         ClassicAssert.AreEqual("each", MiniJson.Text(member, "delivery"));
         ClassicAssert.AreEqual(0, MiniJson.Number(member, "hard_bounces"));
         ClassicAssert.AreEqual("", MiniJson.Text(member, "reason") ?? "");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Back again", "Body.");
         StringAssert.Contains("Subject: Back again", Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The owner's reset lifts a pause the bounces made and never one the member chose: a member who paused their own delivery, and then bounced, is reset to no bounces and stays paused.")]
      public void ResetBouncesLeavesAMembersOwnPauseAlone()
      {
         AddAccounts(Member1, Member2, Poster);
         DistributionList list = MakeList(1, Member1, Member2);
         list.Verp = true;
         list.Save();

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Before the pause", "Body.");
         Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         string headers = HeaderBlock(Pop3ClientSimulator.AssertGetFirstMessageText(Member2, Password));

         // Member2 pauses, by their own choice.
         ClassicAssert.AreEqual(200, As(Member2, "PUT", "/api/v1/me/lists/" + ListAddress, "{\"delivery\":\"none\"}").status);

         // A late bounce of the copy Member2 was sent before: counted, and the
         // pause stays the member's own.
         SmtpClientSimulator.StaticSendRaw("", "team-bounces+member2=example.test@example.test", Dsn(Member2, "5.1.1", headers));
         WaitFor(() => MiniJson.Number(Member(Member2), "hard_bounces") == 1, "The late bounce was not counted.");
         ClassicAssert.AreEqual("", MiniJson.Text(Member(Member2), "reason") ?? "", "A bounce took over a pause the member chose.");

         (int status, string body) reset = Http("POST", "/api/v1/lists/" + ListAddress + "/members/" + Member2 + "/reset-bounces");
         ClassicAssert.AreEqual(200, reset.status, reset.body);

         Dictionary<string, object> member = Member(Member2);
         ClassicAssert.AreEqual(0, MiniJson.Number(member, "hard_bounces"));
         ClassicAssert.AreEqual("none", MiniJson.Text(member, "delivery"), "The reset sent a member who paused the list's mail again.");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "After the reset", "Body.");
         StringAssert.Contains("Subject: After the reset", Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Member2, Password, 0);

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ the surfaces

      [Test]
      [Description("REST: the list's manager fields round-trip through PUT and the listing, and a digest hour of 25, a prefix of 65 characters, a policy of 3 and an owner that is the list's own address are refused, changing nothing.")]
      public void TheManagersFieldsRoundTripOverRestAndOutOfRangeValuesAreRefused()
      {
         AddAccounts(Member1, Owner);
         MakeList(0, Member1);

         (int status, string body) put = Http("PUT", "/api/v1/lists/" + ListAddress,
            "{\"managed\":true,\"subscribe_policy\":2,\"archive\":2,\"archive_days\":30,\"digest\":true,\"digest_hour\":8," +
            "\"reply_to\":1,\"subject_prefix\":\"[team]\",\"footer\":\"The footer.\",\"verp\":true,\"owner\":\"" + Owner + "\"}");
         ClassicAssert.AreEqual(200, put.status, put.body);

         Dictionary<string, object> entry = ListEntry();
         ClassicAssert.IsTrue(MiniJson.Flag(entry, "managed"));
         ClassicAssert.AreEqual(2, MiniJson.Number(entry, "subscribe_policy"));
         ClassicAssert.AreEqual(2, MiniJson.Number(entry, "archive"));
         ClassicAssert.AreEqual(30, MiniJson.Number(entry, "archive_days"));
         ClassicAssert.IsTrue(MiniJson.Flag(entry, "digest"));
         ClassicAssert.AreEqual(8, MiniJson.Number(entry, "digest_hour"));
         ClassicAssert.AreEqual(1, MiniJson.Number(entry, "reply_to"));
         ClassicAssert.AreEqual("[team]", MiniJson.Text(entry, "subject_prefix"));
         ClassicAssert.AreEqual("The footer.", MiniJson.Text(entry, "footer"));
         ClassicAssert.IsTrue(MiniJson.Flag(entry, "verp"));
         ClassicAssert.AreEqual(Owner, MiniJson.Text(entry, "owner"));

         ClassicAssert.AreEqual(400, Http("PUT", "/api/v1/lists/" + ListAddress, "{\"digest_hour\":25}").status);
         ClassicAssert.AreEqual(400, Http("PUT", "/api/v1/lists/" + ListAddress, "{\"subject_prefix\":\"" + new string('x', 65) + "\"}").status);
         ClassicAssert.AreEqual(400, Http("PUT", "/api/v1/lists/" + ListAddress, "{\"subscribe_policy\":3}").status);
         ClassicAssert.AreEqual(400, Http("PUT", "/api/v1/lists/" + ListAddress, "{\"owner\":\"team-owner@example.test\"}").status);
         ClassicAssert.AreEqual(400, Http("PUT", "/api/v1/lists/" + ListAddress, "{\"owner\":\"alice@example.test, bob@example.test\"}").status, "Two owners were taken for one.");

         entry = ListEntry();
         ClassicAssert.AreEqual(8, MiniJson.Number(entry, "digest_hour"));
         ClassicAssert.AreEqual("[team]", MiniJson.Text(entry, "subject_prefix"));
         ClassicAssert.AreEqual(2, MiniJson.Number(entry, "subscribe_policy"));
         ClassicAssert.AreEqual(Owner, MiniJson.Text(entry, "owner"));

         // team-lead@ is none of team's own addresses, and may own it.
         ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/lists/" + ListAddress, "{\"owner\":\"team-lead@example.test\"}").status);
         ClassicAssert.AreEqual("team-lead@example.test", MiniJson.Text(ListEntry(), "owner"));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("COM: the list's manager properties and a member's delivery round-trip, and a digest hour of 25, a prefix of 65 characters, a policy of 3 and a delivery of 3 are refused where they are written.")]
      public void TheManagersPropertiesRoundTripOverComAndOutOfRangeValuesAreRefused()
      {
         AddAccounts(Member1, Owner);
         DistributionList list = MakeList(1, Member1);

         list.SubscribePolicy = 2;
         list.Archive = 1;
         list.ArchiveDays = 10;
         list.Digest = true;
         list.DigestHour = 22;
         list.ReplyToList = true;
         list.SubjectPrefix = "[t]";
         list.Footer = "F";
         list.Verp = true;
         list.Owner = Owner;
         list.Save();

         DistributionList read = _domain.DistributionLists.get_ItemByAddress(ListAddress);
         ClassicAssert.IsTrue(read.Managed);
         ClassicAssert.AreEqual(2, read.SubscribePolicy);
         ClassicAssert.AreEqual(1, read.Archive);
         ClassicAssert.AreEqual(10, read.ArchiveDays);
         ClassicAssert.IsTrue(read.Digest);
         ClassicAssert.AreEqual(22, read.DigestHour);
         ClassicAssert.IsTrue(read.ReplyToList);
         ClassicAssert.AreEqual("[t]", read.SubjectPrefix);
         ClassicAssert.AreEqual("F", read.Footer);
         ClassicAssert.IsTrue(read.Verp);
         ClassicAssert.AreEqual(Owner, read.Owner);

         Assert.Throws<COMException>(() => read.DigestHour = 25);
         Assert.Throws<COMException>(() => read.SubjectPrefix = new string('x', 65));
         Assert.Throws<COMException>(() => read.SubscribePolicy = 3);

         DistributionListRecipient recipient = read.Recipients[0];
         recipient.Delivery = 1;
         recipient.Save();
         ClassicAssert.AreEqual(1, _domain.DistributionLists.get_ItemByAddress(ListAddress).Recipients[0].Delivery);
         ClassicAssert.AreEqual(1, recipient.Confirmed);
         ClassicAssert.AreEqual(0, recipient.HardBounces);
         ClassicAssert.AreEqual(0, recipient.SoftBounces);
         Assert.Throws<COMException>(() => recipient.Delivery = 3);

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ the membership query

      [Test]
      [Description("An account the list's membership query answers is a member already: a request in its name sends nothing and makes no row; and a request made for an address before the query answered it does not keep it from the list's mail once it does - where a request for an address outside the query still sends it nothing.")]
      public void ARequestNeverKeepsAnAccountTheQueryAnswersFromTheListsMail()
      {
         AddAccounts(Member1, Poster, Joiner, "qone@example.test");
         DistributionList list = MakeList(1, Member1);
         list.Membership = "pattern:q*@example.test";
         list.Save();

         // Asked in the name of an account the query answers: nothing is sent,
         // and it stays the query's.
         ClassicAssert.AreEqual(200, Public("POST", "/lists/" + ListAddress + "/subscribe", "address=qone%40example.test").status);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount("qone@example.test", Password, 0);
         ClassicAssert.AreEqual("query", MiniJson.Text(Member("qone@example.test"), "source"), "A request made a row for an account the query answers.");

         // A request for an address before the query answered it - its account
         // made afterwards - and one for an address the query never answers.
         Public("POST", "/lists/" + ListAddress + "/subscribe", "address=qlate%40example.test");
         ClassicAssert.AreEqual("pending", MiniJson.Text(Member("qlate@example.test"), "state"));
         AddAccounts("qlate@example.test");

         Public("POST", "/lists/" + ListAddress + "/subscribe", "address=joiner%40example.test");
         Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password);
         ClassicAssert.AreEqual("pending", MiniJson.Text(Member(Joiner), "state"));

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "To everybody", "Body.");

         StringAssert.Contains("Subject: To everybody", Pop3ClientSimulator.AssertGetFirstMessageText("qlate@example.test", Password),
            "A request waiting in its name kept an account the query answers from the list's mail.");
         StringAssert.Contains("Subject: To everybody", Pop3ClientSimulator.AssertGetFirstMessageText("qone@example.test", Password));

         // The negative control: a request for an address outside the query is
         // not a member, and is sent none of the list's mail.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Joiner, Password, 0);

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("An account the query answers leaves by the one-click in its copy, and by the mailto answered with a link and a token; a row then keeps it out of the list's mail, while the query's other accounts are still sent it.")]
      public void AnAccountTheQueryAnswersLeavesByTheOneClickAndByMail()
      {
         AddAccounts(Poster, "qone@example.test", "qtwo@example.test", "qthree@example.test");
         DistributionList list = MakeList(1);
         list.Membership = "pattern:q*@example.test";
         list.Save();

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "First", "Body.");
         string copy = Pop3ClientSimulator.AssertGetFirstMessageText("qone@example.test", Password);
         Pop3ClientSimulator.AssertGetFirstMessageText("qtwo@example.test", Password);
         Pop3ClientSimulator.AssertGetFirstMessageText("qthree@example.test", Password);

         // Its way out by link: the address and the list, and the HMAC that
         // proves this server wrote them. No member tag - it has no row to count
         // bounces on.
         string link = QueryLink(copy);
         StringAssert.Contains("&m=qone@example.test&l=" + list.ID, link);
         ClassicAssert.IsFalse(Regex.IsMatch(copy, "X-hMailServer-List-Member:"), "A query member's copy carries a member tag.");

         // A link with another address, or another list, leaves nobody.
         ClassicAssert.AreEqual(410, Public("POST", link.Replace("m=qone@", "m=qtwo@"), "List-Unsubscribe=One-Click").status, "A link changed to another address worked.");

         (int status, string body) oneClick = Public("POST", link, "List-Unsubscribe=One-Click");
         ClassicAssert.AreEqual(200, oneClick.status, oneClick.body);
         ClassicAssert.AreEqual("left", MiniJson.Text(Member("qone@example.test"), "state"));

         // By mail: the request is answered with a link and a token, and changes nothing.
         SmtpClientSimulator.StaticSend("qtwo@example.test", "team-unsubscribe@example.test", "unsubscribe", "Take me off.");
         string asked = Pop3ClientSimulator.AssertGetFirstMessageText("qtwo@example.test", Password);
         StringAssert.Contains("Confirm that you want to leave " + ListAddress, asked);
         ClassicAssert.AreEqual("query", MiniJson.Text(Member("qtwo@example.test"), "source"), "The request alone took the address off.");

         SmtpClientSimulator.StaticSend("qtwo@example.test", "team-unsubscribe@example.test", "Re: leave [" + Token(asked) + "]", "Yes.");
         string told = Pop3ClientSimulator.AssertGetFirstMessageText("qtwo@example.test", Password);
         StringAssert.Contains("qtwo@example.test has been taken off the list", told);
         ClassicAssert.AreEqual("left", MiniJson.Text(Member("qtwo@example.test"), "state"));

         // Both are out; the query's third account, the negative control, is not.
         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Second", "Body.");
         StringAssert.Contains("Subject: Second", Pop3ClientSimulator.AssertGetFirstMessageText("qthree@example.test", Password));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount("qone@example.test", Password, 0);
         Pop3ClientSimulator.AssertMessageCount("qtwo@example.test", Password, 0);

         // And the export: an address that left is not one of the list's members.
         StringAssert.DoesNotContain("qone@example.test", Http("GET", "/api/v1/domains/example.test/lists").body);

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ what cannot be read

      [Test]
      [Description("A posting the list manager cannot read - here one that has neither a From nor a Received field - is kept in the queue and tried again, not lost: fixed, the same posting reaches the members. With no retries left, its sender is told with a class 4 status.")]
      public void APostingThatCannotBeReadIsTriedAgainAndNotLost()
      {
         AddAccounts(Member1, Poster);
         MakeList(1, Member1);

         _settings.SMTPNoOfTries = 3;

         // Made through COM, which writes no Received field; and without a From.
         var held = SingletonProvider<TestSetup>.Instance.CreateMessage();
         held.FromAddress = Poster;
         held.AddRecipient("", ListAddress);
         held.Subject = "Held posting";
         held.Body = "Tried again.";
         held.Save();
         long heldId = held.ID;
         string heldFile = held.Filename;

         WaitFor(() => File.Exists(LogHandler.GetErrorLogFileName()) && LogHandler.ReadErrorLog().Contains("It is kept in the queue and tried again"),
            "The posting that could not be read was not reported.");

         // Not delivered, not lost: still queued.
         ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(Member1, Password), "The unreadable posting was distributed.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(1, false);

         // Fixed, and tried again at once: the same posting reaches the member.
         string content = File.ReadAllText(heldFile);
         File.WriteAllText(heldFile, "From: " + Poster + "\r\n" + content);

         for (int attempt = 0; attempt < 5; attempt++)
         {
            var queue = _application.GlobalObjects.DeliveryQueue;
            queue.ResetDeliveryTime(heldId);
            queue.StartDelivery();

            if (new Pop3ClientSimulator().GetMessageCount(Member1, Password) > 0)
               break;

            Thread.Sleep(1000);
         }

         StringAssert.Contains("Held posting", Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password),
            "The held posting was not distributed when it could be read.");

         // No retries left: the sender is told, with a class 4 status - this
         // server failed, not the list.
         _settings.SMTPNoOfTries = 0;

         var lost = SingletonProvider<TestSetup>.Instance.CreateMessage();
         lost.FromAddress = Poster;
         lost.AddRecipient("", ListAddress);
         lost.Subject = "No retries";
         lost.Body = "Reported.";
         lost.Save();

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(Poster, Password);
         StringAssert.Contains("4.3.0", report);
         StringAssert.Contains(ListAddress, report);

         CustomAsserts.AssertReportedError("HM7250");
      }

      [Test]
      [Description("An archived message of a megabyte and more of HTML, tag after tag, is shown on the public page as text within seconds - the text of its HTML is read in one pass - cut, with a sentence saying so.")]
      public void AHugeTagHeavyMessageIsShownQuicklyAndCut()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.Archive = 2;
         list.Save();

         var html = new StringBuilder();
         while (html.Length < 3 * 1024 * 1024)
            html.Append("<b>x</b><i>&amp;</i><span>y</span>\r\n");

         string raw =
            "From: " + Poster + "\r\n" +
            "To: " + ListAddress + "\r\n" +
            "Subject: Heavy\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: text/html; charset=us-ascii\r\n" +
            "\r\n" +
            html + "\r\n";

         SmtpClientSimulator.StaticSendRaw(Poster, ListAddress, raw);
         Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);

         (int status, string body) listed = Http("GET", "/api/v1/lists/" + ListAddress + "/archive");
         long id = MiniJson.Number((Dictionary<string, object>) MiniJson.List(MiniJson.Object(listed.body), "messages")[0], "id");

         Stopwatch watch = Stopwatch.StartNew();
         (int status, string body) shown = Public("GET", "/lists/" + ListAddress + "/archive?id=" + id);
         watch.Stop();

         ClassicAssert.AreEqual(200, shown.status);
         ClassicAssert.Less(watch.Elapsed.TotalSeconds, 20, "The page took " + watch.Elapsed.TotalSeconds + " seconds.");
         StringAssert.Contains("is not shown here", shown.body, "A message larger than the page shows was not said to be cut.");
         StringAssert.DoesNotContain("<b>", shown.body);

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ the digest

      [Test]
      [Description("A digest switched on starts from the newest message the archive holds: a message archived before it, which its digest member was sent as it went, is not in the first digest; one after it is.")]
      public void ADigestSwitchedOnStartsFromTheNewestMessage()
      {
         AddAccounts(Member2, Poster);
         DistributionList list = MakeList(1);
         list.Archive = 1;
         list.Save();
         AddMember(list, Member2, 1);

         // The digest is off: the member who chose it is sent each message.
         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Before the digest", "Old.");
         StringAssert.Contains("Subject: Before the digest", Pop3ClientSimulator.AssertGetFirstMessageText(Member2, Password));

         list = _domain.DistributionLists.get_ItemByAddress(ListAddress);
         list.Digest = true;
         list.Save();

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "After the digest", "New.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Member2, Password, 0);

         ClassicAssert.AreEqual(1, list.SendDigest());
         string digest = Pop3ClientSimulator.AssertGetFirstMessageText(Member2, Password);
         StringAssert.Contains("Subject: After the digest", digest);
         StringAssert.DoesNotContain("Before the digest", digest, "The first digest resent what the member had already been sent.");

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ signing

      [Test]
      [Description("A member's copy, from a list whose domain signs, carries exactly one DKIM-Signature, made after the list's fields and covering List-Unsubscribe and List-Unsubscribe-Post, that verifies with the domain's key; one byte changed in the body and it no longer verifies.")]
      public void ACopyIsSignedAsTheListsDomainOverItsListFields()
      {
         string sslExamples = Paths.Combine(TestContext.CurrentContext.TestDirectory, "..\\..\\..\\..\\SSL examples");
         string privateKeyFile = Paths.Combine(sslExamples, "example.key");
         string certificateFile = Paths.Combine(sslExamples, "example.crt");
         ClassicAssert.IsTrue(File.Exists(privateKeyFile) && File.Exists(certificateFile), "The suite's example key pair is missing.");

         _domain.DKIMPrivateKeyFile = privateKeyFile;
         _domain.DKIMSelector = "lists";
         _domain.DKIMSignEnabled = true;
         _domain.Save();

         AddAccounts(Poster);
         DistributionList list = MakeList(1, "outside@dummy-example.com");
         list.SubjectPrefix = "[team]";
         list.Save();

         // One-click wants an https base; the header is written, never fetched.
         PutSettings("{\"list_base_url\":\"https://lists.example.test\"}");

         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         using (var certificate = new X509Certificate2(certificateFile))
         using (RSA publicKey = certificate.GetRSAPublicKey())
         {
            server.AddRecipientResult(new Dictionary<string, int> { ["outside@dummy-example.com"] = 250 });
            server.StartListen();
            BounceTestSupport.RouteTo(port);

            SmtpClientSimulator.StaticSend(Poster, ListAddress, "Signed", "A body the list signs.");

            server.WaitForCompletion();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            string received = server.MessageData;
            string header = HeaderBlock(received);

            ClassicAssert.AreEqual(1, Regex.Matches(header, "^DKIM-Signature:", RegexOptions.Multiline | RegexOptions.IgnoreCase).Count,
               "The copy must carry exactly one DKIM-Signature, the list's: " + header);
            StringAssert.Contains("List-Unsubscribe-Post:", header);

            Dictionary<string, string> tags = DkimTags(header);
            ClassicAssert.AreEqual("example.test", tags["d"]);
            StringAssert.Contains("list-unsubscribe-post", tags["h"].ToLowerInvariant(), "The signature does not cover List-Unsubscribe-Post: " + tags["h"]);
            StringAssert.Contains("list-unsubscribe", tags["h"].ToLowerInvariant().Replace("list-unsubscribe-post", ""));

            ClassicAssert.IsTrue(DkimSignatureVerifies(received, publicKey, out string why), "The copy's DKIM signature does not verify: " + why);

            // The negative control: the verifier checks something.
            int bodyAt = received.IndexOf("\r\n\r\n", StringComparison.Ordinal) + 4;
            char changed = received[bodyAt] == 'M' ? 'N' : 'M';
            string tampered = received.Substring(0, bodyAt) + changed + received.Substring(bodyAt + 1);
            ClassicAssert.IsFalse(DkimSignatureVerifies(tampered, publicKey, out string _), "A changed body still verified.");
         }

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ what the list answers

      [Test]
      [Description("One requester gets twenty answers an hour from a list and no more: the twenty-first request, from the same address, sends nothing - while the same request to another list is answered.")]
      public void TheTwentyFirstRequestFromOneRequesterInAnHourIsNotAnswered()
      {
         AddAccounts(Member1, Joiner);
         MakeList(1, Member1);

         // Twenty requests from this address, each for an address with no
         // mailbox here: each is counted, and none is delivered.
         for (int i = 0; i < 20; i++)
            ClassicAssert.AreEqual(200, Public("POST", "/lists/" + ListAddress + "/subscribe", "address=nobody" + i + "%40example.test").status);

         ClassicAssert.AreEqual(200, Public("POST", "/lists/" + ListAddress + "/subscribe", "address=joiner%40example.test").status);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Joiner, Password, 0);
         ClassicAssert.IsNull(Member(Joiner), "The twenty-first request made a request.");
         WaitFor(() => LogHandler.DefaultLogContains("its ceiling for one requester"), "The ceiling was not logged.");

         // The negative control: another list has its own count.
         DistributionList other = SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "other@example.test", new List<string> { Member1 });
         other.Managed = true;
         other.SubscribePolicy = 1;
         other.Save();

         Public("POST", "/lists/other@example.test/subscribe", "address=joiner%40example.test");
         StringAssert.Contains("Confirm your subscription to other@example.test", Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A command message that is itself automatic - Auto-Submitted - is never answered, so two servers cannot answer each other; the same request without it is answered with the help.")]
      public void AnAutomaticMessageToACommandAddressIsNeverAnswered()
      {
         AddAccounts(Member1, Joiner);
         MakeList(1, Member1);

         string automatic =
            "From: " + Joiner + "\r\n" +
            "To: team-request@example.test\r\n" +
            "Subject: Out of office\r\n" +
            "Auto-Submitted: auto-replied\r\n" +
            "\r\n" +
            "I am away.\r\n";

         SmtpClientSimulator.StaticSendRaw(Joiner, "team-request@example.test", automatic);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Joiner, Password, 0);

         SmtpClientSimulator.StaticSend(Joiner, "team-request@example.test", "help", "What are the addresses?");
         string help = Pop3ClientSimulator.AssertGetFirstMessageText(Joiner, Password);
         StringAssert.Contains("answers these addresses", help);
         StringAssert.Contains("team-unsubscribe@example.test", help);

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("name-owner@ reaches the list's owner, or with no owner the list's moderator.")]
      public void TheOwnerAddressReachesTheOwnerOrTheModerator()
      {
         AddAccounts(Member1, Member2, Owner, Joiner);
         DistributionList list = MakeList(1, Member1);
         list.ModeratorAddress = Member2;
         list.Save();

         SmtpClientSimulator.StaticSend(Joiner, "team-owner@example.test", "To the moderator", "Hello.");
         StringAssert.Contains("Subject: To the moderator", Pop3ClientSimulator.AssertGetFirstMessageText(Member2, Password));

         list.Owner = Owner;
         list.Save();

         SmtpClientSimulator.StaticSend(Joiner, "team-owner@example.test", "To the owner", "Hello.");
         StringAssert.Contains("Subject: To the owner", Pop3ClientSimulator.AssertGetFirstMessageText(Owner, Password));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(Member2, Password, 0);

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The footer goes on neither part of a signed posting, which appending would break, nor again on a reply that already quotes it; on a plain posting it goes on once.")]
      public void TheFooterLeavesASignedPostingAndAQuotingReplyAlone()
      {
         AddAccounts(Member1, Poster);
         DistributionList list = MakeList(1, Member1);
         list.Footer = "Sent through the team list.";
         list.Save();

         string signed =
            "From: " + Poster + "\r\n" +
            "To: " + ListAddress + "\r\n" +
            "Subject: Signed\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"signed-boundary\"\r\n" +
            "\r\n" +
            "--signed-boundary\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "Signed words.\r\n" +
            "--signed-boundary\r\n" +
            "Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\n" +
            "Content-Transfer-Encoding: base64\r\n" +
            "\r\n" +
            "MIIBSignatureBytesStandInHere\r\n" +
            "--signed-boundary--\r\n";

         SmtpClientSimulator.StaticSendRaw(Poster, ListAddress, signed);
         string signedCopy = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Signed words.", signedCopy);
         StringAssert.DoesNotContain("Sent through the team list.", signedCopy, "The footer was added to a signed posting.");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Re: earlier", "My answer.\r\n\r\n> Sent through the team list.");
         ClassicAssert.AreEqual(1, Count(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password), "Sent through the team list."),
            "The footer was added again to a reply that quotes it.");

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Plain", "Plain words.");
         ClassicAssert.AreEqual(1, Count(Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password), "Sent through the team list."));

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A report to the plain bounce address - no VERP - is read from its final recipients: without the member's tag it counts for nothing; with the header of a copy the list sent, it counts.")]
      public void APlainBounceCountsOnlyWithTheMembersTag()
      {
         AddAccounts(Member1, Poster);
         MakeList(1, Member1);

         SmtpClientSimulator.StaticSend(Poster, ListAddress, "Plain bounce", "Body.");
         string copy = Pop3ClientSimulator.AssertGetFirstMessageText(Member1, Password);
         StringAssert.Contains("Return-Path: <team-bounces@example.test>", copy);

         SmtpClientSimulator.StaticSendRaw("", "team-bounces@example.test", Dsn(Member1, "5.1.1", "From: somebody@elsewhere.example\r\nSubject: Not a copy\r\n"));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ClassicAssert.AreEqual(0, MiniJson.Number(Member(Member1), "hard_bounces"), "A forged plain report was counted.");

         SmtpClientSimulator.StaticSendRaw("", "team-bounces@example.test", Dsn(Member1, "5.1.1", HeaderBlock(copy)));
         WaitFor(() => MiniJson.Number(Member(Member1), "hard_bounces") == 1, "A plain report carrying the member's tag was not counted.");

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The four settings refuse what the list manager would not use: a base URL that is not http(s), or not printable ASCII; bounce counts of 0 and 101; a window of 366 days. Each refusal leaves the stored value as it was.")]
      public void TheListSettingsRefuseWhatTheManagerWouldNotUse()
      {
         PutSettings("{\"list_base_url\":\"" + BaseUrl + "\",\"list_hard_bounces_to_unsubscribe\":2,\"list_soft_bounces_to_unsubscribe\":6,\"list_bounce_window_days\":31}");

         string[] refused =
         {
            "{\"list_base_url\":\"ftp://lists.example.test\"}",
            "{\"list_base_url\":\"https://l\u00e4sts.example.test\"}",
            "{\"list_base_url\":\"https://lists.example.test/a b\"}",
            "{\"list_hard_bounces_to_unsubscribe\":0}",
            "{\"list_hard_bounces_to_unsubscribe\":101}",
            "{\"list_soft_bounces_to_unsubscribe\":0}",
            "{\"list_bounce_window_days\":366}"
         };

         foreach (string body in refused)
         {
            (int status, string answer) = Http("PUT", "/api/v1/settings", body);
            ClassicAssert.AreEqual(400, status, body + " was taken: " + answer);
         }

         Dictionary<string, object> settings = Settings();
         ClassicAssert.AreEqual(BaseUrl, MiniJson.Text(settings, "list_base_url"));
         ClassicAssert.AreEqual(2, MiniJson.Number(settings, "list_hard_bounces_to_unsubscribe"));
         ClassicAssert.AreEqual(6, MiniJson.Number(settings, "list_soft_bounces_to_unsubscribe"));
         ClassicAssert.AreEqual(31, MiniJson.Number(settings, "list_bounce_window_days"));

         CustomAsserts.AssertNoReportedError();
      }

      // ------------------------------------------------------------ helpers

      // ------------------------------------------------------------ a member's forward

      [Test]
      [Description("A posting from outside whose From names the list's domain, and that passed DMARC for it when it arrived, is distributed, and a member here that forwards its copy to an address outside sends it on from the list's bounce address: each copy carries what DMARC passed for when the posting arrived (docs/SendAs.md, the envelope a copy is sent on with).")]
      public void AMembersForwardOfAPostingThatPassedDmarcIsSentOn()
      {
         Account member = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Member1, Password);
         member.ForwardEnabled = true;
         member.ForwardAddress = "external@example.com";
         member.ForwardKeepOriginal = true;
         member.Save();
         MakeList(1, Member1);

         int markBefore = (int) _settings.AntiSpam.SpamMarkThreshold;
         int deleteBefore = (int) _settings.AntiSpam.SpamDeleteThreshold;
         bool dmarcBefore = _settings.AntiSpam.DMARCEnabled;
         Route route = null;
         try
         {
            _settings.AntiSpam.SpamMarkThreshold = 100;
            _settings.AntiSpam.SpamDeleteThreshold = 1000;
            _settings.AntiSpam.DMARCEnabled = true;
            _application.Reinitialize();

            // DMARC passes for example.test on arrival: SPF for the loopback at its
            // subdomain bounces.example.test, which aligns with the From (SuiteDns).
            SuiteDns.Zone
               .WithTxt("_dmarc.example.test", "v=DMARC1; p=none")
               .WithTxt("bounces.example.test", "v=spf1 ip4:127.0.0.1 -all");

            int port = TestSetup.GetNextFreePort();
            using (var server = new SmtpServerSimulator(1, port))
            {
               server.SecondsToWaitBeforeTerminate = 60;
               server.AddRecipientResult(new Dictionary<string, int> { ["external@example.com"] = 250 });
               server.StartListen();

               route = _settings.Routes.Add();
               route.DomainName = "example.com";
               route.TargetSMTPHost = "localhost";
               route.TargetSMTPPort = port;
               route.NumberOfTries = 1;
               route.MinutesBetweenTry = 5;
               route.Save();

               new SmtpClientSimulator().SendRaw("bounce@bounces.example.test", ListAddress,
                  "From: The CEO <ceo@example.test>\r\nTo: " + ListAddress + "\r\nSubject: From the CEO\r\n\r\nPlease read.\r\n");

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);

               Pop3ClientSimulator.AssertMessageCount(Member1, Password, 1);

               string mailFrom = server.NumberOfConnectedClients > 0 ? (server.MailFromCommand ?? "") : "";
               ClassicAssert.IsTrue(mailFrom.Length > 0,
                  "The member's forward of a posting that passed DMARC for example.test when it arrived was not sent on.");
               StringAssert.Contains("<team-bounces@example.test>", mailFrom,
                  "The member's forward should keep the envelope the list's copy was given, its bounce address.");
            }
         }
         finally
         {
            SuiteDns.Reset();
            if (route != null)
            {
               try { _settings.Routes.DeleteByDBID(route.ID); } catch (Exception) { }
            }
            _settings.AntiSpam.SpamMarkThreshold = markBefore;
            _settings.AntiSpam.SpamDeleteThreshold = deleteBefore;
            _settings.AntiSpam.DMARCEnabled = dmarcBefore;
            _application.Reinitialize();
         }
      }

      private void AddAccounts(params string[] addresses)
      {
         foreach (string address in addresses)
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
      }

      private DistributionList MakeList(int policy, params string[] members)
      {
         DistributionList list = SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, ListAddress, new List<string>(members));
         list.Managed = true;
         list.SubscribePolicy = policy;
         list.Save();
         return list;
      }

      private static void AddMember(DistributionList list, string address, int delivery)
      {
         DistributionListRecipient recipient = list.Recipients.Add();
         recipient.RecipientAddress = address;
         recipient.Delivery = delivery;
         recipient.Save();
         Marshal.ReleaseComObject(recipient);
      }

      /// <summary>The members route's entry for one address, or null when it is not a row.</summary>
      private static Dictionary<string, object> Member(string address)
      {
         (int status, string body) answer = Http("GET", "/api/v1/lists/" + ListAddress + "/members");
         ClassicAssert.AreEqual(200, answer.status, answer.body);

         foreach (object entry in MiniJson.List(MiniJson.Object(answer.body), "members"))
         {
            var item = entry as Dictionary<string, object>;
            if (item != null && string.Equals(MiniJson.Text(item, "address"), address, StringComparison.OrdinalIgnoreCase))
               return item;
         }

         return null;
      }

      private static Dictionary<string, object> ListEntry()
      {
         (int status, string body) answer = Http("GET", "/api/v1/domains/example.test/lists");
         ClassicAssert.AreEqual(200, answer.status, answer.body);

         object parsed = MiniJson.Parse(answer.body);
         IEnumerable<object> entries = parsed as List<object> ?? (IEnumerable<object>) MiniJson.List((Dictionary<string, object>) parsed, "lists");

         foreach (object entry in entries)
         {
            var item = entry as Dictionary<string, object>;
            if (item != null && MiniJson.Text(item, "address") == ListAddress)
               return item;
         }

         Assert.Fail("The list is not in the listing: " + answer.body);
         return null;
      }

      private static void WaitFor(Func<bool> condition, string message)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(30);

         while (DateTime.UtcNow < until)
         {
            if (condition())
               return;

            Thread.Sleep(100);
         }

         Assert.Fail(message);
      }

      private static List<string> Messages(string address, int count)
      {
         Pop3ClientSimulator.AssertMessageCount(address, Password, count);

         var texts = new List<string>();
         for (int i = 0; i < count; i++)
            texts.Add(Pop3ClientSimulator.AssertGetFirstMessageText(address, Password));

         return texts;
      }

      private static string Token(string text)
      {
         Match match = Regex.Match(text, "[0-9a-f]{64}");
         ClassicAssert.IsTrue(match.Success, "No token in: " + text);
         return match.Value;
      }

      private static string UnsubscribeToken(string copy)
      {
         Match match = Regex.Match(copy, "/lists/unsubscribe\\?t=([0-9a-f]{64})");
         ClassicAssert.IsTrue(match.Success, "No unsubscribe link in: " + copy);
         return match.Groups[1].Value;
      }

      /// <summary>A received message's header block, as a report returns it.</summary>
      private static string HeaderBlock(string message)
      {
         int end = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         return end > 0 ? message.Substring(0, end + 2) : message;
      }

      private static int Count(string haystack, string needle)
      {
         int count = 0;
         int index = haystack.IndexOf(needle, StringComparison.Ordinal);

         while (index >= 0)
         {
            count++;
            index = haystack.IndexOf(needle, index + needle.Length, StringComparison.Ordinal);
         }

         return count;
      }

      /// <summary>An RFC 3464 report about one recipient, returning the header given: a failure.</summary>
      private static string Dsn(string reportedRecipient, string status, string returnedHeaders)
      {
         return Dsn(reportedRecipient, status, returnedHeaders, "failed");
      }

      /// <summary>An RFC 3464 report about one recipient, with the Action given.</summary>
      private static string Dsn(string reportedRecipient, string status, string returnedHeaders, string action)
      {
         string boundary = "report-" + Guid.NewGuid().ToString("N");
         bool hard = status.StartsWith("5", StringComparison.Ordinal);

         return
            "From: MAILER-DAEMON@remote.example\r\n" +
            "To: team-bounces@example.test\r\n" +
            "Subject: Delivery Status Notification\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/report; report-type=delivery-status; boundary=\"" + boundary + "\"\r\n" +
            "\r\n" +
            "--" + boundary + "\r\n" +
            "Content-Type: text/plain\r\n" +
            "\r\n" +
            "The message could not be delivered.\r\n" +
            "\r\n" +
            "--" + boundary + "\r\n" +
            "Content-Type: message/delivery-status\r\n" +
            "\r\n" +
            "Reporting-MTA: dns; remote.example\r\n" +
            "\r\n" +
            "Final-Recipient: rfc822; " + reportedRecipient + "\r\n" +
            "Action: " + action + "\r\n" +
            "Status: " + status + "\r\n" +
            "Diagnostic-Code: smtp; " + (hard ? "550 5.1.1 No such user" : "452 4.2.2 Mailbox full") + "\r\n" +
            "\r\n" +
            "--" + boundary + "\r\n" +
            "Content-Type: text/rfc822-headers\r\n" +
            "\r\n" +
            returnedHeaders + "\r\n" +
            "--" + boundary + "--\r\n";
      }

      private static string QueryLink(string copy)
      {
         Match match = Regex.Match(copy, "https?://[^/>\\s]+/lists/unsubscribe\\?t=[0-9a-f]{64}&m=[^&>\\s]+&l=[0-9]+");
         ClassicAssert.IsTrue(match.Success, "No query member's link in: " + copy);
         return match.Value.Substring(match.Value.IndexOf("/lists/", StringComparison.Ordinal));
      }

      /// <summary>A webmail session of an account, as its cookie's value.</summary>
      private static string SignIn(string address)
      {
         string credential = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + Password));
         (int status, string headers, string body) created = SendRaw("POST", "/api/v1/session", credential, null, null);
         ClassicAssert.AreEqual(201, created.status, created.body);

         const string needle = "Set-Cookie: hmailsession=";
         int at = created.headers.IndexOf(needle, StringComparison.Ordinal);
         ClassicAssert.IsTrue(at >= 0, "No session cookie: " + created.headers);

         int start = at + needle.Length;
         int end = created.headers.IndexOfAny(new[] { ';', '\r' }, start);
         return end < 0 ? created.headers.Substring(start) : created.headers.Substring(start, end - start);
      }

      /// <summary>An HTTP request with its answer's header block, and one extra header line if given.</summary>
      private static (int status, string headers, string body) SendRaw(string method, string path, string authorization, string requestBody, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var request = new StringBuilder();
               request.Append(method + " " + path + " HTTP/1.0\r\n");
               request.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  request.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  request.Append("Content-Type: application/x-www-form-urlencoded\r\n");
                  request.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               if (extraHeaders != null)
                  request.Append(extraHeaders);

               request.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(request.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
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
               string responseHeaders = separator >= 0 ? raw.Substring(0, separator + 2) : raw;
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";
               return (statusCode, responseHeaders, body);
            }
         }
      }

      // ---- DKIM, verified here rather than trusted (RFC 6376 sections 3.4, 3.7)

      /// <summary>Each header field as its name and its whole text, folded lines and line end included.</summary>
      private static List<KeyValuePair<string, string>> HeaderFields(string header)
      {
         var fields = new List<KeyValuePair<string, string>>();
         StringBuilder current = null;
         string name = null;

         foreach (string line in header.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.Length == 0)
               continue;

            if ((line[0] == ' ' || line[0] == '\t') && current != null)
            {
               current.Append(line).Append("\r\n");
               continue;
            }

            if (current != null)
               fields.Add(new KeyValuePair<string, string>(name, current.ToString()));

            int colon = line.IndexOf(':');
            name = colon > 0 ? line.Substring(0, colon).Trim() : line;
            current = new StringBuilder(line).Append("\r\n");
         }

         if (current != null)
            fields.Add(new KeyValuePair<string, string>(name, current.ToString()));

         return fields;
      }

      private static string RelaxedHeader(string field)
      {
         int colon = field.IndexOf(':');
         string name = field.Substring(0, colon).Trim().ToLowerInvariant();
         string value = field.Substring(colon + 1).Replace("\r\n", "");
         value = Regex.Replace(value, "[ \t]+", " ").Trim();
         return name + ":" + value + "\r\n";
      }

      private static string CanonicalBody(string body, bool relaxed)
      {
         var lines = new List<string>(body.Split(new[] { "\r\n" }, StringSplitOptions.None));
         if (lines.Count > 0 && lines[lines.Count - 1].Length == 0)
            lines.RemoveAt(lines.Count - 1);

         if (relaxed)
            lines = lines.Select(line => Regex.Replace(line, "[ \t]+", " ").TrimEnd(' ')).ToList();

         while (lines.Count > 0 && lines[lines.Count - 1].Length == 0)
            lines.RemoveAt(lines.Count - 1);

         if (lines.Count == 0)
            return relaxed ? "" : "\r\n";

         return string.Join("\r\n", lines) + "\r\n";
      }

      private static Dictionary<string, string> Tags(string value)
      {
         var tags = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (string part in value.Split(';'))
         {
            int equals = part.IndexOf('=');
            if (equals < 0)
               continue;
            tags[Regex.Replace(part.Substring(0, equals), "\\s", "")] = Regex.Replace(part.Substring(equals + 1), "\\s", "");
         }
         return tags;
      }

      /// <summary>The tags of the topmost DKIM-Signature of a header block.</summary>
      private static Dictionary<string, string> DkimTags(string header)
      {
         List<KeyValuePair<string, string>> fields = HeaderFields(header);
         int at = fields.FindIndex(field => string.Equals(field.Key, "DKIM-Signature", StringComparison.OrdinalIgnoreCase));
         ClassicAssert.IsTrue(at >= 0, "No DKIM-Signature in: " + header);
         string signatureField = fields[at].Value;
         return Tags(signatureField.Substring(signatureField.IndexOf(':') + 1));
      }

      /// <summary>
      ///    Whether the topmost DKIM-Signature of a message verifies with the key: the
      ///    body hash over the body as it is, and the RSA signature over the fields h=
      ///    names, each taken from the bottom up, then the signature field with its b=
      ///    value emptied - both canonicalized as its c= tag says.
      /// </summary>
      private static bool DkimSignatureVerifies(string message, RSA publicKey, out string why)
      {
         int separator = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         string header = separator >= 0 ? message.Substring(0, separator + 2) : message;
         string messageBody = separator >= 0 ? message.Substring(separator + 4) : "";

         List<KeyValuePair<string, string>> fields = HeaderFields(header);
         int at = fields.FindIndex(field => string.Equals(field.Key, "DKIM-Signature", StringComparison.OrdinalIgnoreCase));
         if (at < 0)
         {
            why = "the message carries no DKIM-Signature";
            return false;
         }

         string signatureField = fields[at].Value;
         Dictionary<string, string> tags = Tags(signatureField.Substring(signatureField.IndexOf(':') + 1));
         if (!tags.ContainsKey("bh") || !tags.ContainsKey("b") || !tags.ContainsKey("h"))
         {
            why = "the DKIM-Signature lacks bh=, b= or h=: " + signatureField;
            return false;
         }

         string[] methods = (tags.ContainsKey("c") ? tags["c"] : "simple/simple").Split('/');
         bool relaxedHeader = methods[0] == "relaxed";
         bool relaxedBody = methods.Length > 1 && methods[1] == "relaxed";

         string bodyHash;
         using (SHA256 sha = SHA256.Create())
            bodyHash = Convert.ToBase64String(sha.ComputeHash(Encoding.UTF8.GetBytes(CanonicalBody(messageBody, relaxedBody))));

         if (bodyHash != tags["bh"])
         {
            why = "bh= is " + tags["bh"] + " and the body hashes to " + bodyHash;
            return false;
         }

         var used = new HashSet<int> { at };
         var signed = new StringBuilder();
         foreach (string wanted in tags["h"].Split(':'))
         {
            for (int i = fields.Count - 1; i >= 0; i--)
            {
               if (used.Contains(i) || !string.Equals(fields[i].Key, wanted, StringComparison.OrdinalIgnoreCase))
                  continue;
               used.Add(i);
               signed.Append(relaxedHeader ? RelaxedHeader(fields[i].Value) : fields[i].Value);
               break;
            }
         }

         string withoutSignature = Regex.Replace(signatureField, "(^|;)(\\s*b\\s*=)[^;]*", "$1$2");
         string canonical = relaxedHeader ? RelaxedHeader(withoutSignature) : withoutSignature;
         if (canonical.EndsWith("\r\n", StringComparison.Ordinal))
            canonical = canonical.Substring(0, canonical.Length - 2);
         signed.Append(canonical);

         bool verified = publicKey.VerifyData(Encoding.UTF8.GetBytes(signed.ToString()), Convert.FromBase64String(tags["b"]),
            HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

         why = verified ? "" : "the RSA signature does not verify over:\r\n" + signed;
         return verified;
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

      private static string AdminHeader
      {
         get { return "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)); }
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, AdminHeader, requestBody);
      }

      private static (int status, string body) Public(string method, string path, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, null, requestBody);
      }

      private static (int status, string body) As(string address, string method, string path, string requestBody = null)
      {
         string credential = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + Password));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, credential, requestBody);
      }
   }
}
