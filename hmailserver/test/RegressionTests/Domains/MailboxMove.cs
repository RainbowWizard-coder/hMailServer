// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Domains
{
   /// <summary>
   ///    Renaming a mailbox, and moving one to another domain on this server.
   ///
   ///    What is being tested is that the account keeps its IDENTITY. Everything
   ///    keyed on the account id - its rules, its folders and the messages in
   ///    them, its signature, its group memberships - follows the row without
   ///    being touched, which is the whole reason this exists instead of "create
   ///    an account and copy the mail into it". The parts that do NOT follow the
   ///    id are the ones a test has to be suspicious of: the message store on
   ///    disk, which is named after the address and from which every message
   ///    path is rebuilt at read time, and the references other objects hold to
   ///    the address as TEXT.
   ///
   ///    So every test here reads the messages back through a client after the
   ///    move rather than counting rows. A rename that leaves the store behind
   ///    passes any assertion made about the database and fails the moment
   ///    somebody opens their mail, which is the defect this feature exists to
   ///    avoid.
   ///
   ///    The new COM members are reached by late binding. The test project
   ///    compiles against the type library registered on the bench, which is
   ///    older than this IDL until an elevated build/register-com.ps1 has run, so
   ///    an early-bound call to one of them does not compile; through IDispatch
   ///    it does, and a refusal still arrives as the COMException each assertion
   ///    expects.
   /// </summary>
   [TestFixture]
   public class MailboxMove : TestFixtureBase
   {
      private const string Password = "test-Pass-1";
      private const string SecondDomain = "mailboxmove.test";

      [TearDown]
      public void RemoveTheSecondDomain()
      {
         // PerformBasicSetup removes every domain before the next test, but a
         // failure here would otherwise leave a domain behind holding an address
         // the next test asks for.
         try
         {
            Domain other = _application.Domains.get_ItemByName(SecondDomain);
            if (other != null)
               _application.Domains.DeleteByDBID(other.ID);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
         }
      }

      // ------------------------------------------------------- the new members

      private static string ComMoveTo(Account account, string newAddress, bool keepAlias)
         => (string) ((dynamic) account).MoveTo(newAddress, keepAlias);

      private static string ComInUseReason(Account account) => (string) ((dynamic) account).InUseReason;

      private static string ComLastMove(Account account) => (string) ((dynamic) account).LastMove;

      private static string ComMoveMailbox(Application application, string address, string newAddress, bool keepAlias)
         => (string) ((dynamic) application.Utilities).MoveMailbox(address, newAddress, keepAlias);

      private static string ComMailboxMoves(Application application, string address, int maxRows)
         => (string) ((dynamic) application.Utilities).GetMailboxMoves(address, maxRows);

      /// <summary>
      ///    The POP3 mailbox lock is released when the session's socket closes,
      ///    which is after the client has read the reply to QUIT - so a test that
      ///    reads a mailbox and then moves it has a race to wait out, and
      ///    InUseReason is the thing to wait on. The same shape as
      ///    POP3/Fetching/LockHelper.
      /// </summary>
      private static void WaitUntilMovable(Account account)
      {
         DateTime until = DateTime.Now.AddSeconds(30);
         string reason = "";

         while (DateTime.Now < until)
         {
            reason = ComInUseReason(account);

            if (reason.Length == 0)
               return;

            Thread.Sleep(100);
         }

         Assert.Fail("The mailbox was still in use after 30 seconds: " + reason);
      }

      /// <summary>An account with one message in its inbox, read once.</summary>
      private Account Seeded(string address, out string firstBody)
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);

         firstBody = Guid.NewGuid().ToString();
         SmtpClientSimulator.StaticSend("outside@example.com", address, "In the inbox", firstBody);
         Pop3ClientSimulator.AssertMessageCount(address, Password, 1);

         return account;
      }

      // -------------------------------------------------------------- the tests

      [Test]
      [Description("A rename carries the message store, the folder tree and everything keyed on the account: the messages are still readable over IMAP under the new address, the rules, the signature, the folder and its subscription are the same rows, the account id has not changed, and the alias and list membership that named the account as text have been rewritten.")]
      public void ARenameCarriesTheAccountsStoresAndItsMessages()
      {
         Account account = Seeded("renamed-from@example.test", out string inboxBody);
         int accountId = account.ID;

         // A folder of its own, with a message in it, so that the assertion is
         // about the store and not only about the inbox.
         var seeding = new ImapClientSimulator();
         Assert.IsTrue(seeding.ConnectAndLogon(account.Address, Password));
         Assert.IsTrue(seeding.CreateFolder("Archive2026"));
         Assert.IsTrue(seeding.Subscribe("Archive2026"));
         Assert.IsTrue(seeding.SelectFolder("INBOX"));
         Assert.IsTrue(seeding.Copy(1, "Archive2026"));
         seeding.Logout();
         seeding.Disconnect();

         // A rule, a signature and a group membership: three of the stores that
         // hang off the account id.
         Rule rule = account.Rules.Add();
         rule.Name = "MailboxMove rule";
         rule.Active = true;

         RuleCriteria criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = "1000000";
         criteria.Save();

         RuleAction action = rule.Actions.Add();
         action.Type = eRuleActionType.eRADeleteEmail;
         action.Save();
         rule.Save();

         account.SignatureEnabled = true;
         account.SignaturePlainText = "-- \r\nThe signature that must survive";
         account.Save();

         Group group = SingletonProvider<TestSetup>.Instance.AddGroup("mailboxmove-group");
         SingletonProvider<TestSetup>.Instance.AddGroupMember(group, account);

         // An alias pointing AT the account, and a distribution list it belongs
         // to: two references held as text, which a rename has to rewrite.
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "points-at@example.test", account.Address);
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "listed@example.test",
            new List<string> { account.Address });

         WaitUntilMovable(account);

         string summary = ComMoveTo(account, "renamed-to@example.test", true);

         StringAssert.Contains("\"old_address\":\"renamed-from@example.test\"", summary, summary);
         StringAssert.Contains("\"new_address\":\"renamed-to@example.test\"", summary, summary);
         StringAssert.Contains("\"alias_kept\":true", summary, summary);

         // The identity: the same row, at the new address.
         Account moved = _domain.Accounts.get_ItemByAddress("renamed-to@example.test");
         Assert.AreEqual(accountId, moved.ID, "A move keeps the account row; a copy would not.");

         // The stores that hang off the id.
         Assert.AreEqual(1, moved.Rules.Count, "The account's rules move with it.");
         Assert.AreEqual("MailboxMove rule", moved.Rules[0].Name);
         Assert.IsTrue(moved.SignatureEnabled);
         StringAssert.Contains("The signature that must survive", moved.SignaturePlainText);
         Assert.AreEqual(1, _settings.Groups.get_ItemByName("mailboxmove-group").Members.Count,
            "A group membership is a row keyed on the account id, so it follows without being touched.");

         // The messages, read back through a client at the new address. This is
         // the assertion a rename that left the store behind would fail.
         var reader = new ImapClientSimulator();
         Assert.IsTrue(reader.ConnectAndLogon("renamed-to@example.test", Password),
            "The moved mailbox must sign in at its new address.");
         Assert.AreEqual(1, reader.GetMessageCount("INBOX"));
         StringAssert.Contains(inboxBody, reader.Fetch("1 RFC822"), "The inbox message is readable after the move.");

         StringAssert.Contains("Archive2026", reader.List(), "The folder tree moves with the account.");
         Assert.AreEqual(1, reader.GetMessageCount("Archive2026"));
         StringAssert.Contains(inboxBody, reader.Fetch("1 RFC822"), "The filed copy is readable too.");
         StringAssert.Contains("Archive2026", reader.LSUB(), "A folder's subscription is the folder's own row and moves with it.");
         reader.Logout();
         reader.Disconnect();

         // The references held as text.
         Assert.AreEqual("renamed-to@example.test", _domain.Aliases.get_ItemByName("points-at@example.test").Value,
            "An alias pointing at the account follows it, or mail to the alias is delivered nowhere.");
         Assert.AreEqual("renamed-to@example.test",
            _domain.DistributionLists.get_ItemByAddress("listed@example.test").Recipients[0].RecipientAddress);
      }

      [Test]
      [Description("A move to another domain on this server does everything a rename does, and the messages are readable under the new domain - which the old rename path could never do, since it built both halves of the store path from the old address.")]
      public void AMoveToAnotherDomainCarriesTheSameThings()
      {
         Account account = Seeded("mover@example.test", out string body);
         int accountId = account.ID;

         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain(SecondDomain);

         WaitUntilMovable(account);

         string summary = ComMoveTo(account, "mover@" + SecondDomain, true);

         StringAssert.Contains("\"new_domain\":\"" + SecondDomain + "\"", summary, summary);
         StringAssert.Contains("\"store_moved\":true", summary, summary);

         Account moved = other.Accounts.get_ItemByAddress("mover@" + SecondDomain);
         Assert.AreEqual(accountId, moved.ID, "The account row is the same one, listed under the other domain now.");

         var reader = new ImapClientSimulator();
         Assert.IsTrue(reader.ConnectAndLogon("mover@" + SecondDomain, Password));
         Assert.AreEqual(1, reader.GetMessageCount("INBOX"));
         StringAssert.Contains(body, reader.Fetch("1 RFC822"),
            "The message store followed the account into the other domain's directory.");
         reader.Logout();
         reader.Disconnect();

         // And the store really is under the new domain on disk, which is what
         // the message path above is rebuilt from.
         string movedDirectory = Paths.Combine(Paths.Combine(_settings.Directories.DataDirectory, SecondDomain), "mover");
         Assert.IsTrue(Directory.Exists(movedDirectory), "The mailbox directory is under the new domain: " + movedDirectory);
      }

      [Test]
      [Description("The old address is kept as an alias by default and goes on receiving into the moved mailbox.")]
      public void TheOldAddressIsKeptAsAnAliasAndReceives()
      {
         Account account = Seeded("kept-old@example.test", out string _);
         WaitUntilMovable(account);

         ComMoveTo(account, "kept-new@example.test", true);

         Alias left = _domain.Aliases.get_ItemByName("kept-old@example.test");
         Assert.IsNotNull(left, "The old address is left behind as an alias.");
         Assert.AreEqual("kept-new@example.test", left.Value);
         Assert.IsTrue(left.Active);

         string body = Guid.NewGuid().ToString();
         SmtpClientSimulator.StaticSend("outside@example.com", "kept-old@example.test", "To the old address", body);

         Pop3ClientSimulator.AssertMessageCount("kept-new@example.test", Password, 2);
      }

      [Test]
      [Description("With the switch off the old address stops being an address: nothing is left behind, mail to it is refused, and the moved mailbox still holds what it held.")]
      public void WithoutTheAliasTheOldAddressStopsReceiving()
      {
         Account account = Seeded("dropped-old@example.test", out string _);
         WaitUntilMovable(account);

         ComMoveTo(account, "dropped-new@example.test", false);

         Assert.AreEqual(0, _domain.Aliases.Count, "Nothing is left behind when the switch is off.");

         var smtp = new SmtpClientSimulator();
         Assert.Throws<DeliveryFailedException>(() =>
            smtp.Send("outside@example.com", "dropped-old@example.test", "Nobody there", "body"));

         Pop3ClientSimulator.AssertMessageCount("dropped-new@example.test", Password, 1);
      }

      [Test]
      [Description("Everything a move refuses, it refuses before it changes anything: an address an account already has, an address an alias already has, a domain this server does not hold, and the address the account is already at. After all four the account is where it was, nothing has been created, and its mail is still readable.")]
      public void ARefusedMoveLeavesTheAccountExactlyAsItWas()
      {
         Account account = Seeded("refuser@example.test", out string body);
         int accountId = account.ID;

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "occupied@example.test", Password);
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "aliased@example.test", "occupied@example.test");

         WaitUntilMovable(account);

         COMException taken = Assert.Throws<COMException>(() => ComMoveTo(account, "occupied@example.test", true));
         StringAssert.Contains("An account already exists at occupied@example.test", taken.Message, taken.Message);

         COMException aliased = Assert.Throws<COMException>(() => ComMoveTo(account, "aliased@example.test", true));
         StringAssert.Contains("An alias already exists at aliased@example.test", aliased.Message, aliased.Message);

         COMException unknown = Assert.Throws<COMException>(() => ComMoveTo(account, "refuser@nowhere.invalid", true));
         StringAssert.Contains("does not hold the domain nowhere.invalid", unknown.Message, unknown.Message);

         COMException same = Assert.Throws<COMException>(() => ComMoveTo(account, "refuser@example.test", true));
         StringAssert.Contains("already at that address", same.Message, same.Message);

         // Untouched: the same row, at the same address, with its mail readable.
         Account after = _domain.Accounts.get_ItemByAddress("refuser@example.test");
         Assert.AreEqual(accountId, after.ID);
         Assert.AreEqual(2, _domain.Accounts.Count, "A refusal creates nothing and deletes nothing.");
         Assert.AreEqual(1, _domain.Aliases.Count, "A refusal leaves no alias behind either.");

         StringAssert.Contains(body, Pop3ClientSimulator.AssertGetFirstMessageText("refuser@example.test", Password));
      }

      [Test]
      [Description("The journal and InUseReason answer for one mailbox: nothing has been moved before the first move, the move leaves a complete row naming both addresses, and Utilities.GetMailboxMoves lists it under the address the mailbox has now.")]
      public void TheJournalAndInUseReasonAnswerForTheMailbox()
      {
         Account account = Seeded("journal-from@example.test", out string _);

         Assert.AreEqual("", ComLastMove(account), "A mailbox that has never been moved has no journal row.");

         WaitUntilMovable(account);
         Assert.AreEqual("", ComInUseReason(account), "Nothing is holding the mailbox, so a move would not be refused.");

         ComMoveTo(account, "journal-to@example.test", true);

         Account moved = _domain.Accounts.get_ItemByAddress("journal-to@example.test");
         string last = ComLastMove(moved);

         StringAssert.Contains("\"old_address\":\"journal-from@example.test\"", last, last);
         StringAssert.Contains("\"new_address\":\"journal-to@example.test\"", last, last);
         StringAssert.Contains("\"complete\":true", last, last);
         StringAssert.Contains("\"stage_name\":\"complete\"", last, last);

         string journal = ComMailboxMoves(_application, "journal-to@example.test", 10);
         StringAssert.Contains("\"moves\":[", journal, journal);
         StringAssert.Contains("journal-from@example.test", journal, journal);
      }

      [Test]
      [Description("Utilities.MoveMailbox is the same operation reached by address rather than through an Account object, and it refuses an address no account has.")]
      public void UtilitiesMovesAMailboxByAddress()
      {
         Account account = Seeded("byaddress-from@example.test", out string body);
         WaitUntilMovable(account);

         string summary = ComMoveMailbox(_application, "byaddress-from@example.test", "byaddress-to@example.test", true);
         StringAssert.Contains("\"new_address\":\"byaddress-to@example.test\"", summary, summary);

         StringAssert.Contains(body, Pop3ClientSimulator.AssertGetFirstMessageText("byaddress-to@example.test", Password));

         COMException missing = Assert.Throws<COMException>(
            () => ComMoveMailbox(_application, "nobody@example.test", "somebody@example.test", true));
         StringAssert.Contains("no account at that address", missing.Message, missing.Message);
      }

      [Test]
      [Description("A move is from where the account IS, not from whatever address the object in hand was last given: an Account whose Address has been set and not saved is moved from the address the row holds, so the store is carried and the references follow.")]
      public void AStaleAccountObjectIsMovedFromWhereTheAccountReallyIs()
      {
         Account account = Seeded("stale-from@example.test", out string body);
         int accountId = account.ID;

         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "points-at-stale@example.test", account.Address);

         WaitUntilMovable(account);

         // Set and NOT saved. The object now says an address no account has.
         // Taken at its word, the move would find no directory at that name,
         // carry nothing, write the new address over the row and leave the
         // alias, the list memberships and the queue naming an address that has
         // gone - which is the whole mailbox orphaned by one assignment.
         account.Address = "never-saved@example.test";

         string summary = ComMoveTo(account, "stale-to@example.test", true);

         StringAssert.Contains("\"old_address\":\"stale-from@example.test\"", summary, summary);
         StringAssert.Contains("\"store_moved\":true", summary, summary);

         Account moved = _domain.Accounts.get_ItemByAddress("stale-to@example.test");
         Assert.AreEqual(accountId, moved.ID);
         Assert.AreEqual(1, _domain.Accounts.Count,
            "The address that was never saved is not an account and never was: one account, at the address it was moved to.");

         StringAssert.Contains(body, Pop3ClientSimulator.AssertGetFirstMessageText("stale-to@example.test", Password),
            "The store was carried from the address the account actually had.");

         Assert.AreEqual("stale-to@example.test", _domain.Aliases.get_ItemByName("points-at-stale@example.test").Value,
            "The alias pointing at the account was re-pointed, which it could only be if the move knew the real old address.");
      }

      [Test]
      [Description("Changing only the capitalisation of an address is allowed, moves no directory, and leaves no alias - because the old spelling already reaches the account, and an alias at it would be a duplicate of the account itself. Nothing is reported as an error.")]
      public void ACapitalisationOnlyRenameLeavesNoAliasAndReportsNothing()
      {
         Account account = Seeded("case-me@example.test", out string body);
         WaitUntilMovable(account);

         string summary = ComMoveTo(account, "Case-Me@example.test", true);

         StringAssert.Contains("\"alias_kept\":false", summary, summary);
         StringAssert.Contains("\"store_moved\":false", summary,
            "There is no directory to move when only the spelling changes: " + summary);

         Assert.AreEqual(0, _domain.Aliases.Count,
            "An alias at the old spelling would be a duplicate of the account itself, so none is made.");

         StringAssert.Contains(body, Pop3ClientSimulator.AssertGetFirstMessageText("Case-Me@example.test", Password));

         // The alias that cannot be made used to be attempted anyway, and the
         // refusal became HM6941 in the ERROR log - saying that mail to the old
         // address would be refused until an alias was added by hand, about mail
         // that is delivered.
         RegressionTests.Infrastructure.CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A mailbox cannot be moved into a domain that is already at the maximum number of accounts its administrator set: the account's own save asks that of new accounts only, by an id of zero, and an account arriving from another domain has an id.")]
      public void AMoveIntoADomainAtItsAccountLimitIsRefused()
      {
         Account account = Seeded("limited-from@example.test", out string body);

         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain(SecondDomain);
         other.MaxNumberOfAccountsEnabled = true;
         other.MaxNumberOfAccounts = 1;
         other.Save();

         SingletonProvider<TestSetup>.Instance.AddAccount(other, "resident@" + SecondDomain, Password);

         WaitUntilMovable(account);

         COMException full = Assert.Throws<COMException>(
            () => ComMoveTo(account, "limited-from@" + SecondDomain, true));
         StringAssert.Contains("maximum number of accounts", full.Message, full.Message);

         // Untouched: the account is where it was and its mail is readable.
         Assert.IsNotNull(_domain.Accounts.get_ItemByAddress("limited-from@example.test"));
         Assert.AreEqual(1, other.Accounts.Count, "Nothing arrived in the domain that is full.");
         StringAssert.Contains(body, Pop3ClientSimulator.AssertGetFirstMessageText("limited-from@example.test", Password));
      }
   }
}
