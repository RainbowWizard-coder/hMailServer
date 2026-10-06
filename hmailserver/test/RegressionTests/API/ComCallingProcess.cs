// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A Message or a FetchAccount a program creates itself - Set m =
   ///    CreateObject("hMailServer.Message") - holds no credential of its own. Since 6.3.6 it
   ///    answers with the credential of an Application its COM client process holds and has
   ///    signed in on (the most privileged, where it holds several), and with none once that
   ///    Application is released or a later Authenticate on it fails; with none, it is refused
   ///    what needs a credential (ComWithoutCredential). And mail a credential queues that is
   ///    not the server administrator's claims to be from only an address that credential may
   ///    send as: an account's own, an alias of it, one granted or verified - the rule
   ///    authenticated SMTP applies - and for a domain's administrator any address of the
   ///    domain.
   ///
   ///    The scripts run as cscript.exe processes of their own, as a scheduled VBScript does:
   ///    this test process holds the suite's signed-in Application, so a message it created
   ///    itself would answer with the suite's administrator credential.
   ///
   ///    Windows only - it is COM - and so linked into RegressionTests alone.
   /// </summary>
   [TestFixture]
   public class ComCallingProcess : TestFixtureBase
   {
      private const string AdministratorPassword = "testar";
      private const string SenderPassword = "Sender-Passw0rd!";

      private string _scratch;

      [SetUp]
      public void CreateScratchFolder()
      {
         _scratch = Path.Combine(Path.GetTempPath(), "hm-com-calling-process-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(_scratch);
      }

      [TearDown]
      public void RemoveScratchFolder()
      {
         try
         {
            Directory.Delete(_scratch, true);
         }
         catch (IOException)
         {
         }
         catch (UnauthorizedAccessException)
         {
         }

         // A domain a test here added is cleared by the next test's setup; after the
         // last test of a run there is none, and the bench's pre-flight counts one.
         try
         {
            var domains = SingletonProvider<TestSetup>.Instance.GetApp().Domains;
            for (var i = domains.Count - 1; i >= 0; i--)
            {
               if (!string.Equals(domains[i].Name, "example.test", StringComparison.OrdinalIgnoreCase))
                  domains[i].Delete();
            }
         }
         catch (COMException)
         {
         }
      }

      private static void RequireAnAdministratorPassword()
      {
         object administrator = new Application().Authenticate("Administrator", "");
         if (administrator != null)
            Assert.Ignore("This server has no administrator password, so COM answers anonymously as the " +
                          "server administrator and there is no holder without a credential to refuse.");

         if (new Application().Authenticate("Administrator", AdministratorPassword) == null)
            Assert.Ignore("The scripts sign in with the suite's administrator password, which this server does not take.");
      }

      private string WriteFile(string name, string content)
      {
         var path = Path.Combine(_scratch, name);
         File.WriteAllText(path, content);
         return path;
      }

      private static string[] SubjectsIn(Account account)
      {
         var messages = account.IMAPFolders.get_ItemByName("INBOX").Messages;
         var subjects = new List<string>();
         for (var i = 0; i < messages.Count; i++)
            subjects.Add(messages[i].Subject);
         return subjects.ToArray();
      }

      // Every message queued before a control sent over SMTP afterwards has been delivered
      // once the control has, and what the account then holds is what was queued.
      private static string[] SubjectsAfterAControl(Account account, string password, int expected)
      {
         SmtpClientSimulator.StaticSend("someone@example.test", account.Address, "Control", "Control");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         CustomAsserts.AssertFolderMessageCount(account.IMAPFolders.get_ItemByName("INBOX"), expected);
         return SubjectsIn(account);
      }

      private static void AssertRefused(string printed, string what)
      {
         StringAssert.Contains("REFUSED", printed, what + " was not refused: " + printed);
         StringAssert.Contains("You do not have access", printed, what + " failed, but not as refused: " + printed);
      }

      // As AssertRefused, collected: a server that answers one step wrongly is asked
      // every other step too, and the test reports them all.
      private static void Refused(List<string> problems, string printed, string what)
      {
         if (!printed.Contains("REFUSED"))
            problems.Add(what + " was not refused: " + printed);
         else if (!printed.Contains("You do not have access"))
            problems.Add(what + " failed, but not as refused: " + printed);
      }

      [Test]
      [Description("A program that signs in as the administrator and then creates the message it sends itself - " +
                   "CreateObject(\"hMailServer.Message\"), Attachments.Add, Save - sends it, file and all, as scripts " +
                   "did before 6.3.6.")]
      public void AProgramThatSignedInSendsAMessageItCreatedItselfWithAFile()
      {
         RequireAnAdministratorPassword();

         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "recipient@example.test", "test");
         var source = WriteFile("signed-in.txt", "attached by a program that signed in");

         var printed = ComScripts.SendDirectly(_scratch, "Administrator", AdministratorPassword, "someone@example.test", "-",
            recipient.Address, "Signed in first", source);
         ClassicAssert.AreEqual("SAVED", printed);

         var delivered = CustomAsserts.AssertRetrieveFirstMessage(recipient.IMAPFolders.get_ItemByName("INBOX"));
         ClassicAssert.AreEqual("Signed in first", delivered.Subject);
         ClassicAssert.AreEqual(1, delivered.Attachments.Count);

         var written = Path.Combine(_scratch, "written.txt");
         delivered.Attachments[0].SaveAs(written);
         ClassicAssert.AreEqual("attached by a program that signed in", File.ReadAllText(written));
      }

      [Test]
      [Description("A program that signs in as the administrator and then creates a fetch account itself may point " +
                   "it at an account and save it.")]
      public void AProgramThatSignedInSavesAFetchAccountItCreatedItself()
      {
         RequireAnAdministratorPassword();

         var target = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "target@example.test", "test");

         var printed = ComScripts.FetchAccountDirectly(_scratch, "Administrator", AdministratorPassword, target.ID);
         ClassicAssert.AreEqual("ACCOUNTID-TAKEN SAVED", printed);
         ClassicAssert.AreEqual(1, target.FetchAccounts.Count, "The fetch account was not stored on the account.");
      }

      [Test]
      [Description("While one program holds an Application signed in as the administrator, another that has not " +
                   "signed in is refused the message it creates itself; once the first lets its Application go, the " +
                   "first is refused one too, and so is the second again.")]
      public void ASignInIsTheHoldingProcesssAloneAndEndsWithItsApplication()
      {
         RequireAnAdministratorPassword();

         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "holding@example.test", "test");
         var problems = new List<string>();

         var printed = ComScripts.HoldASignIn(_scratch, AdministratorPassword, recipient.Address, holder =>
         {
            Refused(problems, ComScripts.SendDirectly(_scratch, "-", "-", "someone@example.test", "-", recipient.Address,
               "Not signed in, while another holds", "-"), "A second program's Save while the first holds a sign-in");

            holder.Release();

            Refused(problems, ComScripts.SendDirectly(_scratch, "-", "-", "someone@example.test", "-", recipient.Address,
               "Not signed in, after the other let go", "-"), "A second program's Save after the first let go");
         });

         if (!printed.Contains("HOLDING SAVED"))
            problems.Add("The holding program's own message was not saved: " + printed);
         if (!printed.Contains("RELEASED SAVE-REFUSED") || !printed.Contains("You do not have access"))
            problems.Add("The holding program's message was not refused after it had let its Application go: " + printed);

         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         CollectionAssert.AreEquivalent(new[] { "Saved while holding", "Control" },
            SubjectsAfterAControl(recipient, "test", 2));
      }

      [Test]
      [Description("An Application whose later Authenticate fails no longer lends its credential to the messages " +
                   "its process creates itself.")]
      public void ALaterFailedAuthenticateEndsTheSignIn()
      {
         RequireAnAdministratorPassword();

         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "failed@example.test", "test");

         var printed = ComScripts.SignInThenFail(_scratch, AdministratorPassword, recipient.Address);

         StringAssert.Contains("BEFORE SAVED", printed);
         StringAssert.Contains("AFTER SAVE-REFUSED", printed, "A message was saved after the Application's Authenticate failed.");

         CollectionAssert.AreEquivalent(new[] { "Before the failed sign-in", "Control" },
            SubjectsAfterAControl(recipient, "test", 2));
      }

      [Test]
      [Description("Where a program holds two Applications signed in, its own messages answer with the more " +
                   "privileged: an account's and the administrator's sign the administrator's, and once that one is " +
                   "let go the account's, which may not attach a file from the server's disk or send as another.")]
      public void TheMostPrivilegedSignInOfAProcessIsTheOneItsMessagesTake()
      {
         RequireAnAdministratorPassword();

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "two@example.test", SenderPassword);
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "both@example.test", "test");
         var source = WriteFile("both.txt", "attached while the administrator was signed in too");

         var printed = ComScripts.TwoSignIns(_scratch, sender.Address, SenderPassword, AdministratorPassword,
            recipient.Address, source);

         StringAssert.Contains("BOTH SAVED", printed, "With the administrator signed in too, the message was refused.");
         StringAssert.Contains("ACCOUNT-ALONE-FILE ADD-REFUSED", printed,
            "With the administrator let go, the account's sign-in attached a file from the server's disk.");
         StringAssert.Contains("ACCOUNT-ALONE-FOREIGN SAVE-REFUSED", printed,
            "With the administrator let go, the account's sign-in sent as another address.");
         StringAssert.Contains("ACCOUNT-ALONE-OWN SAVED", printed, "The account's sign-in could not send as itself.");

         CollectionAssert.AreEquivalent(new[] { "Both signed in", "The account alone, as itself", "Control" },
            SubjectsAfterAControl(recipient, "test", 3));
      }

      [Test]
      [Description("A program signed in as an account sends the message it creates itself as the account, and is " +
                   "refused one that claims to be from another address.")]
      public void AnAccountsProgramSendsAsItselfAndNotAsAnother()
      {
         RequireAnAdministratorPassword();

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sender@example.test", SenderPassword);
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@example.test", "test");

         AssertRefused(ComScripts.SendDirectly(_scratch, sender.Address, SenderPassword, "ceo@elsewhere.test", "-",
            recipient.Address, "As another, envelope", "-"), "An envelope sender of another address");
         AssertRefused(ComScripts.SendDirectly(_scratch, sender.Address, SenderPassword, sender.Address,
            "CEO <ceo@elsewhere.test>", recipient.Address, "As another, From field", "-"), "A From field of another address");

         ClassicAssert.AreEqual("SAVED", ComScripts.SendDirectly(_scratch, sender.Address, SenderPassword, sender.Address,
            "Sender <" + sender.Address + ">", recipient.Address, "As itself", "-"));

         CollectionAssert.AreEquivalent(new[] { "As itself", "Control" }, SubjectsAfterAControl(recipient, "test", 2));
      }

      // The message an Application signed in as the address creates (Application.CreateMessage,
      // reached late-bound: the interop assembly is made from the registered type library).
      private static hMailServer.Message CreateMessageAs(string address, string password)
      {
         dynamic application = new Application();
         ClassicAssert.IsNotNull(application.Authenticate(address, password), address + " could not authenticate.");
         return (hMailServer.Message) application.CreateMessage();
      }

      private static hMailServer.Message Compose(string address, string password, string envelope, string from, string recipient, string subject)
      {
         var message = CreateMessageAs(address, password);
         message.FromAddress = envelope;
         if (from != null)
            message.From = from;
         message.AddRecipient("", recipient);
         message.Subject = subject;
         message.Body = "Body";
         return message;
      }

      private static void TrySave(List<string> problems, hMailServer.Message message, string what)
      {
         try
         {
            message.Save();
            problems.Add(what + " was saved");
         }
         catch (COMException refused)
         {
            if (!refused.Message.Contains("You do not have access"))
               problems.Add(what + " failed, but not as refused: " + refused.Message);
         }
      }

      [Test]
      [Description("An account's Application.CreateMessage sends as the account and an alias of it, and is refused " +
                   "an envelope sender, a From or a Sender of another address, and a From that names two mailboxes or " +
                   "hides one in a comment.")]
      public void AnAccountsCreatedMessageClaimsOnlyItsOwnAddresses()
      {
         RequireAnAdministratorPassword();
         var problems = new List<string>();

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "own@example.test", SenderPassword);
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "own.alias@example.test", sender.Address);
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "inbox@example.test", "test");

         TrySave(problems, Compose(sender.Address, SenderPassword, "ceo@elsewhere.test", null, recipient.Address, "Envelope"),
            "An envelope sender of another address");
         TrySave(problems, Compose(sender.Address, SenderPassword, sender.Address, "CEO <ceo@elsewhere.test>", recipient.Address, "From"),
            "A From of another address");
         TrySave(problems, Compose(sender.Address, SenderPassword, sender.Address, sender.Address + ", ceo@elsewhere.test", recipient.Address, "Two"),
            "A From of two mailboxes");
         TrySave(problems, Compose(sender.Address, SenderPassword, sender.Address, "ceo@elsewhere.test (<" + sender.Address + ">)", recipient.Address, "Comment"),
            "A From whose own address is in a comment");

         var withSender = Compose(sender.Address, SenderPassword, sender.Address, sender.Address, recipient.Address, "Sender");
         withSender.set_HeaderValue("Sender", "ceo@elsewhere.test");
         TrySave(problems, withSender, "A Sender of another address");

         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         Compose(sender.Address, SenderPassword, sender.Address, "Own <" + sender.Address + ">", recipient.Address, "Own").Save();
         Compose(sender.Address, SenderPassword, "own.alias@example.test", "own.alias@example.test", recipient.Address, "Alias").Save();

         CollectionAssert.AreEquivalent(new[] { "Own", "Alias", "Control" }, SubjectsAfterAControl(recipient, "test", 3));
      }

      [Test]
      [Description("An account's message, once queued, is refused a Save that makes it claim another address: the " +
                   "program keeps the message object, and a Save of a queued message writes the file its delivery " +
                   "reads.")]
      public void AnAccountsQueuedMessageSavedAgainClaimsOnlyItsOwnAddresses()
      {
         RequireAnAdministratorPassword();
         var problems = new List<string>();

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "requeue@example.test", SenderPassword);
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "requeue.inbox@example.test", "test");

         var message = Compose(sender.Address, SenderPassword, sender.Address, "Own <" + sender.Address + ">",
            recipient.Address, "Queued as itself");
         message.Save();

         message.From = "CEO <ceo@elsewhere.test>";
         TrySave(problems, message, "The queued message saved again with a From of another address");

         message.From = "Own <" + sender.Address + ">";
         message.set_HeaderValue("Sender", "ceo@elsewhere.test");
         TrySave(problems, message, "The queued message saved again with a Sender of another address");

         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         CollectionAssert.AreEquivalent(new[] { "Queued as itself", "Control" }, SubjectsAfterAControl(recipient, "test", 2));

         var messages = recipient.IMAPFolders.get_ItemByName("INBOX").Messages;
         for (var i = 0; i < messages.Count; i++)
         {
            if (messages[i].Subject != "Queued as itself")
               continue;

            StringAssert.Contains(sender.Address, messages[i].From);
            StringAssert.DoesNotContain("elsewhere", messages[i].get_HeaderValue("From") + messages[i].get_HeaderValue("Sender"));
         }
      }

      [Test]
      [Description("An account's message has a header written as one: a From or a Sender of another address put " +
                   "into another field's value with a line break, a From whose value a line break and a field follow, " +
                   "a field named \"From \", a lone line feed and an " +
                   "address in an encoded word of the display name are refused; a display name outside ASCII, " +
                   "quoted or not, and a subject long enough to be folded are sent.")]
      public void AnAccountsMessageHasItsHeaderWrittenAsOne()
      {
         RequireAnAdministratorPassword();
         var problems = new List<string>();

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "header@example.test", SenderPassword);
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "header.inbox@example.test", "test");

         TrySave(problems, Compose(sender.Address, SenderPassword, sender.Address, null, recipient.Address,
               "Injected\r\nFrom: CEO <ceo@elsewhere.test>"),
            "A Subject that holds a From field of another address, in a message with no From of its own");

         var sentBy = Compose(sender.Address, SenderPassword, sender.Address, sender.Address, recipient.Address, "Sender in To");
         sentBy.AddRecipient("Board\r\nSender: ceo@elsewhere.test", "board@example.test");
         TrySave(problems, sentBy, "A recipient's name that holds a Sender field of another address");

         var spaced = Compose(sender.Address, SenderPassword, sender.Address, sender.Address, recipient.Address, "Spaced");
         spaced.set_HeaderValue("From ", "ceo@elsewhere.test");
         TrySave(problems, spaced, "A field named \"From \", written \"From :\"");

         // One header as given - the From, then a field of its own - but written as
         // one From, since the writer keeps a value to one field: "header@example.test",
         // two spaces and "X-Note: CEO <ceo@elsewhere.test>", whose first address is
         // the CEO's to the parser DKIMSigner reads a From with.
         TrySave(problems, Compose(sender.Address, SenderPassword, sender.Address,
               sender.Address + "\r\nX-Note: CEO <ceo@elsewhere.test>", recipient.Address, "Joined"),
            "A From of its own address, a line break and a field naming another address, which the writer joins to the From");

         var lineFeed = Compose(sender.Address, SenderPassword, sender.Address, sender.Address, recipient.Address, "Line feed");
         lineFeed.set_HeaderValue("X-Note", "note\nFrom: ceo@elsewhere.test");
         TrySave(problems, lineFeed, "A value that holds a line feed alone");

         var encoded = Compose(sender.Address, SenderPassword, sender.Address, null, recipient.Address, "Encoded");
         encoded.EncodeFields = false;
         encoded.From = "=?utf-8?q?ceo=40elsewhere.test?= <" + sender.Address + ">";
         TrySave(problems, encoded, "A From whose display name is another address in an encoded word");

         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         var longSubject = "Long " + string.Concat(Enumerable.Repeat("gr\u00fc\u00dfe aus M\u00fcnchen ", 12)).Trim();

         Compose(sender.Address, SenderPassword, sender.Address, "J\u00f6rg M\u00fcller <" + sender.Address + ">",
            recipient.Address, "Outside ASCII").Save();
         Compose(sender.Address, SenderPassword, sender.Address, "\"M\u00fcller, J\u00f6rg\" <" + sender.Address + ">",
            recipient.Address, "Quoted").Save();
         Compose(sender.Address, SenderPassword, sender.Address, sender.Address, recipient.Address, longSubject).Save();

         var subjects = SubjectsAfterAControl(recipient, "test", 4);
         CollectionAssert.IsSubsetOf(new[] { "Outside ASCII", "Quoted", "Control" }, subjects);
         ClassicAssert.AreEqual(1, subjects.Count(subject => subject.StartsWith("Long ")), string.Join(" | ", subjects));
      }

      [Test]
      [Description("A domain administrator's Application.CreateMessage sends as any address of its domain and is " +
                   "refused one of another domain.")]
      public void ADomainAdministratorsCreatedMessageClaimsOnlyItsDomain()
      {
         RequireAnAdministratorPassword();

         var administrator = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "domainadmin@example.test", SenderPassword);
         administrator.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         administrator.Save();
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "staff@example.test", "test");

         var problems = new List<string>();
         TrySave(problems, Compose(administrator.Address, SenderPassword, "ceo@elsewhere.test", "ceo@elsewhere.test", recipient.Address, "Elsewhere"),
            "An address of another domain");
         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         Compose(administrator.Address, SenderPassword, "noreply@example.test", "Notices <noreply@example.test>", recipient.Address, "Domain").Save();

         CollectionAssert.AreEquivalent(new[] { "Domain", "Control" }, SubjectsAfterAControl(recipient, "test", 2));
      }

      [Test]
      [Description("An account given the domainadmin role over two domains sends Application.CreateMessage's " +
                   "message as an address of either, and is refused one of a domain it does not administer.")]
      public void ADomainAdministratorSendsAsEveryDomainItsRoleNames()
      {
         RequireAnAdministratorPassword();

         SingletonProvider<TestSetup>.Instance.AddDomain("second-domain.test");
         var administrator = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "regional@example.test", SenderPassword);
         ((dynamic) SingletonProvider<TestSetup>.Instance.GetApp()).AssignRole(administrator.Address, "domainadmin",
            "example.test, second-domain.test");
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "regional.inbox@example.test", "test");

         var problems = new List<string>();
         TrySave(problems, Compose(administrator.Address, SenderPassword, "ceo@elsewhere.test", "ceo@elsewhere.test", recipient.Address, "Elsewhere"),
            "An address of a domain it does not administer");
         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         Compose(administrator.Address, SenderPassword, "notices@second-domain.test", "Notices <notices@second-domain.test>", recipient.Address, "Second").Save();
         Compose(administrator.Address, SenderPassword, "notices@example.test", "notices@example.test", recipient.Address, "Own").Save();

         CollectionAssert.AreEquivalent(new[] { "Second", "Own", "Control" }, SubjectsAfterAControl(recipient, "test", 3));
      }

      [Test]
      [Description("An account given the domainadmin role over every domain (\"*\") sends Application.CreateMessage's " +
                   "message as an address of any domain this server holds, and is refused one of a domain held " +
                   "elsewhere, which its role names too but nobody here administers.")]
      public void ADomainAdministratorOfEveryDomainSendsAsNoDomainHeldElsewhere()
      {
         RequireAnAdministratorPassword();

         SingletonProvider<TestSetup>.Instance.AddDomain("every-domain.test");
         var administrator = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "everywhere@example.test", SenderPassword);
         ((dynamic) SingletonProvider<TestSetup>.Instance.GetApp()).AssignRole(administrator.Address, "domainadmin", "*");
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "everywhere.inbox@example.test", "test");

         var problems = new List<string>();
         TrySave(problems, Compose(administrator.Address, SenderPassword, "ceo@elsewhere.test", null, recipient.Address, "Envelope elsewhere"),
            "An envelope sender of a domain held elsewhere");
         TrySave(problems, Compose(administrator.Address, SenderPassword, administrator.Address, "CEO <ceo@elsewhere.test>", recipient.Address, "From elsewhere"),
            "A From of a domain held elsewhere");
         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         Compose(administrator.Address, SenderPassword, "notices@every-domain.test", "Notices <notices@every-domain.test>", recipient.Address, "Held here").Save();

         CollectionAssert.AreEquivalent(new[] { "Held here", "Control" }, SubjectsAfterAControl(recipient, "test", 2));
      }

      [Test]
      [Description("A domain administrator's Application.CreateMessage is refused the address of an account of its " +
                   "domain whose authority it does not take in - one whose admin level is the server " +
                   "administrator's - and sends as an ordinary account of the domain.")]
      public void ADomainAdministratorDoesNotSendAsAnAccountItDoesNotTakeIn()
      {
         RequireAnAdministratorPassword();

         var administrator = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "local.admin@example.test", SenderPassword);
         administrator.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         administrator.Save();
         var boss = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "boss@example.test", "test");
         boss.AdminLevel = eAdminLevel.hAdminLevelServerAdmin;
         boss.Save();
         var staff = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "staff.member@example.test", "test");
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "local.inbox@example.test", "test");

         var problems = new List<string>();
         TrySave(problems, Compose(administrator.Address, SenderPassword, boss.Address, null, recipient.Address, "As the boss, envelope"),
            "An envelope sender of an account with the server administrator's admin level");
         TrySave(problems, Compose(administrator.Address, SenderPassword, administrator.Address, "Boss <" + boss.Address + ">", recipient.Address, "As the boss, From"),
            "A From of an account with the server administrator's admin level");
         if (problems.Count > 0)
            Assert.Fail(string.Join(Environment.NewLine, problems));

         Compose(administrator.Address, SenderPassword, staff.Address, "Staff <" + staff.Address + ">", recipient.Address, "As staff").Save();

         CollectionAssert.AreEquivalent(new[] { "As staff", "Control" }, SubjectsAfterAControl(recipient, "test", 2));
      }

      [Test]
      [Description("The server administrator's Application.CreateMessage sends as anybody, as it always has.")]
      public void TheServerAdministratorsCreatedMessageClaimsAnyAddress()
      {
         var recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "anybody@example.test", "test");

         var message = SingletonProvider<TestSetup>.Instance.CreateMessage();
         message.FromAddress = "ceo@elsewhere.test";
         message.From = "CEO <ceo@elsewhere.test>, Board <board@elsewhere.test>";
         message.AddRecipient("", recipient.Address);
         message.Subject = "Anybody";
         message.Body = "Body";
         message.Save();

         CollectionAssert.AreEquivalent(new[] { "Anybody", "Control" }, SubjectsAfterAControl(recipient, "test", 2));
      }
   }

   /// <summary>
   ///    VBScript run by cscript.exe as a COM client process of its own - what a scheduled
   ///    script is - printing one line per step for the test to read.
   /// </summary>
   internal static class ComScripts
   {
      private static readonly TimeSpan Patience = TimeSpan.FromSeconds(90);

      // Shared by every script: a message the script creates itself (hMailServer.Message),
      // sent; "-" stands for an empty argument. Prints SAVED, or what refused it.
      private const string SendDirectlyFunction = @"
Function Arg(i)
   Arg = WScript.Arguments(i)
   If Arg = ""-"" Then Arg = """"
End Function

Function TrySend(envelope, fromField, recipient, subject, attachment)
   Dim m
   On Error Resume Next
   Set m = CreateObject(""hMailServer.Message"")
   If Err.Number <> 0 Then
      TrySend = ""CREATE-FAILED "" & Err.Description
      Exit Function
   End If
   m.FromAddress = envelope
   If fromField <> """" Then m.From = fromField
   m.AddRecipient """", recipient
   m.Subject = subject
   m.Body = ""Body""
   If attachment <> """" Then
      m.Attachments.Add attachment
      If Err.Number <> 0 Then
         TrySend = ""ADD-REFUSED "" & Err.Description
         Exit Function
      End If
   End If
   m.Save
   If Err.Number <> 0 Then
      TrySend = ""SAVE-REFUSED "" & Err.Description
   Else
      TrySend = ""SAVED""
   End If
End Function

Function SignIn(user, password)
   Dim app, account
   Set app = CreateObject(""hMailServer.Application"")
   Set account = app.Authenticate(user, password)
   If account Is Nothing Then
      WScript.Echo ""SIGN-IN-FAILED "" & user
      WScript.Quit 2
   End If
   Set SignIn = app
End Function

Sub Signal(name)
   Dim fso
   Set fso = CreateObject(""Scripting.FileSystemObject"")
   fso.CreateTextFile(fso.BuildPath(Arg(0), name)).Close
End Sub

Sub WaitFor(name)
   Dim fso, waited
   Set fso = CreateObject(""Scripting.FileSystemObject"")
   waited = 0
   Do While Not fso.FileExists(fso.BuildPath(Arg(0), name))
      WScript.Sleep 100
      waited = waited + 1
      If waited > 900 Then
         WScript.Echo ""TIMED-OUT waiting for "" & name
         WScript.Quit 3
      End If
   Loop
End Sub
";

      // Arguments: folder, user, password, envelope, From, recipient, subject, attachment.
      private const string SendDirectlyScript = @"Option Explicit
Dim app
If Arg(1) <> """" Then Set app = SignIn(Arg(1), Arg(2))
WScript.Echo TrySend(Arg(3), Arg(4), Arg(5), Arg(6), Arg(7))
" + SendDirectlyFunction;

      // Arguments: folder, user, password, account id.
      private const string FetchAccountScript = @"Option Explicit
Dim app, f, result
If Arg(1) <> """" Then Set app = SignIn(Arg(1), Arg(2))
Set f = CreateObject(""hMailServer.FetchAccount"")
f.Name = ""Created by a program""
f.ServerAddress = ""fetch.example.test""
f.Username = ""user""
On Error Resume Next
f.AccountID = CLng(Arg(3))
If Err.Number <> 0 Then
   result = ""ACCOUNTID-REFUSED "" & Err.Description
   Err.Clear
Else
   result = ""ACCOUNTID-TAKEN""
End If
f.Save
If Err.Number <> 0 Then
   result = result & "" SAVE-REFUSED "" & Err.Description
Else
   result = result & "" SAVED""
End If
WScript.Echo result
" + SendDirectlyFunction;

      // Arguments: folder, password, recipient. Signs in, sends, says ready, waits to be
      // told to let go, lets go, sends again, says so, waits to be told to end.
      private const string HoldScript = @"Option Explicit
Dim app
Set app = SignIn(""Administrator"", Arg(1))
WScript.Echo ""HOLDING "" & TrySend(""someone@example.test"", """", Arg(2), ""Saved while holding"", """")
Signal ""ready""
WaitFor ""release""
Set app = Nothing
WScript.Echo ""RELEASED "" & TrySend(""someone@example.test"", """", Arg(2), ""Saved after releasing"", """")
Signal ""released""
WaitFor ""end""
" + SendDirectlyFunction;

      // Arguments: folder, password, recipient.
      private const string FailScript = @"Option Explicit
Dim app, again
Set app = SignIn(""Administrator"", Arg(1))
WScript.Echo ""BEFORE "" & TrySend(""someone@example.test"", """", Arg(2), ""Before the failed sign-in"", """")
Set again = app.Authenticate(""Administrator"", ""not the password"")
If Not again Is Nothing Then WScript.Echo ""THE WRONG PASSWORD SIGNED IN""
WScript.Echo ""AFTER "" & TrySend(""someone@example.test"", """", Arg(2), ""After the failed sign-in"", """")
" + SendDirectlyFunction;

      // Arguments: folder, account, account password, administrator password, recipient, file.
      private const string TwoSignInsScript = @"Option Explicit
Dim asAccount, asAdministrator
Set asAccount = SignIn(Arg(1), Arg(2))
Set asAdministrator = SignIn(""Administrator"", Arg(3))
WScript.Echo ""BOTH "" & TrySend(""ceo@elsewhere.test"", ""CEO <ceo@elsewhere.test>"", Arg(4), ""Both signed in"", Arg(5))
Set asAdministrator = Nothing
WScript.Echo ""ACCOUNT-ALONE-FILE "" & TrySend(Arg(1), """", Arg(4), ""The account alone, with a file"", Arg(5))
WScript.Echo ""ACCOUNT-ALONE-FOREIGN "" & TrySend(""ceo@elsewhere.test"", """", Arg(4), ""The account alone, as another"", """")
WScript.Echo ""ACCOUNT-ALONE-OWN "" & TrySend(Arg(1), """", Arg(4), ""The account alone, as itself"", """")
" + SendDirectlyFunction;

      public static string SendDirectly(string folder, string user, string password, string envelope, string from,
         string recipient, string subject, string attachment)
      {
         return Run(folder, "send-directly.vbs", SendDirectlyScript, folder, user, password, envelope, from, recipient,
            subject, attachment);
      }

      public static string FetchAccountDirectly(string folder, string user, string password, int accountId)
      {
         return Run(folder, "fetch-account.vbs", FetchAccountScript, folder, user, password, accountId.ToString());
      }

      public static string SignInThenFail(string folder, string password, string recipient)
      {
         return Run(folder, "fail.vbs", FailScript, folder, password, recipient);
      }

      public static string TwoSignIns(string folder, string account, string accountPassword, string administratorPassword,
         string recipient, string file)
      {
         return Run(folder, "two.vbs", TwoSignInsScript, folder, account, accountPassword, administratorPassword, recipient, file);
      }

      public sealed class Holder
      {
         private readonly string _folder;

         internal Holder(string folder)
         {
            _folder = folder;
         }

         // The signal files the holding script waits for, in the folder it was given.
         public void Release()
         {
            File.WriteAllText(Path.Combine(_folder, "release"), "");
            WaitForSignal(_folder, "released");
         }

         internal bool Released => File.Exists(Path.Combine(_folder, "release"));

         internal void End()
         {
            File.WriteAllText(Path.Combine(_folder, "end"), "");
         }
      }

      // Runs the holding script; while it holds its sign-in, and after Release, the body runs.
      public static string HoldASignIn(string folder, string password, string recipient, Action<Holder> body)
      {
         var process = Start(folder, "hold.vbs", HoldScript, folder, password, recipient);
         var output = new System.Text.StringBuilder();
         process.OutputDataReceived += (sender, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
         process.ErrorDataReceived += (sender, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
         process.BeginOutputReadLine();
         process.BeginErrorReadLine();

         try
         {
            if (!WaitForSignal(folder, "ready", process))
            {
               lock (output)
                  Assert.Fail("The holding script never signed in: " + output);
            }

            var holder = new Holder(folder);
            body(holder);

            if (!holder.Released)
               holder.Release();

            holder.End();

            if (!process.WaitForExit((int) Patience.TotalMilliseconds))
               Assert.Fail("The holding script did not end.");
            process.WaitForExit();
         }
         finally
         {
            if (!process.HasExited)
               process.Kill();
            process.Dispose();
         }

         lock (output)
            return output.ToString();
      }

      private static bool WaitForSignal(string folder, string name, Process process = null)
      {
         var until = DateTime.Now + Patience;
         while (DateTime.Now < until)
         {
            if (File.Exists(Path.Combine(folder, name)))
               return true;
            if (process != null && process.HasExited)
               return false;
            Thread.Sleep(100);
         }

         Assert.Fail("Waited " + Patience.TotalSeconds + " s for the script to say " + name + ".");
         return false;
      }

      private static Process Start(string folder, string name, string script, params string[] arguments)
      {
         var path = Path.Combine(folder, name);
         File.WriteAllText(path, script.Replace("\r\n", "\n").Replace("\n", "\r\n"), System.Text.Encoding.ASCII);

         var system = Path.Combine(Environment.GetEnvironmentVariable("SystemRoot") ?? @"C:\Windows",
            Environment.Is64BitProcess || !Environment.Is64BitOperatingSystem ? "System32" : "Sysnative");

         var start = new ProcessStartInfo(Path.Combine(system, "cscript.exe"))
         {
            Arguments = "//NoLogo //E:vbscript \"" + path + "\" " +
                        string.Join(" ", arguments.Select(argument => "\"" + argument + "\"")),
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
         };

         return Process.Start(start);
      }

      // The script's output, trimmed; a script that does not end in time fails the test.
      private static string Run(string folder, string name, string script, params string[] arguments)
      {
         using (var process = Start(folder, name, script, arguments))
         {
            var error = process.StandardError.ReadToEndAsync();
            var output = process.StandardOutput.ReadToEndAsync();

            if (!process.WaitForExit((int) Patience.TotalMilliseconds))
            {
               process.Kill();
               Assert.Fail(name + " did not end within " + Patience.TotalSeconds + " s.");
            }

            process.WaitForExit();
            var printed = (output.Result + error.Result).Trim();

            if (process.ExitCode != 0)
               Assert.Fail(name + " ended with " + process.ExitCode + ": " + printed);

            return printed;
         }
      }
   }
}
