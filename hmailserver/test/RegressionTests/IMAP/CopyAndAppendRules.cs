// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   /// What APPEND, COPY and MOVE into a folder tell the user and carry with them - four
   /// of the findings of upstream's UIDPLUS work (#650, #666) that this server shared:
   ///
   ///  * RFC 3501 section 6.4.7: "If the COPY command is unsuccessful for any reason,
   ///    server implementations MUST restore the destination mailbox to its state
   ///    before the COPY attempt." The copies made before the message that could not
   ///    be copied were left behind.
   ///  * RFC 6851 section 3.3, of MOVE and UID MOVE: "The server SHOULD NOT leave any
   ///    message in both mailboxes ... This is true even if the server returns a
   ///    tagged NO response to the command." A MOVE that failed part way left the
   ///    copies it had made in the destination, and every original in the source -
   ///    found by the release review of 6.3.7, once COPY was put right.
   ///
   /// and, into a folder another account owns, by the rights the user holds there:
   ///
   ///  * RFC 4315 section 6: APPENDUID and COPYUID tell the folder's UIDVALIDITY and
   ///    the UIDs given, and "SHOULD NOT be issued if the client does not have access
   ///    to SELECT or EXAMINE the mailbox" - a folder the user may only drop mail into.
   ///    They were given whatever the rights.
   ///  * RFC 4314 section 4: the flags a message arrives with are those the user may set
   ///    there - \Seen with 's', \Deleted with 't', \Draft, \Answered, \Flagged and the
   ///    keywords with 'w'. Only \Seen was left out, by APPEND and COPY alike.
   ///  * RFC 5530 section 3: a refusal by the access control is NO [NOPERM].
   /// </summary>
   [TestFixture]
   public class CopyAndAppendRules : TestFixtureBase
   {
      private Account _account;

      [SetUp]
      public void CreateAccount()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "uidplus-acl@example.test", "test");
      }

      private IMAPFolder AddPublicFolder(string name, params eACLPermission[] rights)
      {
         var folder = _settings.PublicFolders.Add(name);
         folder.Save();

         var permission = folder.Permissions.Add();
         permission.PermissionAccountID = _account.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;

         foreach (var right in rights)
            permission.set_Permission(right, true);

         permission.Save();

         return folder;
      }

      private ImapClientSimulator SignIn()
      {
         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.LogonWithLiteral(_account.Address, "test");
         return simulator;
      }

      private ImapClientSimulator SignInWithAMessageInTheInbox()
      {
         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, "UIDPLUS and ACL", "Body");
         CustomAsserts.AssertFolderMessageCount(_account.IMAPFolders.get_ItemByName("INBOX"), 1);

         var simulator = SignIn();
         Assert.IsTrue(simulator.SelectFolder("INBOX"));
         return simulator;
      }

      [Test]
      [Description("APPEND into a folder the user may insert into and not read is OK with no APPENDUID; " +
                   "with the read right it has one.")]
      public void AppendIntoAFolderTheUserCannotReadGivesNoAppendUid()
      {
         AddPublicFolder("DropBox", eACLPermission.ePermissionLookup, eACLPermission.ePermissionInsert);
         AddPublicFolder("Readable", eACLPermission.ePermissionLookup, eACLPermission.ePermissionInsert, eACLPermission.ePermissionRead);

         var simulator = SignIn();

         string dropped = simulator.SendSingleCommandWithLiteral("A01 APPEND #Public.DropBox {4}", "ABCD");
         StringAssert.Contains("A01 OK", dropped);
         StringAssert.DoesNotContain("APPENDUID", dropped, "A user who may not read the folder must not be told its UIDs.");

         string readable = simulator.SendSingleCommandWithLiteral("A02 APPEND #Public.Readable {4}", "ABCD");
         StringAssert.Contains("A02 OK [APPENDUID ", readable);

         simulator.Disconnect();
      }

      [Test]
      [Description("COPY into a folder the user may insert into and not read is OK with no COPYUID; with " +
                   "the read right it has one.")]
      public void CopyIntoAFolderTheUserCannotReadGivesNoCopyUid()
      {
         AddPublicFolder("DropBox", eACLPermission.ePermissionLookup, eACLPermission.ePermissionInsert);
         AddPublicFolder("Readable", eACLPermission.ePermissionLookup, eACLPermission.ePermissionInsert, eACLPermission.ePermissionRead);

         var simulator = SignInWithAMessageInTheInbox();

         string dropped = simulator.SendSingleCommand("A01 COPY 1 \"#Public.DropBox\"");
         StringAssert.Contains("A01 OK", dropped);
         StringAssert.DoesNotContain("COPYUID", dropped, "A user who may not read the folder must not be told its UIDs.");

         string readable = simulator.SendSingleCommand("A02 COPY 1 \"#Public.Readable\"");
         StringAssert.Contains("A02 OK [COPYUID ", readable);

         simulator.Disconnect();
      }

      [Test]
      [Description("A message copied into a folder where the user may insert and read, and set no flag, " +
                   "arrives with none of its flags. \\Deleted, \\Flagged, \\Answered and \\Draft were copied.")]
      public void CopyCarriesOnlyTheFlagsTheUserMaySet()
      {
         AddPublicFolder("NoFlags", eACLPermission.ePermissionLookup, eACLPermission.ePermissionInsert, eACLPermission.ePermissionRead);

         var simulator = SignInWithAMessageInTheInbox();

         StringAssert.Contains("A01 OK", simulator.SendSingleCommand("A01 STORE 1 +FLAGS (\\Seen \\Deleted \\Flagged \\Answered \\Draft)"));
         StringAssert.Contains("A02 OK", simulator.SendSingleCommand("A02 COPY 1 \"#Public.NoFlags\""));

         Assert.IsTrue(simulator.SelectFolder("#Public.NoFlags"));
         string flags = simulator.SendSingleCommand("A03 FETCH 1 (FLAGS)");

         foreach (var flag in new[] { "\\Seen", "\\Deleted", "\\Flagged", "\\Answered", "\\Draft" })
            StringAssert.DoesNotContain(flag, flags, "The copy carried a flag the user may not set there: " + flags);

         simulator.Disconnect();
      }

      [Test]
      [Description("A message appended to a folder where the user may insert and read, and set no flag, " +
                   "is stored with none of the flags asked for. \\Deleted and \\Flagged were stored.")]
      public void AppendCarriesOnlyTheFlagsTheUserMaySet()
      {
         AddPublicFolder("NoFlags", eACLPermission.ePermissionLookup, eACLPermission.ePermissionInsert, eACLPermission.ePermissionRead);

         var simulator = SignIn();

         StringAssert.Contains("A01 OK", simulator.SendSingleCommandWithLiteral("A01 APPEND #Public.NoFlags (\\Seen \\Deleted \\Flagged) {4}", "ABCD"));

         Assert.IsTrue(simulator.SelectFolder("#Public.NoFlags"));
         string flags = simulator.SendSingleCommand("A02 FETCH 1 (FLAGS)");

         foreach (var flag in new[] { "\\Seen", "\\Deleted", "\\Flagged" })
            StringAssert.DoesNotContain(flag, flags, "The appended message carried a flag the user may not set there: " + flags);

         simulator.Disconnect();
      }

      // An account of 1 MB whose INBOX holds two messages of 300 KB, signed in with
      // INBOX selected, and an empty folder Target. One copy of a message fits the
      // quota and a second does not: each copy counts against it as it is saved,
      // before a MOVE has taken any original out.
      private ImapClientSimulator SignInWithTwoMessagesOfWhichOneCopyFits(string address)
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, "test", 1);

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder("Target"));

         var body = new System.Text.StringBuilder();
         for (int line = 0; line < 3072; line++)
            body.Append(new string('x', 98)).Append("\r\n");

         for (int i = 0; i < 2; i++)
         {
            string message = "From: " + address + "\r\nSubject: Large " + i + "\r\n\r\n" + body;
            string appended = simulator.SendSingleCommandWithLiteral("A0" + i + " APPEND INBOX {" + message.Length + "}", message);
            StringAssert.Contains("A0" + i + " OK", appended);
         }

         Assert.IsTrue(simulator.SelectFolder("INBOX"));

         return simulator;
      }

      [Test]
      [Description("A COPY that fails part way - the second copy over the quota - leaves the destination as " +
                   "it was, and the copy taken out again no longer counts against the quota. The first copy " +
                   "was left in the destination with the command answered NO.")]
      public void ACopyThatFailsPartWayLeavesTheDestinationAsItWas()
      {
         var simulator = SignInWithTwoMessagesOfWhichOneCopyFits("copy-atomic@example.test");

         string copy = simulator.SendSingleCommand("A10 COPY 1:2 \"Target\"");
         StringAssert.Contains("A10 NO", copy);
         StringAssert.DoesNotContain("COPYUID", copy);

         string status = simulator.Status("Target", "MESSAGES");
         StringAssert.Contains("(MESSAGES 0)", status, "A failed COPY left copies in the destination: " + status);

         // The copy taken out no longer counts against the quota.
         StringAssert.Contains("A11 OK", simulator.SendSingleCommand("A11 COPY 1 \"Target\""));

         simulator.Disconnect();
      }

      // A MOVE or a UID MOVE (command) of messages 1 and 2, UIDs 1 and 2, that fails part
      // way: the first copy fits the quota and the second does not.
      private void AssertAMoveThatFailsPartWayLeavesNoMessageInBothMailboxes(string address, string command)
      {
         var simulator = SignInWithTwoMessagesOfWhichOneCopyFits(address);

         string refused = simulator.SendSingleCommand("A10 " + command + " 1:2 \"Target\"");
         StringAssert.Contains("A10 NO", refused);
         StringAssert.DoesNotContain("COPYUID", refused, "A failed " + command + " named copies: " + refused);
         StringAssert.DoesNotContain("EXPUNGE", refused, "A failed " + command + " expunged an original: " + refused);

         // Each message moved or left as it was, and none in both mailboxes.
         string target = simulator.Status("Target", "MESSAGES");
         StringAssert.Contains("(MESSAGES 0)", target, "A failed " + command + " left copies in the destination: " + target);

         string inbox = simulator.Status("INBOX", "MESSAGES");
         StringAssert.Contains("(MESSAGES 2)", inbox, "A failed " + command + " took an original out of the source: " + inbox);

         // The copy taken out no longer counts against the quota: one message moves.
         StringAssert.Contains("A11 OK", simulator.SendSingleCommand("A11 " + command + " 1 \"Target\""));

         target = simulator.Status("Target", "MESSAGES");
         StringAssert.Contains("(MESSAGES 1)", target);

         simulator.Disconnect();
      }

      [Test]
      [Description("A MOVE that fails part way - the second copy over the quota - leaves no message in both " +
                   "mailboxes (RFC 6851 section 3.3): the destination as it was, both originals where they were, " +
                   "and the copy taken out again no longer counted against the quota. The first copy was left in " +
                   "the destination, beside its original, with the command answered NO.")]
      public void AMoveThatFailsPartWayLeavesNoMessageInBothMailboxes()
      {
         AssertAMoveThatFailsPartWayLeavesNoMessageInBothMailboxes("move-atomic@example.test", "MOVE");
      }

      [Test]
      [Description("A UID MOVE that fails part way leaves no message in both mailboxes, as a MOVE does. The " +
                   "first copy was left in the destination, beside its original, with the command answered NO.")]
      public void AUidMoveThatFailsPartWayLeavesNoMessageInBothMailboxes()
      {
         AssertAMoveThatFailsPartWayLeavesNoMessageInBothMailboxes("uid-move-atomic@example.test", "UID MOVE");
      }

      [Test]
      [Description("A MOVE into the mailbox the session has selected that fails part way leaves the mailbox " +
                   "as it was, and tells the session of no message: the first copy was left in it, beside its " +
                   "original, and never reported to the session that made it.")]
      public void AMoveIntoTheSelectedMailboxThatFailsPartWayLeavesItAsItWas()
      {
         var simulator = SignInWithTwoMessagesOfWhichOneCopyFits("move-selected-atomic@example.test");

         string refused = simulator.SendSingleCommand("A10 MOVE 1:2 \"INBOX\"");
         StringAssert.Contains("A10 NO", refused);
         StringAssert.DoesNotContain("EXISTS", refused, refused);
         StringAssert.DoesNotContain("EXPUNGE", refused, refused);

         string inbox = simulator.Status("INBOX", "MESSAGES");
         StringAssert.Contains("(MESSAGES 2)", inbox, "A failed MOVE left a copy in the mailbox: " + inbox);

         // The session knows the two messages it knew, by the numbers it knew them by.
         string fetched = simulator.SendSingleCommand("A11 FETCH 1:* (UID)");
         StringAssert.Contains("* 1 FETCH (UID 1)", fetched);
         StringAssert.Contains("* 2 FETCH (UID 2)", fetched);
         StringAssert.DoesNotContain("* 3 ", fetched, fetched);

         // The copy taken out no longer counts against the quota: one message moves.
         StringAssert.Contains("A12 OK", simulator.SendSingleCommand("A12 MOVE 1 \"INBOX\""));

         simulator.Disconnect();
      }

      [Test]
      [Description("A refusal by the access control is NO [NOPERM] (RFC 5530 section 3).")]
      public void AnAclRefusalIsNoPerm()
      {
         AddPublicFolder("NoInsert", eACLPermission.ePermissionLookup, eACLPermission.ePermissionRead);

         var simulator = SignInWithAMessageInTheInbox();

         string refused = simulator.SendSingleCommand("A01 COPY 1 \"#Public.NoInsert\"");
         StringAssert.Contains("A01 NO [NOPERM] ", refused);

         simulator.Disconnect();
      }

      // A grant to this fixture's account on a folder of another account's, as an
      // hm_acl row (SharedMailboxes.GrantRightsOnFolder does the same), with the
      // rights as their bits: l 1, x 128.
      private void GrantOnAnotherAccountsFolder(IMAPFolder folder, int rightsMask)
      {
         SingletonProvider<TestSetup>.Instance.GetApp().Database.ExecuteSQL(
            string.Format(
               "insert into hm_acl (aclsharefolderid, aclpermissiontype, aclpermissiongroupid, aclpermissionaccountid, aclvalue) " +
               "values ({0}, 0, 0, {1}, {2})",
               folder.ID, _account.ID, rightsMask));
      }

      [Test]
      [Description("RENAME, SETACL, GETACL and LISTRIGHTS refused by the access control are NO [NOPERM] as " +
                   "well (RFC 5530 section 3): RENAME without the 'x' right on the folder, or the 'k' right " +
                   "where it would go, in a public folder and in another account's, and the three ACL commands " +
                   "without the 'a' right. They answered NO with no response code.")]
      public void RenameAndTheAclCommandsRefusedByTheAccessControlAreNoPerm()
      {
         const int lookup = 1;
         const int deleteMailbox = 128;

         AddPublicFolder("Fixed", eACLPermission.ePermissionLookup, eACLPermission.ePermissionRead);
         AddPublicFolder("Parent", eACLPermission.ePermissionLookup, eACLPermission.ePermissionRead, eACLPermission.ePermissionCreate);

         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "noperm-owner@example.test", "test");
         SmtpClientSimulator.StaticSend("sender@example.test", owner.Address, "Inbox", "Body");
         CustomAsserts.AssertFolderMessageCount(owner.IMAPFolders.get_ItemByName("INBOX"), 1);

         var ownerSimulator = new ImapClientSimulator();
         Assert.IsTrue(ownerSimulator.ConnectAndLogon(owner.Address, "test"));
         Assert.IsTrue(ownerSimulator.CreateFolder("Locked"));
         Assert.IsTrue(ownerSimulator.CreateFolder("Cases"));
         Assert.IsTrue(ownerSimulator.CreateFolder("INBOX.Drafts"));
         ownerSimulator.Disconnect();

         // "Locked" may be seen only; "Cases" and INBOX with its "Drafts" may be seen
         // and deleted, and nothing may be created anywhere in the owner's tree.
         GrantOnAnotherAccountsFolder(CustomAsserts.AssertFolderExists(owner.IMAPFolders, "Locked"), lookup);
         GrantOnAnotherAccountsFolder(CustomAsserts.AssertFolderExists(owner.IMAPFolders, "Cases"), lookup | deleteMailbox);
         GrantOnAnotherAccountsFolder(owner.IMAPFolders.get_ItemByName("INBOX"), lookup | deleteMailbox);

         const string tree = "#Users.noperm-owner@example.test.";

         var simulator = SignIn();

         var refusals = new[]
         {
            // A public folder renamed without 'x' on it.
            "A01 RENAME \"#Public.Fixed\" \"#Public.Parent.Fixed\"",
            // Another account's folder renamed without 'x' on it...
            "A02 RENAME \"" + tree + "Locked\" \"" + tree + "INBOX.Locked\"",
            // ...with 'x', under a parent without 'k'...
            "A03 RENAME \"" + tree + "INBOX.Drafts\" \"" + tree + "INBOX.Done\"",
            // ...and with 'x', to the root of the owner's tree, where only the owner creates.
            "A04 RENAME \"" + tree + "Cases\" \"" + tree + "Closed\"",
            // The ACL of a folder the user may not administer.
            "A05 SETACL \"#Public.Fixed\" " + _account.Address + " lr",
            "A06 GETACL \"#Public.Fixed\"",
            "A07 LISTRIGHTS \"#Public.Fixed\" " + _account.Address
         };

         foreach (var command in refusals)
         {
            string tag = command.Substring(0, 3);
            string response = simulator.SendSingleCommand(command);
            StringAssert.Contains(tag + " NO [NOPERM] ", response, command);
         }

         simulator.Disconnect();
      }
   }
}
