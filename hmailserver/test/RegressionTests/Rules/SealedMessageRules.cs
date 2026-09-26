// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Rules
{
   /// <summary>
   ///    What a rule's body criterion does to a message the server cannot read.
   ///
   ///    The body of a signed or encrypted message is not empty. It is unreadable,
   ///    and the rule engine used to state those two facts identically: GetBody()
   ///    walks the MIME tree for a text/plain part, a sealed message has none, and
   ///    the criterion was therefore compared against "". "Contains" then missed,
   ///    harmlessly. "NotContains" matched EVERY sealed message - so the rule an
   ///    organisation writes when it takes confidentiality seriously,
   ///
   ///        body does not contain our confidentiality notice -> move to Held
   ///
   ///    swallowed every encrypted message it ever received, silently, and the more
   ///    seriously the organisation took encryption the more of its mail vanished.
   ///
   ///    The criterion now declines: a body criterion the server cannot evaluate
   ///    does not match, whatever the match type, and one line per rule per message
   ///    says so. Two things are deliberately NOT changed, and both have a test
   ///    here: an ordinary message is filtered exactly as before, and a criterion
   ///    declining is not the whole rule declining - the other criteria of the same
   ///    rule are still evaluated, so an "anyof" rule still fires on its subject.
   ///
   ///    See hmailserver/docs/SealedMail.md.
   /// </summary>
   [TestFixture]
   public class SealedMessageRules : TestFixtureBase
   {
      private const string Password = "secret";
      private const string HeldFolder = "INBOX.Held";
      private const string HeldFolderName = "Inbox.Held";
      private const string RuleName = "Hold what carries no notice";

      private const string Notice = "This message is confidential.";
      // Real OpenPGP packets, armoured - a session key packet and then encrypted
      // data - because the server decides "encrypted" from what the ciphertext
      // part holds and not from the label.
      private const string CiphertextMarker = "wQwDAAAAAAAAAAABAAHSGQFzZWFsZWQtY2lwaGVydGV4dC1tYXJrZXI=";

      // SensitiveContent by its IDL number, as OutboundContentPolicy uses it.
      private const int SensitiveContent = 10;

      private Account _account;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sealed-rules@example.test", Password);

         // A global rule: PerformBasicSetup removes every one of them before each
         // test, so nothing this fixture creates can reach the next one.
         var rule = _application.Rules.Add();
         rule.Name = RuleName;
         rule.Active = true;

         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTBody;
         criteria.MatchType = eRuleMatchType.eMTNotContains;
         criteria.MatchValue = Notice;
         criteria.Save();

         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRAMoveToImapFolder;
         action.IMAPFolder = HeldFolder;
         action.Save();

         rule.Save();

         // The folder the rule moves into exists from the start, so that a test
         // can ask it for a count of zero: a rule that never fires never creates
         // it, and a folder that is not there cannot be examined.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_account.Address, Password));
         Assert.IsTrue(imap.CreateFolder(HeldFolderName));
         imap.Disconnect();
      }

      private static string PlainMessage(string recipient, string body)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Sealed rules\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                body + "\r\n";
      }

      private static string PgpMimeMessage(string recipient)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Sealed rules\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";\r\n" +
                " boundary=\"sealed-boundary\"\r\n" +
                "\r\n" +
                "--sealed-boundary\r\n" +
                "Content-Type: application/pgp-encrypted\r\n" +
                "\r\n" +
                "Version: 1\r\n" +
                "\r\n" +
                "--sealed-boundary\r\n" +
                "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n" +
                "Content-Disposition: inline; filename=\"encrypted.asc\"\r\n" +
                "\r\n" +
                "-----BEGIN PGP MESSAGE-----\r\n" +
                "\r\n" +
                CiphertextMarker + "\r\n" +
                "=abcd\r\n" +
                "-----END PGP MESSAGE-----\r\n" +
                "\r\n" +
                "--sealed-boundary--\r\n";
      }

      /// <summary>
      ///    A multipart/encrypted label over an ordinary text part: nothing in it is
      ///    encrypted, so every rule reads the text as it would in any message.
      /// </summary>
      private static string FakeEncryptedMessage(string recipient, string body)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Sealed rules\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";\r\n" +
                " boundary=\"fake-boundary\"\r\n" +
                "\r\n" +
                "--fake-boundary\r\n" +
                "Content-Type: application/pgp-encrypted\r\n" +
                "\r\n" +
                "Version: 1\r\n" +
                "\r\n" +
                "--fake-boundary\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                body + "\r\n" +
                "\r\n" +
                "--fake-boundary--\r\n";
      }

      /// <summary>
      ///    An S/MIME detached signature: the text as written, then smime.p7s.
      ///    Sealed - the server must not rewrite it - but not opaque: the text is
      ///    in the clear and a body criterion reads it as it always has.
      /// </summary>
      private static string SMimeSignedMessage(string recipient, string body)
      {
         return "From: correspondent@example.com\r\n" +
                "To: " + recipient + "\r\n" +
                "Subject: Sealed rules\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\";\r\n" +
                " micalg=sha-256; boundary=\"signed-boundary\"\r\n" +
                "\r\n" +
                "--signed-boundary\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                body + "\r\n" +
                "\r\n" +
                "--signed-boundary\r\n" +
                "Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"smime.p7s\"\r\n" +
                "\r\n" +
                "c2lnbmF0dXJlLWJ5dGVz\r\n" +
                "\r\n" +
                "--signed-boundary--\r\n";
      }

      [Test]
      [Description("A signed message is readable, so its body criterion is evaluated as before: without the notice it is held, and no line says it declined.")]
      public void ASignedMessageIsStillReadByABodyCriterion()
      {
         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            SMimeSignedMessage(_account.Address, "No notice on this one either."));

         ImapClientSimulator.AssertMessageCount(_account.Address, Password, HeldFolderName, 1);
         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox", 0);

         StringAssert.DoesNotContain("could not be evaluated and did not match", LogHandler.ReadCurrentDefaultLog(),
            "A signed message has its text in the clear; declining to read it would switch off every body rule on an organisation's own signed mail.");
      }

      [Test]
      [Description("The rule still works: an ordinary message without the notice is held.")]
      public void AnOrdinaryMessageWithoutTheNoticeIsStillHeld()
      {
         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            PlainMessage(_account.Address, "No notice on this one."));

         ImapClientSimulator.AssertMessageCount(_account.Address, Password, HeldFolderName, 1);
         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox", 0);
      }

      [Test]
      [Description("The rule still works the other way: an ordinary message that carries the notice is left alone.")]
      public void AnOrdinaryMessageCarryingTheNoticeIsLeftAlone()
      {
         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            PlainMessage(_account.Address, Notice));

         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox", 1);
      }

      [Test]
      [Description("A sealed message is not held by a body NotContains criterion, and the log names the rule.")]
      public void ASealedMessageIsNotHeldByABodyCriterion()
      {
         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            PgpMimeMessage(_account.Address));

         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox", 1);
         ImapClientSimulator.AssertMessageCount(_account.Address, Password, HeldFolderName, 0);

         BounceTestSupport.AssertLogContains("could not be evaluated and did not match",
            "A rule that stopped applying to a message has to say so, once, naming the rule.");
         BounceTestSupport.AssertLogContains(RuleName,
            "The log line has to name the rule, or an administrator with thirty rules learns nothing from it.");
      }

      [Test]
      [Description("A multipart/encrypted label over readable text does not make a body criterion decline: the text is read and the rule fires.")]
      public void AnEncryptedLabelOverReadableTextIsStillReadByABodyCriterion()
      {
         // Against the code before the fix this fails: the label alone made the
         // criterion decline, so the message went to the Inbox and the log said
         // the body could not be evaluated.
         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            FakeEncryptedMessage(_account.Address, "No notice here, and nothing is encrypted."));

         ImapClientSimulator.AssertMessageCount(_account.Address, Password, HeldFolderName, 1);
         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox", 0);

         StringAssert.DoesNotContain("could not be evaluated and did not match", LogHandler.ReadCurrentDefaultLog(),
            "A body criterion declined on a message whose text is in the clear.");
      }

      [Test]
      [Description("A multipart/encrypted label over readable text does not hide a card number from a sensitive-content rule.")]
      public void AnEncryptedLabelDoesNotHideSensitiveContent()
      {
         // Against the code before the fix this fails: FTSensitiveContent declined
         // on the label, so the card number inside ordinary text went nowhere.
         const string cardsFolder = "INBOX.Cards";

         var rule = _application.Rules.Add();
         rule.Name = "Card numbers";
         rule.Active = true;

         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = (eRulePredefinedField) SensitiveContent;
         criteria.MatchType = eRuleMatchType.eMTContains;
         criteria.MatchValue = "card_number";
         criteria.Save();

         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRAMoveToImapFolder;
         action.IMAPFolder = cardsFolder;
         action.Save();

         rule.Save();

         // The notice is in the text, so the fixture's own rule leaves it alone
         // and only the card-number rule can move it.
         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            FakeEncryptedMessage(_account.Address, Notice + " Please charge 4111 1111 1111 1111."));

         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox.Cards", 1);
         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox", 0);
      }

      [Test]
      [Description("A criterion declining is not the whole rule declining: an anyof rule still fires on its subject.")]
      public void ASealedMessageStillMatchesTheOtherCriteriaOfTheSameRule()
      {
         // A second rule, evaluated after the first: body OR subject. The body half
         // cannot be evaluated on a sealed message and the subject half can, so the
         // rule must still fire - a fix that made any rule with a body criterion
         // skip a sealed message outright would fail here.
         //
         // The body criterion is added FIRST on purpose. An "any" rule's criteria
         // are evaluated in order and the loop stops at the first match, so with
         // the subject first the body criterion would never be reached and this
         // test would pass against a fix that returned from the whole rule as
         // soon as one criterion declined. Body first means the decline happens,
         // is logged, and the subject is still consulted afterwards.
         const string ruleName = "Either the body or the subject";

         var rule = _application.Rules.Add();
         rule.Name = ruleName;
         rule.Active = true;
         rule.UseAND = false;

         var bodyCriteria = rule.Criterias.Add();
         bodyCriteria.UsePredefined = true;
         bodyCriteria.PredefinedField = eRulePredefinedField.eFTBody;
         bodyCriteria.MatchType = eRuleMatchType.eMTContains;
         bodyCriteria.MatchValue = "nothing in this message says this";
         bodyCriteria.Save();

         var subjectCriteria = rule.Criterias.Add();
         subjectCriteria.UsePredefined = true;
         subjectCriteria.PredefinedField = eRulePredefinedField.eFTSubject;
         subjectCriteria.MatchType = eRuleMatchType.eMTContains;
         subjectCriteria.MatchValue = "Sealed rules";
         subjectCriteria.Save();

         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRAMoveToImapFolder;
         action.IMAPFolder = "INBOX.Either";
         action.Save();

         rule.Save();

         SmtpClientSimulator.StaticSendRaw("correspondent@example.com", _account.Address,
            PgpMimeMessage(_account.Address));

         ImapClientSimulator.AssertMessageCount(_account.Address, Password, "Inbox.Either", 1);

         BounceTestSupport.AssertLogContains("the body criterion of rule \"" + ruleName + "\" could not be evaluated and did not match",
            "The body criterion of the second rule was evaluated and declined, and the line has to name that rule.");
      }
   }
}
