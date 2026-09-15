// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The domain signature and the domain disclaimer on mail a person writes in the
   ///    webmail: POST /api/v1/me/messages, and the send-later task that sends a draft
   ///    when its minute comes. Until 15 September 2026 neither footer went on either,
   ///    because both paths write the message into the queue themselves and the two
   ///    adders were called only on the SMTP accept path. Both now go through
   ///    OutgoingFooters, which the SMTP path calls too; SMTP.DomainTransforms has the
   ///    SMTP versions of these tests, and this fixture holds the webmail to the same
   ///    rules.
   ///
   ///    Every outward message is captured by a listener a route of the test's own
   ///    points at, because what the partner receives is the claim. Every exclusion is
   ///    asserted on a message the footers demonstrably ran on - the domain signature is
   ///    on it, or an unexcluded control sent with the same settings carries both - so
   ///    that "no disclaimer" cannot pass because nothing ran at all.
   ///
   ///    The null sender, the one disclaimer exclusion not driven here, cannot be
   ///    reached from the webmail: its envelope sender is always the signed-in account
   ///    or one of its identities.
   /// </summary>
   [TestFixture]
   public class WebmailFooters : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9544;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Footers-Passw0rd!";

      // Six different sentences, none containing another, so that a count of one
      // is a count of one - a footer whose HTML form contained its plain form would
      // read one appended copy as two. No "=" and no trailing space in any of them,
      // which the quoted-printable the webmail writes its parts in would encode.
      private const string DisclaimerText = "Confidential: this note may be privileged.";
      private const string DisclaimerHtml = "<p>Confidential in HTML: may be privileged.</p>";
      private const string DisclaimerHtmlWords = "Confidential in HTML: may be privileged.";
      private const string SignatureText = "Sent on behalf of the example domain.";
      private const string SignatureHtml = "<p>Signed in HTML by the example domain.</p>";
      private const string SignatureHtmlWords = "Signed in HTML by the example domain.";
      private const string AccountSignatureText = "Ada Lovelace, analytical engines.";

      private const string AppliedHeader = "X-hMailServer-Disclaimer";

      private Account _sender;

      private string Address
      {
         get { return "writer@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         _sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status", AdminHeader());
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      // ------------------------------------------------------------ settings ---

      private void EnableDisclaimer(Domain domain, string text, string html)
      {
         domain.DisclaimerEnabled = true;
         domain.DisclaimerPlainText = text;
         domain.DisclaimerHTML = html;
         domain.Save();
      }

      private void EnableDomainSignature(eDomainSignatureMethod method)
      {
         _domain.SignatureEnabled = true;
         _domain.SignaturePlainText = SignatureText;
         _domain.SignatureHTML = SignatureHtml;
         _domain.SignatureMethod = method;

         // Both said rather than inherited, because two tests rely on them: a
         // domain signs mail between colleagues, and does not sign replies.
         _domain.AddSignaturesToLocalMail = true;
         _domain.AddSignaturesToReplies = false;
         _domain.Save();
      }

      // ------------------------------------------------------------- sending ---

      private static int Occurrences(string haystack, string needle)
      {
         int count = 0;

         for (int at = haystack.IndexOf(needle, StringComparison.Ordinal); at >= 0;
              at = haystack.IndexOf(needle, at + needle.Length, StringComparison.Ordinal))
            count++;

         return count;
      }

      private static string JsonText(string text)
      {
         return "\"" + text.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n") + "\"";
      }

      /// <summary>
      ///    Runs send with a route for domainName pointed at a listener of this test's
      ///    own, and answers the message as that listener received it. The route is
      ///    taken away again, because a test that sends twice would otherwise leave two
      ///    routes for one domain and aim the second message at a listener that has gone.
      /// </summary>
      private string CaptureOutwards(string domainName, string recipient, Action send)
      {
         int capturePort = TestSetup.GetNextFreePort();

         using (var capture = new SmtpServerSimulator(1, capturePort))
         {
            capture.AddRecipientResult(new Dictionary<string, int> {{recipient, 250}});
            capture.StartListen();

            var route = _settings.Routes.Add();

            try
            {
               route.DomainName = domainName;
               route.TargetSMTPHost = "localhost";
               route.TargetSMTPPort = capturePort;
               route.NumberOfTries = 1;
               route.MinutesBetweenTry = 5;
               route.Save();

               send();

               capture.WaitForCompletion();

               return capture.MessageData;
            }
            finally
            {
               if (route.ID > 0)
                  _settings.Routes.DeleteByDBID(route.ID);
            }
         }
      }

      /// <summary>POST /api/v1/me/messages as the page posts it, answered 201.</summary>
      private void SendFromWebmail(string authorization, string json)
      {
         (int status, string body) sent = Http("POST", "/api/v1/me/messages", authorization, json);
         Assert.AreEqual(201, sent.status, "The webmail's send was refused. Body: " + sent.body);
      }

      private string WebmailOutwards(string domainName, string recipient, string json)
      {
         return CaptureOutwards(domainName, recipient, () => SendFromWebmail(UserHeader(), json));
      }

      private static string TextAndHtml(string to, string subject)
      {
         return "{\"to\":\"" + to + "\",\"subject\":\"" + subject + "\"," +
                "\"text\":\"Here is the quotation.\",\"html\":\"<p>Here is the quotation.</p>\"}";
      }

      // --------------------------------------------------------------- tests ---

      [Test]
      [Description("A message written in the webmail to a partner outside gets the domain's signature and its disclaimer, each once in the text part and once in the HTML part, and nothing while the switches are off.")]
      public void AWebmailSendGetsTheSignatureAndTheDisclaimer()
      {
         const string partner = "partner@outside-webmail.test";

         // The control: with both switches off the same send carries neither, so
         // what is asserted below comes from the settings and from nowhere else.
         string before = WebmailOutwards("outside-webmail.test", partner, TextAndHtml(partner, "Before"));
         StringAssert.Contains("Here is the quotation.", before, "The message itself must arrive: " + before);
         StringAssert.DoesNotContain(DisclaimerText, before, "Nothing may be added while the disclaimer is off: " + before);
         StringAssert.DoesNotContain(SignatureText, before, "Nothing may be added while the signature is off: " + before);
         StringAssert.DoesNotContain(AppliedHeader, before);

         EnableDomainSignature(eDomainSignatureMethod.eSMSetIfNotSpecifiedInAccount);
         EnableDisclaimer(_domain, DisclaimerText, DisclaimerHtml);

         string sent = WebmailOutwards("outside-webmail.test", partner, TextAndHtml(partner, "Quotation"));

         ClassicAssert.AreEqual(1, Occurrences(sent, SignatureText),
            "The domain signature must be appended to the text part exactly once: " + sent);
         ClassicAssert.AreEqual(1, Occurrences(sent, SignatureHtmlWords),
            "The domain signature must be appended to the HTML part exactly once: " + sent);
         ClassicAssert.AreEqual(1, Occurrences(sent, DisclaimerText),
            "The disclaimer must be appended to the text part exactly once: " + sent);
         ClassicAssert.AreEqual(1, Occurrences(sent, DisclaimerHtmlWords),
            "The disclaimer must be appended to the HTML part exactly once: " + sent);
         StringAssert.Contains(AppliedHeader + ": " + _domain.Name, sent,
            "The message must be stamped with the domain that footed it, as on the SMTP path.");
         ClassicAssert.Less(sent.IndexOf(SignatureText, StringComparison.Ordinal), sent.IndexOf(DisclaimerText, StringComparison.Ordinal),
            "The disclaimer goes below the signature, as on the SMTP path: " + sent);
      }

      [Test]
      [Description("A draft sent later by the send-later task gets the signature and the disclaimer when it goes to a partner outside, and the signature without the disclaimer when it goes to a colleague only.")]
      public void AScheduledSendGetsTheSignatureAndTheDisclaimer()
      {
         const string partner = "partner@outside-later.test";
         string colleague = "colleague@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, colleague, UserPassword);

         EnableDomainSignature(eDomainSignatureMethod.eSMSetIfNotSpecifiedInAccount);
         EnableDisclaimer(_domain, DisclaimerText, DisclaimerHtml);

         string sent = CaptureOutwards("outside-later.test", partner, () =>
         {
            long outward = SaveDraft("{\"to\":\"" + partner + "\",\"subject\":\"Later, outside\",\"text\":\"Sent when the minute comes.\"}");
            long inward = SaveDraft("{\"to\":\"" + colleague + "\",\"subject\":\"Later, inside\",\"text\":\"Sent when the minute comes.\"}");

            // Both on the same minute, so one run of the task sends both.
            string at = DateTime.Now.AddSeconds(4).ToString("yyyy-MM-dd HH:mm:ss");
            Schedule(outward, at);
            Schedule(inward, at);

            Thread.Sleep(5000);

            // Not asserted on "ran": the task's own minute tick may have sent them
            // first, which is the same code and just as good.
            (int status, string body) ran = Http("POST", "/api/v1/scheduled/run", AdminHeader());
            Assert.AreEqual(200, ran.status, "Body: " + ran.body);
         });

         StringAssert.Contains("Sent when the minute comes.", sent, "The draft itself must arrive: " + sent);
         ClassicAssert.AreEqual(1, Occurrences(sent, SignatureText),
            "A scheduled send must carry the domain signature once: " + sent);
         ClassicAssert.AreEqual(1, Occurrences(sent, DisclaimerText),
            "A scheduled send to a partner outside must carry the disclaimer once: " + sent);
         StringAssert.Contains(AppliedHeader, sent);

         // The colleague's copy went through the same function: the signature says so
         // (a domain adds its signature to local mail unless told not to), and the
         // disclaimer's absence is then its own rule - no recipient outside.
         string inside = Pop3ClientSimulator.AssertGetFirstMessageText(colleague, UserPassword);
         StringAssert.Contains("Later, inside", inside);
         ClassicAssert.AreEqual(1, Occurrences(inside, SignatureText),
            "The scheduled send to a colleague must still carry the domain signature: " + inside);
         StringAssert.DoesNotContain(DisclaimerText, inside,
            "A message with no recipient outside this server gets no disclaimer: " + inside);
         StringAssert.DoesNotContain(AppliedHeader, inside);
      }

      [Test]
      [Description("The webmail writes the account's own signature into the compose box, so the server adds only what the domain adds on top of it - never the account's a second time - while over SMTP the server still adds both.")]
      public void TheAccountsOwnSignatureIsNotAddedTwiceByTheWebmail()
      {
         const string partner = "partner@outside-signed-twice.test";

         _sender.SignatureEnabled = true;
         _sender.SignaturePlainText = AccountSignatureText;
         _sender.Save();

         EnableDomainSignature(eDomainSignatureMethod.eSMAppendToAccountSignature);

         // The control, and the reason "once" below means anything: over SMTP, with
         // these very settings, the server adds the account's signature itself.
         string overSmtp = CaptureOutwards("outside-signed-twice.test", partner, () =>
            new SmtpClientSimulator().SendRaw(Address, UserPassword, Address, partner,
               "From: " + Address + "\r\n" +
               "To: " + partner + "\r\n" +
               "Subject: Over SMTP\r\n" +
               "\r\n" +
               "Thank you.\r\n"));

         ClassicAssert.AreEqual(1, Occurrences(overSmtp, AccountSignatureText),
            "Over SMTP the server adds the account's signature: " + overSmtp);
         ClassicAssert.AreEqual(1, Occurrences(overSmtp, SignatureText),
            "Over SMTP the server appends the domain's signature to it: " + overSmtp);

         // The webmail: the body as the page sends it, the account's signature
         // already in it below the "-- " line the page writes.
         string pageBody = "Thank you.\n\n-- \n" + AccountSignatureText;

         string appended = WebmailOutwards("outside-signed-twice.test", partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"From the webmail\",\"text\":" + JsonText(pageBody) + "}");

         ClassicAssert.AreEqual(1, Occurrences(appended, AccountSignatureText),
            "The account's signature the page wrote must not be added a second time: " + appended);
         ClassicAssert.AreEqual(1, Occurrences(appended, SignatureText),
            "The domain's signature is still appended to it: " + appended);

         // Set-if-not-specified: the account has a signature, so the domain adds
         // nothing - decided against the account's signature, not against the body.
         EnableDomainSignature(eDomainSignatureMethod.eSMSetIfNotSpecifiedInAccount);

         string kept = WebmailOutwards("outside-signed-twice.test", partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"From the webmail again\",\"text\":" + JsonText(pageBody) + "}");

         ClassicAssert.AreEqual(1, Occurrences(kept, AccountSignatureText),
            "The account's signature appears once, as the page wrote it: " + kept);
         StringAssert.DoesNotContain(SignatureText, kept,
            "An account that has its own signature gets no domain signature under set-if-not-specified: " + kept);
      }

      [Test]
      [Description("A webmail message sent from an identity - an alias of the account - is still decided against the signed-in account's signature, which is the one the page wrote, and its name fills the domain signature's macros.")]
      public void AnIdentitysMessageIsDecidedAgainstTheSignedInAccount()
      {
         const string partner = "partner@outside-identity.test";
         string alias = "sales@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, alias, Address);

         _sender.PersonFirstName = "Ada";
         _sender.SignatureEnabled = true;
         _sender.SignaturePlainText = AccountSignatureText;
         _sender.Save();

         string pageBody = "Thank you.\n\n-- \n" + AccountSignatureText;
         string fromAlias = "{\"to\":\"" + partner + "\",\"from\":\"" + alias + "\",\"subject\":\"From sales\",\"text\":" + JsonText(pageBody) + "}";

         // Set-if-not-specified. The alias is no account and has no signature of its
         // own, but the person who wrote the message has one and the page put it in
         // the body, so the domain's is not added.
         EnableDomainSignature(eDomainSignatureMethod.eSMSetIfNotSpecifiedInAccount);

         string decided = WebmailOutwards("outside-identity.test", partner, fromAlias);
         StringAssert.Contains("sales@", decided, "The message must go from the alias: " + decided);
         ClassicAssert.AreEqual(1, Occurrences(decided, AccountSignatureText), "The page's signature, once: " + decided);
         StringAssert.DoesNotContain(SignatureText, decided,
            "The signed-in account has a signature, so set-if-not-specified adds no domain signature, whatever identity the message is from: " + decided);

         // Append, with a macro: filled from the signed-in account, which is the
         // person, not left as written because an alias has no name.
         _domain.SignatureEnabled = true;
         _domain.SignaturePlainText = "Sent by %User.FirstName% for the example domain.";
         _domain.SignatureHTML = "";
         _domain.SignatureMethod = eDomainSignatureMethod.eSMAppendToAccountSignature;
         _domain.Save();

         string appended = WebmailOutwards("outside-identity.test", partner, fromAlias);
         StringAssert.Contains("Sent by Ada for the example domain.", appended,
            "The domain signature's macro must be filled with the signed-in account's name: " + appended);
         StringAssert.DoesNotContain("%User.FirstName%", appended);
         ClassicAssert.AreEqual(1, Occurrences(appended, AccountSignatureText), "Still the page's signature once: " + appended);
      }

      [Test]
      [Description("On the webmail path the disclaimer is left off an automatic reply, a list posting, a message this server already stamped and a message to colleagues only, and not added twice to a reply that quotes it; the signature is left off a reply when the domain says so.")]
      public void TheDisclaimersExclusionsHoldOnTheWebmailPath()
      {
         const string partner = "partner@outside-excluded.test";
         const string outside = "outside-excluded.test";
         string colleague = "colleague@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, colleague, UserPassword);

         EnableDomainSignature(eDomainSignatureMethod.eSMSetIfNotSpecifiedInAccount);
         EnableDisclaimer(_domain, DisclaimerText, "");

         // An automatic reply, marked as one (RFC 3834). Written as a page-built
         // entity, whose own header lines join the message's header block.
         string autoReply = WebmailOutwards(outside, partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"Away\",\"mime\":" +
            JsonText("Content-Type: text/plain; charset=us-ascii\r\nAuto-Submitted: auto-replied\r\n\r\nI am away this week.\r\n") + "}");
         ClassicAssert.AreEqual(1, Occurrences(autoReply, SignatureText),
            "The footers ran on the automatic reply - the signature is on it: " + autoReply);
         StringAssert.DoesNotContain(DisclaimerText, autoReply, "An automatic reply gets no disclaimer: " + autoReply);

         string listPosting = WebmailOutwards(outside, partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"To the list\",\"mime\":" +
            JsonText("Content-Type: text/plain; charset=us-ascii\r\nList-Id: The partners <partners.outside-excluded.test>\r\n\r\nA posting.\r\n") + "}");
         ClassicAssert.AreEqual(1, Occurrences(listPosting, SignatureText),
            "The footers ran on the list posting: " + listPosting);
         StringAssert.DoesNotContain(DisclaimerText, listPosting, "A list posting gets no disclaimer: " + listPosting);

         string stamped = WebmailOutwards(outside, partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"Already footed\",\"mime\":" +
            JsonText("Content-Type: text/plain; charset=us-ascii\r\n" + AppliedHeader + ": " + _domain.Name + "\r\n\r\nFooted once already.\r\n") + "}");
         ClassicAssert.AreEqual(1, Occurrences(stamped, SignatureText),
            "The footers ran on the stamped message: " + stamped);
         StringAssert.DoesNotContain(DisclaimerText, stamped, "A message this server already stamped is not footed again: " + stamped);

         // A reply quoting a message that carried the disclaimer: one copy, the
         // quoted one. And a reply, so with AddSignaturesToReplies off the signature is
         // left off - the signature's own rule, on the same path.
         string reply = WebmailOutwards(outside, partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"Re: Quotation\",\"in_reply_to\":\"<quotation@outside-excluded.test>\",\"text\":" +
            JsonText("Thank you.\n\n> Here is the quotation.\n>\n> " + DisclaimerText) + "}");
         ClassicAssert.AreEqual(1, Occurrences(reply, DisclaimerText),
            "A reply that already quotes the disclaimer must not collect a second copy: " + reply);
         StringAssert.DoesNotContain(SignatureText, reply,
            "A domain that does not sign replies does not sign a webmail reply either: " + reply);

         // The control for the reply: the same text as a new message gets both, so
         // the two assertions above are the rules' doing.
         string fresh = WebmailOutwards(outside, partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"Quotation\",\"text\":\"Here is the quotation.\"}");
         ClassicAssert.AreEqual(1, Occurrences(fresh, DisclaimerText), "A new message gets the disclaimer: " + fresh);
         ClassicAssert.AreEqual(1, Occurrences(fresh, SignatureText), "A new message gets the signature: " + fresh);

         // Colleagues only: the signature says the footers ran, the disclaimer's
         // absence is its rule.
         SendFromWebmail(UserHeader(), "{\"to\":\"" + colleague + "\",\"subject\":\"Lunch\",\"text\":\"Lunch at one?\"}");
         string inside = Pop3ClientSimulator.AssertGetFirstMessageText(colleague, UserPassword);
         ClassicAssert.AreEqual(1, Occurrences(inside, SignatureText), "The footers ran on the note to a colleague: " + inside);
         StringAssert.DoesNotContain(DisclaimerText, inside, "A note to a colleague only gets no disclaimer: " + inside);
      }

      [Test]
      [Description("A signed message gets neither footer, whichever way it is sent: the webmail's page-built S/MIME entity and a desktop client's over SMTP both arrive byte for byte, where an unsigned message with the same settings gets both.")]
      public void ASignedMessageGetsNeitherFooterOnEitherPath()
      {
         const string partner = "partner@outside-smime.test";
         const string outside = "outside-smime.test";

         EnableDomainSignature(eDomainSignatureMethod.eSMSetIfNotSpecifiedInAccount);
         EnableDisclaimer(_domain, DisclaimerText, DisclaimerHtml);

         // The control: these settings put both footers on an unsigned message.
         string plainMessage = WebmailOutwards(outside, partner, TextAndHtml(partner, "Unsigned"));
         ClassicAssert.AreEqual(1, Occurrences(plainMessage, SignatureText), "An unsigned message gets the signature: " + plainMessage);
         ClassicAssert.AreEqual(1, Occurrences(plainMessage, DisclaimerText), "An unsigned message gets the disclaimer: " + plainMessage);

         string entity =
            "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"bb\"\r\n\r\n" +
            "--bb\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\nSigned on the page.\r\n" +
            "--bb\r\nContent-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\nContent-Transfer-Encoding: base64\r\n" +
            "Content-Disposition: attachment; filename=\"smime.p7s\"\r\n\r\nAAECAwQFBgc=\r\n--bb--\r\n";

         string fromPage = WebmailOutwards(outside, partner,
            "{\"to\":\"" + partner + "\",\"subject\":\"Signed on the page\",\"mime\":" + JsonText(entity) + "}");
         StringAssert.Contains(entity, fromPage, "The signed entity must arrive exactly as the page built it: " + fromPage);
         StringAssert.DoesNotContain(SignatureText, fromPage, "No signature inside a signed message: " + fromPage);
         StringAssert.DoesNotContain(DisclaimerText, fromPage, "No disclaimer inside a signed message: " + fromPage);
         StringAssert.DoesNotContain(AppliedHeader, fromPage);

         // A desktop client's signed message over SMTP. Before 15 September 2026 the
         // domain signature was appended inside the multipart/signed here, breaking it.
         const string signedBody = "Signed in a desktop client.";
         string overSmtp = CaptureOutwards(outside, partner, () =>
            new SmtpClientSimulator().SendRaw(Address, UserPassword, Address, partner,
               "From: " + Address + "\r\n" +
               "To: " + partner + "\r\n" +
               "Subject: Signed over SMTP\r\n" +
               "MIME-Version: 1.0\r\n" +
               "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"cc\"\r\n" +
               "\r\n" +
               "--cc\r\n" +
               "Content-Type: text/plain; charset=us-ascii\r\n" +
               "\r\n" +
               signedBody + "\r\n" +
               "--cc\r\n" +
               "Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\n" +
               "Content-Transfer-Encoding: base64\r\n" +
               "\r\n" +
               "bm90LWEtcmVhbC1zaWduYXR1cmU=\r\n" +
               "--cc--\r\n"));

         StringAssert.Contains("\r\n\r\n" + signedBody + "\r\n--cc\r\n", overSmtp,
            "The signed part must arrive unchanged over SMTP: " + overSmtp);
         StringAssert.DoesNotContain(SignatureText, overSmtp, "No signature inside a signed message over SMTP: " + overSmtp);
         StringAssert.DoesNotContain(DisclaimerText, overSmtp, "No disclaimer inside a signed message over SMTP: " + overSmtp);
      }

      [Test]
      [Description("A webmail send by an account on one domain carries that domain's disclaimer and not another hosted domain's, in both directions.")]
      public void EachSenderGetsTheirOwnDomainsDisclaimer()
      {
         const string partner = "partner@outside-two-domains.test";
         const string outside = "outside-two-domains.test";
         const string secondText = "The second domain's own footer, in its words.";

         var second = SingletonProvider<TestSetup>.Instance.AddDomain("second-footers.test");
         string secondAddress = "writer@second-footers.test";
         SingletonProvider<TestSetup>.Instance.AddAccount(second, secondAddress, UserPassword);

         EnableDisclaimer(_domain, DisclaimerText, "");
         EnableDisclaimer(second, secondText, "");

         string fromFirst = WebmailOutwards(outside, partner, "{\"to\":\"" + partner + "\",\"subject\":\"From the first\",\"text\":\"One.\"}");
         ClassicAssert.AreEqual(1, Occurrences(fromFirst, DisclaimerText), "The first domain's account gets the first domain's footer: " + fromFirst);
         StringAssert.DoesNotContain(secondText, fromFirst, "and not the second domain's: " + fromFirst);
         StringAssert.Contains(AppliedHeader + ": " + _domain.Name, fromFirst);

         string fromSecond = CaptureOutwards(outside, partner, () =>
            SendFromWebmail(BasicHeader(secondAddress, UserPassword),
               "{\"to\":\"" + partner + "\",\"subject\":\"From the second\",\"text\":\"Two.\"}"));
         ClassicAssert.AreEqual(1, Occurrences(fromSecond, secondText), "The second domain's account gets the second domain's footer: " + fromSecond);
         StringAssert.DoesNotContain(DisclaimerText, fromSecond, "and not the first domain's: " + fromSecond);
         StringAssert.Contains(AppliedHeader + ": second-footers.test", fromSecond);
      }

      // ------------------------------------------------------------- drafts ---

      private long SaveDraft(string json)
      {
         (int status, string body) draft = Http("POST", "/api/v1/me/drafts", UserHeader(), json);
         Assert.AreEqual(201, draft.status, "Body: " + draft.body);
         return long.Parse(Between(draft.body, "\"id\":", ","));
      }

      private void Schedule(long draftId, string at)
      {
         (int status, string body) scheduled = Http("POST", "/api/v1/me/drafts/" + draftId + "/schedule", UserHeader(), "{\"send_at\":\"" + at + "\"}");
         Assert.AreEqual(201, scheduled.status, "Body: " + scheduled.body);
      }

      private static string Between(string body, string after, string until)
      {
         int start = body.IndexOf(after, StringComparison.Ordinal);
         Assert.IsTrue(start >= 0, "Missing " + after + " in: " + body);
         start += after.Length;
         int end = body.IndexOf(until, start, StringComparison.Ordinal);
         Assert.IsTrue(end >= start, "Unterminated " + after + " in: " + body);
         return body.Substring(start, end - start);
      }

      // --------------------------------------------------------------- HTTP ---

      private static string AdminHeader()
      {
         return BasicHeader("Administrator", AdminPassword);
      }

      private string UserHeader()
      {
         return BasicHeader(Address, UserPassword);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      // One HTTP/1.0 request against the REST listener, answering the status and
      // the body. The connect is retried briefly to absorb the listener's bind
      // right after a reinitialize.
      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null)
      {
         using (var client = new TcpClient())
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

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string head = separator >= 0 ? raw.Substring(0, separator) : raw;
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               int status = 0;
               string[] firstLine = head.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               if (firstLine.Length >= 2)
                  int.TryParse(firstLine[1], out status);

               return (status, body);
            }
         }
      }
   }
}
