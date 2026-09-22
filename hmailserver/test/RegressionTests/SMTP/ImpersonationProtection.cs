// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Impersonation protection (Common/Util/ImpersonationCheck.h,
   ///    docs/ImpersonationProtection.md): a borrowed name or a look-alike
   ///    domain on mail from outside, flagged or quarantined per domain.
   ///
   ///    The folding table, the two judgements and the two canonicalisers are
   ///    unit-tested in C++ against values (ImpersonationCheckTester, reached
   ///    from Infrastructure.MainOperations.TestInternals). What is proved
   ///    here is the wiring, each case with its negative control: that the
   ///    checks run at delivery on the recipient's copy and only on a message
   ///    from outside, that the header carries the reason in its two shapes
   ///    and survives a non-ASCII name, that a stranger's forged header is
   ///    removed, replaced or - with the action off - left alone, that the
   ///    quarantine holds the copy and releases it as spam is released and
   ///    flags instead when the store is off, that the default does nothing,
   ///    what is exempt at both scopes and for an alias domain, and the three
   ///    surfaces' refusals and round trips including the XML store through a
   ///    backup.
   ///
   ///    The three new COM properties are reached through dynamic, as the
   ///    restricted-delivery fixture reaches its two: the suite's interop
   ///    assembly is built from the type library registered on the machine.
   ///    The suite's own domain (example.test, remade for every test) is the
   ///    protected one. The Cyrillic spelling of it is sent in its IDNA form,
   ///    xn--exmple-4nf.test, which is what an address carries on the wire and
   ///    what the server accepts at MAIL FROM; the check decodes it. The
   ///    Cyrillic display name is sent as RFC 2047 encodes it, which is what a
   ///    header carries on the wire.
   /// </summary>
   [TestFixture]
   public class ImpersonationProtection : TestFixtureBase
   {
      // The fixture's own port, from the range this row was allocated
      // (9836-9845), so that it never contends with another fixture's listener.
      private static int RestPort = 9836;
      private const string AdminPassword = "testar";
      private const string Password = "Impersonation-Passw0rd!";
      private const string Header = "X-hMailServer-Impersonation";

      private const string Stranger = "stranger@elsewhere.test";

      // "Аdа Lоvеlасе" - Cyrillic А а о е а с е where the Latin letters stand -
      // as an RFC 2047 encoded word.
      private const string CyrillicAdaEncoded = "=?UTF-8?B?0JBk0LAgTNC+dtC1bNCw0YHQtQ==?=";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            EmptyTheQuarantine();
            IniFileSetting.Delete("QuarantineEnabled");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("With the action flag, a message from outside whose From name is an account's person name arrives with the header naming the person and the address the message came from; the same name from the account's own address over an authenticated session, from an alias of the account, and from a protected partner domain has no header; a name one letter off has none; a look-alike spelling has one; a display name that is the account's alias address is compared as an address.")]
      public void ABorrowedAccountNameIsFlaggedAndTheOwnersOwnAddressesAreNot()
      {
         Account ada = NamedAccount("ada", "Ada", "Lovelace");
         Account reader = NamedAccount("reader", "Robert", "Reader");
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "ada.l@" + _domain.Name, ada.Address);
         Configure("flag", "", "partner.example");

         Deliver(Stranger, reader.Address, "\"Ada Lovelace\" <ada.lovelace@elsewhere.test>", "Urgent");
         string flagged = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         StringAssert.Contains(Header + ": name \"Ada Lovelace\" matches Ada Lovelace (" + ada.Address + "); from ada.lovelace@elsewhere.test", Unfolded(flagged),
            "A stranger borrowing an account's name must be flagged with the reason naming the person and the real address.");
         StringAssert.Contains("Subject: Urgent", flagged, "The subject is left alone.");

         // The negative controls. The owner over an authenticated session is
         // inside, and not checked at all.
         new SmtpClientSimulator().SendRaw(ada.Address, Password, ada.Address, reader.Address,
            "From: \"Ada Lovelace\" <" + ada.Address + ">\r\nTo: " + reader.Address + "\r\nSubject: Own\r\n\r\nBody.\r\n");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "The account's own address over an authenticated session must not be flagged.");

         // From outside - the envelope is a stranger's, so the message is
         // outside and the check runs - with the name over an alias of the
         // account: the alias is one of the account's own addresses, and at a
         // hosted domain, which is exempt either way.
         Deliver(Stranger, reader.Address, "\"Ada Lovelace\" <ada.l@" + _domain.Name + ">", "Alias");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "An alias of the account carrying the account's name must not be flagged.");

         // A protected partner's domain is exempt from both checks.
         Deliver("ada@partner.example", reader.Address, "\"Ada Lovelace\" <ada@partner.example>", "Partner");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "A sender at a protected partner domain must not be flagged.");

         // A name one letter off is somebody else's name.
         Deliver(Stranger, reader.Address, "\"Ada Lovelac\" <ada.lovelace@elsewhere.test>", "Off by one");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "A name one letter off must not be flagged.");

         // And a look-alike spelling of the name is the name: the reason names
         // the spelling shown and the person it matches.
         Deliver(Stranger, reader.Address, "\"Ada L0velace\" <ada.lovelace@elsewhere.test>", "Zero for o");
         StringAssert.Contains(Header + ": name \"Ada L0velace\" matches Ada Lovelace (" + ada.Address + ")", Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A confusable spelling of the account's name must be flagged.");

         // The alias address as a display name in front of a stranger's: the
         // account's aliases are its addresses, so this is compared as one.
         Deliver(Stranger, reader.Address, "\"ada.l@" + _domain.Name + "\" <x@elsewhere.test>", "Alias as name");
         StringAssert.Contains(Header + ": name \"ada.l@" + _domain.Name + "\" matches Ada Lovelace (" + ada.Address + "); from x@elsewhere.test",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A display name that is the account's alias address must be compared as an address.");
      }

      [Test]
      [Description("A Cyrillic look-alike display name - sent as an RFC 2047 encoded word, as a header carries it - is flagged, and the reason the webmail reads through GET /api/v1/me/messages/{id} carries the name as shown beside the person it matches; the listing marks the row.")]
      public void ACyrillicDisplayNameIsFlaggedAndTheReasonSurvivesToTheReader()
      {
         Account ada = NamedAccount("ada", "Ada", "Lovelace");
         Account reader = NamedAccount("reader", "Robert", "Reader");
         Configure("flag", "", "");

         Deliver(Stranger, reader.Address, CyrillicAdaEncoded + " <ada.lovelace@elsewhere.test>", "Cyrillic");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1);

         string asReader = Basic(reader.Address, Password);
         var folders = Send("GET", "/api/v1/me/folders", asReader, null);
         Assert.AreEqual(200, folders.status, folders.body);
         string inboxId = FolderId(folders.body, "INBOX");
         Assert.IsNotEmpty(inboxId, "No INBOX in " + folders.body);

         var listing = Send("GET", "/api/v1/me/folders/" + inboxId + "/messages", asReader, null);
         Assert.AreEqual(200, listing.status, listing.body);
         StringAssert.Contains("\"impersonation\":true", listing.body, "The listing marks the stamped row.");
         Match id = Regex.Match(listing.body.Substring(listing.body.IndexOf("\"messages\":[", StringComparison.Ordinal)), "\"id\":(\\d+)");
         Assert.IsTrue(id.Success, "No message id in " + listing.body);

         var message = Send("GET", "/api/v1/me/messages/" + id.Groups[1].Value, asReader, null);
         Assert.AreEqual(200, message.status, message.body);
         StringAssert.Contains("matches Ada Lovelace (" + ada.Address + "); from ada.lovelace@elsewhere.test", message.body,
            "The decoded reason must name the person and the real address.");
         StringAssert.DoesNotContain("name \\\"?d?", message.body, "The shown name must not have been reduced to question marks.");
         StringAssert.Contains("\"impersonation\":\"name \\\"", message.body, "The reason starts with the shown name.");

         string raw = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         StringAssert.Contains(Header + ":", raw, "The header is on the copy.");
      }

      [Test]
      [Description("A protected person's name borrowed from outside is flagged with the reason naming the person; the same name from the address given for the person in the list is not; an address-shaped display name that is an account's address is compared as an address, and a nameless account is named by its address.")]
      public void AProtectedPersonsNameIsFlaggedAndTheirOwnAddressIsNot()
      {
         Account reader = NamedAccount("reader", "Robert", "Reader");
         Account ceo = NamedAccount("ceo", "", "");
         Configure("flag", "The Chairman <chair@board.example>, The Auditor", "");

         Deliver(Stranger, reader.Address, "\"The Chairman\" <chairman@elsewhere.test>", "Wire transfer");
         StringAssert.Contains(Header + ": name \"The Chairman\" matches The Chairman (chair@board.example); from chairman@elsewhere.test",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A protected person's name from another address must be flagged.");

         Deliver("chair@board.example", reader.Address, "\"The Chairman\" <chair@board.example>", "Board papers");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "The protected person writing from the address given for them must not be flagged.");

         Deliver(Stranger, reader.Address, "\"the auditor\" <audit@elsewhere.test>", "Findings");
         StringAssert.Contains(Header + ": name \"the auditor\" matches The Auditor (a protected person); from audit@elsewhere.test",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A protected name with no address must be flagged from any outside address, whatever its case.");

         // A display name that is an account's address, in front of a stranger's.
         Deliver(Stranger, reader.Address, "\"" + ceo.Address + "\" <x@elsewhere.test>", "Address as name");
         StringAssert.Contains(Header + ": name \"" + ceo.Address + "\" matches the address (" + ceo.Address + "); from x@elsewhere.test",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A display name that is an account's address must be compared as an address.");
      }

      [Test]
      [Description("Look-alike domains - a 1 for an l, rn for m, a transposition, an insertion, the IDNA spelling with a Cyrillic a, and a look-alike with a label in front of it - are flagged against the hosted domain, naming both; a subdomain of it and the domain itself are not; a look-alike of a protected partner is flagged and the partner itself is not.")]
      public void LookalikeDomainsAreFlaggedAndOurOwnNamesAreNot()
      {
         Account reader = NamedAccount("reader", "Robert", "Reader");
         Configure("flag", "", "partner.example");

         foreach (string lookalike in new[] { "examp1e.test", "exarnple.test", "exampel.test", "examplee.test", "xn--exmple-4nf.test" })
         {
            Deliver("invoices@" + lookalike, reader.Address, "Invoices <invoices@" + lookalike + ">", "Invoice from " + lookalike);
            StringAssert.Contains(Header + ": domain " + lookalike + " looks like " + _domain.Name + "; from invoices@" + lookalike,
               Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
               lookalike + " must be flagged as a look-alike of " + _domain.Name + ".");
         }

         // The shape a bought look-alike is used in: a label in front of it.
         Deliver("invoices@mail.examp1e.test", reader.Address, "Invoices <invoices@mail.examp1e.test>", "Beneath a look-alike");
         StringAssert.Contains(Header + ": domain examp1e.test looks like " + _domain.Name + "; from invoices@mail.examp1e.test",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A name beneath a look-alike must be flagged by the look-alike inside it.");

         Deliver("invoices@mail." + _domain.Name, reader.Address, "Invoices <invoices@mail." + _domain.Name + ">", "Beneath");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "A name beneath the hosted domain is the domain's own and must not be flagged.");

         // The domain itself, from outside: a forged internal From on an
         // outside envelope is outside, and the sender domain is exactly
         // hosted, which exempts it - that is what DMARC is for.
         Deliver(Stranger, reader.Address, "Invoices <invoices@" + _domain.Name + ">", "Forged");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "The hosted domain itself must not be flagged as a look-alike of itself.");

         Deliver("friend@partner.exarnple", reader.Address, "Friend <friend@partner.exarnple>", "Partner look-alike");
         StringAssert.Contains(Header + ": domain partner.exarnple looks like partner.example; from friend@partner.exarnple",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A look-alike of a protected partner must be flagged.");

         Deliver("friend@partner.example", reader.Address, "Friend <friend@partner.example>", "Partner");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "The partner itself must not be flagged.");

         Deliver(Stranger, reader.Address, "Someone <someone@elsewhere.test>", "Unrelated");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "An unrelated domain must not be flagged.");
      }

      [Test]
      [Description("An alias domain is a hosted name: a sender at it from outside is exempt, and a look-alike of the alias name is flagged naming the alias.")]
      public void AnAliasDomainIsAHostedName()
      {
         Account reader = NamedAccount("reader", "Robert", "Reader");
         Configure("flag", "", "");

         DomainAlias alias = _domain.DomainAliases.Add();
         alias.DomainID = _domain.ID;
         alias.AliasName = "example-alias.test";
         alias.Save();

         try
         {
            // The control: without the alias this spelling is a stranger's.
            Deliver(Stranger, reader.Address, "Someone <someone@example-alias.test>", "At the alias");
            StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
               "A sender at an alias domain is at a hosted name and must not be flagged.");

            Deliver("invoices@examp1e-alias.test", reader.Address, "Invoices <invoices@examp1e-alias.test>", "Alias look-alike");
            StringAssert.Contains(Header + ": domain examp1e-alias.test looks like example-alias.test; from invoices@examp1e-alias.test",
               Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
               "A look-alike of an alias domain must be flagged naming the alias.");
         }
         finally
         {
            _domain.DomainAliases.DeleteByDBID(alias.ID);
         }
      }

      [Test]
      [Description("A stranger's own X-hMailServer-Impersonation header is removed from a message the check finds nothing to say about, replaced by the real reason where it does, and left alone with the action off.")]
      public void AForgedHeaderIsRemovedReplacedOrLeftAlone()
      {
         Account ada = NamedAccount("ada", "Ada", "Lovelace");
         Account reader = NamedAccount("reader", "Robert", "Reader");
         const string forged = Header + ": name \"Nobody\" matches Nobody (nobody@example.test); from forged@elsewhere.test";

         Configure("flag", "", "");

         Deliver(Stranger, reader.Address, "Someone <someone@elsewhere.test>", "Nothing to say", forged);
         string removed = Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password));
         StringAssert.DoesNotContain(Header, removed, "A forged warning on a clean message must be removed.");

         Deliver(Stranger, reader.Address, "\"Ada Lovelace\" <ada.lovelace@elsewhere.test>", "Replaced", forged);
         string replaced = Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password));
         StringAssert.Contains(Header + ": name \"Ada Lovelace\" matches Ada Lovelace (" + ada.Address + ")", replaced, "The real reason must be written.");
         StringAssert.DoesNotContain("Nobody", replaced, "The forged text must be gone.");
         ClassicAssert.AreEqual(1, Occurrences(replaced, Header + ":"), "Exactly one header, the server's.");

         Configure("off", "", "");

         Deliver(Stranger, reader.Address, "\"Ada Lovelace\" <ada.lovelace@elsewhere.test>", "Left alone", forged);
         string untouched = Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password));
         StringAssert.Contains("Nobody", untouched, "With the action off nothing is read, so a forged header is left as it arrived.");
      }

      [Test]
      [Description("With the action quarantine, the message is in the quarantine with the reason and absent from the mailbox, the digest lists it, and the administrator's release delivers it to the mailbox; a clean message from the same stranger is delivered.")]
      public void QuarantineHoldsTheCopyAndTheReleaseDeliversIt()
      {
         Account ada = NamedAccount("ada", "Ada", "Lovelace");
         Account reader = NamedAccount("reader", "Robert", "Reader");

         IniFileSetting.Write("QuarantineEnabled", "1");
         _application.Reinitialize();
         EmptyTheQuarantine();

         dynamic settings = _settings;
         settings.QuarantineDigestBaseUrl = "http://127.0.0.1:" + RestPort;
         dynamic domainSettings = _domain;
         domainSettings.QuarantineDigest = true;
         _domain.Save();

         try
         {
            Configure("quarantine", "", "");

            Deliver(Stranger, reader.Address, "\"Ada Lovelace\" <ada.lovelace@elsewhere.test>", "Held");
            Deliver(Stranger, reader.Address, "Someone <someone@elsewhere.test>", "Clean");

            // The clean message arrives; the held one does not.
            Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1);
            StringAssert.Contains("Subject: Clean", Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password));

            var quarantine = _settings.AntiSpam.Quarantine;
            quarantine.Refresh();
            ClassicAssert.AreEqual(1, quarantine.Count, "The borrowed name must be in the quarantine.");
            StringAssert.Contains("name \"Ada Lovelace\" matches Ada Lovelace (" + ada.Address + ")", quarantine[0].Reason, "The row's reason is the header's text.");
            StringAssert.Contains(reader.Address, quarantine[0].Recipients);

            // The digest lists it.
            dynamic antiSpam = _settings.AntiSpam;
            ClassicAssert.AreEqual(1, (int) antiSpam.SendQuarantineDigests(), "One account has something held; one digest.");
            string digest = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
            StringAssert.Contains("Subject: 1 message held for you", digest);
            StringAssert.Contains("Held", digest, "The digest names the held message.");

            // The administrator's release delivers it, unchecked.
            quarantine[0].ReleaseMessage();
            Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1);
            string released = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
            StringAssert.Contains("Subject: Held", released, "The released message must reach the mailbox.");
            StringAssert.DoesNotContain(Header, released, "A release delivers without re-checking, so the copy carries no flag.");

            quarantine.Refresh();
            ClassicAssert.AreEqual(0, quarantine.Count, "A released message is no longer held.");
         }
         finally
         {
            settings.QuarantineDigestBaseUrl = "";
            domainSettings.QuarantineDigest = false;
            _domain.Save();
         }
      }

      [Test]
      [Description("With the action quarantine and the quarantine store off, the copy is flagged and delivered instead: a message stored nowhere is not discarded.")]
      public void QuarantineWithTheStoreOffFlagsInstead()
      {
         Account ada = NamedAccount("ada", "Ada", "Lovelace");
         Account reader = NamedAccount("reader", "Robert", "Reader");

         IniFileSetting.Delete("QuarantineEnabled");
         _application.Reinitialize();

         Configure("quarantine", "", "");

         Deliver(Stranger, reader.Address, "\"Ada Lovelace\" <ada.lovelace@elsewhere.test>", "Store off");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1);
         StringAssert.Contains(Header + ": name \"Ada Lovelace\" matches Ada Lovelace (" + ada.Address + ")",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "With the store off a quarantine action must flag the copy and deliver it.");
      }

      [Test]
      [Description("With the action off - the default - nothing is stamped and nothing is held, for a borrowed account name, a protected person's name and a look-alike domain alike.")]
      public void TheDefaultDoesNothing()
      {
         Account ada = NamedAccount("ada", "Ada", "Lovelace");
         Account reader = NamedAccount("reader", "Robert", "Reader");

         dynamic domain = _domain;
         ClassicAssert.AreEqual(0, (int) domain.ImpersonationAction, "Off by default.");
         ClassicAssert.AreEqual("", (string) domain.ProtectedPersons, "No persons by default.");
         ClassicAssert.AreEqual("", (string) domain.ProtectedPartners, "No partners by default.");

         // The lists set, the action left off: the lists alone do nothing.
         Configure("off", "The Chairman", "partner.example");

         Deliver(Stranger, reader.Address, "\"Ada Lovelace\" <ada.lovelace@elsewhere.test>", "Name");
         Deliver(Stranger, reader.Address, "\"The Chairman\" <chairman@elsewhere.test>", "Person");
         Deliver("invoices@examp1e.test", reader.Address, "Invoices <invoices@examp1e.test>", "Domain");
         Deliver("friend@partner.exarnple", reader.Address, "Friend <friend@partner.exarnple>", "Partner");

         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 4);
         for (int i = 0; i < 4; i++)
            StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password), "Nothing is stamped while the action is off.");
      }

      [Test]
      [Description("A safe-sender entry of the recipient for the look-alike domain exempts it, and so does the domain's allow list; the envelope-sender look-alike with a clean From is flagged, naming the envelope.")]
      public void TheSafeSenderIsExemptAtBothScopesAndTheEnvelopeIsJudged()
      {
         Account reader = NamedAccount("reader", "Robert", "Reader");
         Configure("flag", "", "");

         // The control: the look-alike is flagged before anybody vouches.
         Deliver("invoices@examp1e.test", reader.Address, "Invoices <invoices@examp1e.test>", "Before");
         StringAssert.Contains(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password));

         var safe = Send("POST", "/api/v1/me/sender-list", Basic(reader.Address, Password), "{\"value\":\"examp1e.test\",\"reason\":\"our supplier really is called that\"}");
         Assert.AreEqual(201, safe.status, safe.body);

         Deliver("invoices@examp1e.test", reader.Address, "Invoices <invoices@examp1e.test>", "After");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "A domain the reader marked safe must not be flagged.");

         // The domain's allow list, edited by an administrator over COM as the
         // Control Panel does, exempts another look-alike for every reader.
         Deliver("invoices@exampel.test", reader.Address, "Invoices <invoices@exampel.test>", "Before the domain vouches");
         StringAssert.Contains(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password));

         dynamic domainList = ((dynamic) _domain).SenderList;
         dynamic entry = domainList.Add();
         entry.Value = "exampel.test";
         entry.Blocked = false;
         entry.Reason = "a supplier of the whole domain";
         entry.Save();

         Deliver("invoices@exampel.test", reader.Address, "Invoices <invoices@exampel.test>", "After the domain vouches");
         StringAssert.DoesNotContain(Header, Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password),
            "A domain the domain's allow list vouches for must not be flagged.");

         // The envelope: a clean From over a look-alike return path.
         Deliver("bounce@examplee.test", reader.Address, "Someone <someone@elsewhere.test>", "Envelope");
         StringAssert.Contains(Header + ": domain examplee.test looks like " + _domain.Name + "; envelope sender bounce@examplee.test",
            Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)),
            "A look-alike envelope sender under a clean From must be flagged, naming the envelope.");
      }

      [Test]
      [Description("Over REST: a list of 2,001 characters is 400, a partner that is an address is 400 with a sentence, an unbalanced quote is 400 with a sentence, an action of 3 is 400, GET answers what PUT wrote canonicalised - a quoted name with a comma kept whole - and the action round-trips as digits and as words.")]
      public void TheRestRefusalsAndRoundTrips()
      {
         string domainPath = "/api/v1/domains/" + _domain.Name;

         var tooLong = Admin("PUT", domainPath, "{\"active\":true,\"protected_persons\":\"" + new string('a', 2001) + "\"}");
         Assert.AreEqual(400, tooLong.status, tooLong.body);
         StringAssert.Contains("2000", tooLong.body);

         var partnersTooLong = Admin("PUT", domainPath, "{\"active\":true,\"protected_partners\":\"" + new string('a', 2001) + "\"}");
         Assert.AreEqual(400, partnersTooLong.status, partnersTooLong.body);

         var address = Admin("PUT", domainPath, "{\"active\":true,\"protected_partners\":\"partner.example, someone@partner.example\"}");
         Assert.AreEqual(400, address.status, address.body);
         StringAssert.Contains("names domains, not addresses", address.body);

         var suffix = Admin("PUT", domainPath, "{\"active\":true,\"protected_partners\":\".example\"}");
         Assert.AreEqual(400, suffix.status, suffix.body);

         var badPerson = Admin("PUT", domainPath, "{\"active\":true,\"protected_persons\":\"Ada <not an address>\"}");
         Assert.AreEqual(400, badPerson.status, badPerson.body);
         StringAssert.Contains("not an address", badPerson.body);

         var unbalanced = Admin("PUT", domainPath, "{\"active\":true,\"protected_persons\":\"\\\"Holloway, Chris <chris@board.example>\"}");
         Assert.AreEqual(400, unbalanced.status, unbalanced.body);
         StringAssert.Contains("unbalanced quote", unbalanced.body);

         var fourth = Admin("PUT", domainPath, "{\"active\":true,\"impersonation_action\":3}");
         Assert.AreEqual(400, fourth.status, fourth.body);
         StringAssert.Contains("impersonation_action must be 0", fourth.body);
         Assert.AreEqual(400, Admin("PUT", domainPath, "{\"active\":true,\"impersonation_action\":\"sometimes\"}").status);
         Assert.AreEqual(400, Admin("PUT", domainPath, "{\"active\":true,\"impersonation_action\":1.5}").status);
         Assert.AreEqual(400, Admin("PUT", domainPath, "{\"active\":true,\"impersonation_action\":true}").status);

         var written = Admin("PUT", domainPath,
            "{\"active\":true,\"impersonation_action\":\"quarantine\",\"protected_partners\":\"Partner.Example  other.example,partner.example\",\"protected_persons\":\"The Chairman, chair@Board.Example\\n\\\"Ada\\\" <ADA@Example.Test>, \\\"Holloway, Chris\\\" <chris@board.example>\"}");
         Assert.AreEqual(200, written.status, written.body);
         StringAssert.Contains("\"impersonation_action\":2", written.body);
         StringAssert.Contains("\"protected_partners\":\"partner.example, other.example\"", written.body, "Lower-cased, canonical separators, duplicates dropped.");
         StringAssert.Contains("\"protected_persons\":\"The Chairman\\nchair@board.example\\nAda <ada@example.test>\\n\\\"Holloway, Chris\\\" <chris@board.example>\"", written.body,
            "One per line, addresses lower-cased, a name with a comma kept whole in quotes.");

         string read = Admin("GET", domainPath, null).body;
         StringAssert.Contains("\"impersonation_action\":2", read, "GET answers what PUT wrote.");
         StringAssert.Contains("\"protected_partners\":\"partner.example, other.example\"", read);

         // A PUT of another field leaves them alone.
         Assert.AreEqual(200, Admin("PUT", domainPath, "{\"active\":true,\"postmaster\":\"postmaster@" + _domain.Name + "\"}").status);
         StringAssert.Contains("\"impersonation_action\":2", Admin("GET", domainPath, null).body);

         Assert.AreEqual(200, Admin("PUT", domainPath, "{\"active\":true,\"impersonation_action\":\"1\"}").status);
         StringAssert.Contains("\"impersonation_action\":1", Admin("GET", domainPath, null).body);
         Assert.AreEqual(200, Admin("PUT", domainPath, "{\"active\":true,\"impersonation_action\":\"off\",\"protected_partners\":\"\",\"protected_persons\":\"\"}").status);
         string cleared = Admin("GET", domainPath, null).body;
         StringAssert.Contains("\"impersonation_action\":0", cleared);
         StringAssert.Contains("\"protected_partners\":\"\"", cleared, "Empty clears it.");
         StringAssert.Contains("\"protected_persons\":\"\"", cleared, "Empty clears it.");
      }

      [Test]
      [Description("Over COM: Domain.ImpersonationAction, ProtectedPersons and ProtectedPartners are read, written and kept by Save(), canonical; an action of 3, a partner that is an address and a person whose address is not one are refused by the property.")]
      public void TheThreeComPropertiesRoundTrip()
      {
         dynamic domain = _domain;

         domain.ImpersonationAction = 1;
         domain.ProtectedPersons = "The Chairman, chair@Board.Example";
         domain.ProtectedPartners = "Partner.Example, other.example";
         _domain.Save();

         dynamic reread = _application.Domains.get_ItemByName(_domain.Name);
         ClassicAssert.AreEqual(1, (int) reread.ImpersonationAction);
         ClassicAssert.AreEqual("The Chairman\nchair@board.example", (string) reread.ProtectedPersons, "Canonical after Save.");
         ClassicAssert.AreEqual("partner.example, other.example", (string) reread.ProtectedPartners, "Lower-cased and canonical after Save.");

         var refusedAction = Assert.Throws<COMException>(() => { domain.ImpersonationAction = 3; });
         StringAssert.Contains("ImpersonationAction must be 0", refusedAction.Message);

         var refusedPartner = Assert.Throws<COMException>(() => { domain.ProtectedPartners = "someone@partner.example"; });
         StringAssert.Contains("names domains, not addresses", refusedPartner.Message);

         var refusedPerson = Assert.Throws<COMException>(() => { domain.ProtectedPersons = "Ada <not an address>"; });
         StringAssert.Contains("not an address", refusedPerson.Message);

         // What COM wrote is what the check reads.
         Account reader = NamedAccount("reader", "Robert", "Reader");
         Deliver(Stranger, reader.Address, "\"The Chairman\" <chairman@elsewhere.test>", "Wire transfer");
         StringAssert.Contains(Header + ": name \"The Chairman\" matches The Chairman (a protected person)", Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password)));
      }

      [Test]
      [Description("The domain's XML store: the action and both lists survive a backup and a restore that deleted the domain, and come back canonical.")]
      public void TheXmlStoreRoundTripsThroughABackup()
      {
         Configure("quarantine", "The Chairman <chair@board.example>, The Auditor", "partner.example, other.example");

         string backupDirectory = Path.Combine(Path.GetTempPath(), TestSetup.UniqueString());
         Directory.CreateDirectory(backupDirectory);

         try
         {
            RunBackup(backupDirectory);
            DeleteAndRestore(backupDirectory);

            dynamic restored = _application.Domains.get_ItemByName(_domain.Name);
            ClassicAssert.AreEqual(2, (int) restored.ImpersonationAction, "The action did not come back.");
            ClassicAssert.AreEqual("The Chairman <chair@board.example>\nThe Auditor", (string) restored.ProtectedPersons, "The persons did not come back.");
            ClassicAssert.AreEqual("partner.example, other.example", (string) restored.ProtectedPartners, "The partners did not come back.");
         }
         finally
         {
            try { Directory.Delete(backupDirectory, true); } catch (Exception cleaning) when (!ExceptionPolicy.IsFatal(cleaning)) { }
         }
      }

      // ------------------------------------------------------------ the wires

      private Account NamedAccount(string localPart, string firstName, string lastName)
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, localPart + "@" + _domain.Name, Password);
         account.PersonFirstName = firstName;
         account.PersonLastName = lastName;
         account.Save();
         return account;
      }

      /// <summary>
      ///    The domain's three settings, over REST as the Deck writes them.
      /// </summary>
      private void Configure(string action, string persons, string partners)
      {
         var answer = Admin("PUT", "/api/v1/domains/" + _domain.Name,
            "{\"active\":true,\"impersonation_action\":\"" + action + "\",\"protected_persons\":\"" + persons.Replace("\n", "\\n") + "\",\"protected_partners\":\"" + partners + "\"}");
         Assert.AreEqual(200, answer.status, "The domain's settings were refused: " + answer.body);
      }

      /// <summary>
      ///    One message from outside - an unauthenticated session, the envelope
      ///    sender given - with the From header as written and, when given, one
      ///    more header line the sender wrote.
      /// </summary>
      private static void Deliver(string envelopeSender, string to, string fromHeader, string subject, string extraHeader = null)
      {
         new SmtpClientSimulator().SendRaw(envelopeSender, to,
            "From: " + fromHeader + "\r\nTo: " + to + "\r\nSubject: " + subject + "\r\n" + (extraHeader == null ? "" : extraHeader + "\r\n") + "\r\nBody.\r\n");
      }

      /// <summary>A message text with its folded header lines joined, so a header value can be looked for whole.</summary>
      private static string Unfolded(string message)
      {
         return message.Replace("\r\n ", " ").Replace("\r\n\t", " ");
      }

      private static int Occurrences(string haystack, string needle)
      {
         int count = 0;

         for (int at = haystack.IndexOf(needle, StringComparison.Ordinal); at >= 0;
              at = haystack.IndexOf(needle, at + needle.Length, StringComparison.Ordinal))
            count++;

         return count;
      }

      /// <summary>The id of the folder of that name in the folders document, or empty.</summary>
      private static string FolderId(string foldersJson, string name)
      {
         foreach (Match folder in Regex.Matches(foldersJson, "\\{[^{}]*\\}"))
         {
            if (folder.Value.IndexOf("\"name\":\"" + name + "\"", StringComparison.Ordinal) < 0)
               continue;

            Match id = Regex.Match(folder.Value, "\"id\":(\\d+)");
            if (id.Success)
               return id.Groups[1].Value;
         }

         return "";
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      private void RunBackup(string backupDirectory)
      {
         var settings = _application.Settings.Backup;
         settings.BackupDomains = true;
         settings.BackupMessages = false;
         settings.BackupSettings = false;
         settings.CompressDestinationFiles = true;
         settings.Destination = backupDirectory;

         CustomAsserts.AssertDeleteFile(settings.LogFile);

         _application.BackupManager.StartBackup();

         for (int attempt = 0; attempt < 120; attempt++)
         {
            try
            {
               string log = TestSetup.ReadExistingTextFile(settings.LogFile);

               if (log.Contains("BACKUP ERROR:"))
                  Assert.Fail("The backup failed. Log: " + log);

               if (log.Contains("Backup completed successfully"))
                  return;
            }
            catch (Exception sharing) when (!ExceptionPolicy.IsFatal(sharing))
            {
               // The server is writing the file; read it again in a moment.
            }

            Thread.Sleep(250);
         }

         Assert.Fail("The backup did not finish within 30 seconds.");
      }

      private void DeleteAndRestore(string backupDirectory)
      {
         // The disaster, exactly as a restore assumes it: every domain gone.
         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         string startTime = _application.Status.StartTime;

         // Status.StartTime has one-second resolution; step past the second the
         // last reinitialisation fell in, or the wait below sees no change.
         Thread.Sleep(1100);

         FileInfo[] archives = new DirectoryInfo(backupDirectory).GetFiles("*.7z");
         Assert.AreEqual(1, archives.Length, "Expected exactly one archive in " + backupDirectory);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = true;
         backup.RestoreMessages = false;
         backup.RestoreSettings = false;
         backup.StartRestore();

         for (int attempt = 0; attempt < 600; attempt++)
         {
            try
            {
               string now = _application.Status.StartTime;

               if (now.Length > 0 && now != startTime)
               {
                  Thread.Sleep(500);
                  return;
               }
            }
            catch (Exception talking) when (!ExceptionPolicy.IsFatal(talking))
            {
               // The COM server is reinitialising.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not finish within a minute.");
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);
      }
   }
}
