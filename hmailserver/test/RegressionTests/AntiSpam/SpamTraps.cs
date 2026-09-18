// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Spam traps: addresses of a hosted domain that were never real, and what a
// message to one does.
//
// What these tests pin, and why each is the one worth having:
//
//   * a trap is refused with the same reply, word for word, as an address this
//     server simply does not have - a trap that announces itself is a trap the
//     sender removes from their list, so the negative control (an address that is
//     not a trap) is part of the test rather than beside it;
//   * the connecting address is banned through the server's own auto-ban: the
//     range is expiring, it carries the "Auto-ban: " prefix the firewall
//     reconciliation looks for, and it names the trap;
//   * the addresses that are never banned really are not: one on AutoBanNeverBan,
//     and an authenticated session - the two that would otherwise lock out an
//     administrator and one of our own users;
//   * the envelope sender lands on the server's block list with an expiry, and a
//     sender claiming one of our own domains does not, because that is a forgery
//     and blocking it would block the real user;
//   * an account, an alias and a distribution list cannot be given a trap's
//     address, and a trap cannot be given theirs - in both directions, because
//     either way round leaves a mailbox where the evidence should be;
//   * the hit is listed, with what was done to it, and the listing can be cleared.
//
// The new COM members are reached through dynamic, as SenderLists does: the
// suite's interop is generated from the registered type library, and naming them
// early-bound would not compile against a bench whose server predates them.
//
// Every test that bans an address lifts the ban before it returns, and the
// tear-down lifts any that a failure left behind: the suite talks to this server
// from 127.0.0.1, so a ban left in place would refuse every test after it.
//
// What this fixture puts back, it puts back by name. The server's allow and
// block list is shared with whoever else uses this server, so the entries these
// tests make are removed one at a time - a Clear() of the whole list would take
// an operator's entries, or an earlier fixture's, with them and put nothing back.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   [TestFixture]
   public class SpamTraps : TestFixtureBase
   {
      private const string AdminPassword = "testar";
      private const string Password = "Spam-Traps-1!";

      private const string TrapLocalPart = "never-was-real";

      // The senders these tests put on the server's list, by name, so the
      // tear-down can take those back without touching anybody else's.
      private static readonly string[] OurSenders = { "harvester@spammer.example", "spammer.example" };

      // Set when a test switched one of the four settings, so the tear-down puts
      // back what the run started with rather than what this file thinks is right.
      private int banMinutesBefore_;
      private int blockDaysBefore_;
      private int retentionDaysBefore_;
      private bool teachBefore_;

      [SetUp]
      public void DeclareATrap()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         dynamic settings = _settings;
         banMinutesBefore_ = (int) settings.SpamTrapBanMinutes;
         blockDaysBefore_ = (int) settings.SpamTrapBlockDays;
         retentionDaysBefore_ = (int) settings.SpamTrapHitRetentionDays;
         teachBefore_ = (bool) settings.SpamTrapTeachSpamAssassin;

         // A short ban and a short block, so that what is left behind by a failure
         // expires on its own as well as being removed here.
         settings.SpamTrapBanMinutes = 5;
         settings.SpamTrapBlockDays = 1;

         ClearHits();
         RemoveOurServerListEntries();
      }

      [TearDown]
      public void TakeItBack()
      {
         LiftEverySpamTrapBan();
         RemoveEveryTrap();
         ClearHits();
         RemoveOurServerListEntries();

         IniFileSetting.Delete("AutoBanNeverBan");

         dynamic settings = _settings;
         settings.SpamTrapBanMinutes = banMinutesBefore_;
         settings.SpamTrapBlockDays = blockDaysBefore_;
         settings.SpamTrapHitRetentionDays = retentionDaysBefore_;
         settings.SpamTrapTeachSpamAssassin = teachBefore_;
      }

      // ------------------------------------------------------------------ the refusal

      [Test]
      [Description("A trap is refused with the reply an address this server does not have gets - the same words, so nothing tells the sender which it was.")]
      public void ATrapIsRefusedExactlyAsAnUnknownAddressIs()
      {
         // The negative control FIRST, and that order is the test rather than an
         // accident of writing. A trap hit bans the connecting address, the suite
         // reaches this server from 127.0.0.1, and a banned address is refused at
         // accept: a second connection made after the trap had been addressed
         // would never see a 220 and the comparison below would be a real 550
         // against an empty string, which passes nothing and proves nothing.
         string toNobody = RcptReply("harvester@spammer.example", "nobody-at-all@" + _domain.Name);

         AddTrap(TrapAddress());

         string toTheTrap = RcptReply("harvester@spammer.example", TrapAddress());

         LiftEverySpamTrapBan();

         ClassicAssert.IsTrue(toNobody.Trim().Length > 0,
            "The reply to an address that does not exist was empty, so there is nothing to compare the trap's reply with.");

         StringAssert.Contains("550", toTheTrap, toTheTrap);
         StringAssert.Contains("Unknown user", toTheTrap, toTheTrap);
         ClassicAssert.AreEqual(toNobody.Trim(), toTheTrap.Trim(),
            "The trap answered differently from an address that simply does not exist, so a sender can tell the two apart and take the trap off their list. Trap: " +
            toTheTrap + " Unknown: " + toNobody);
      }

      [Test]
      [Description("A trap is matched through the domain's plus addressing, so a harvester that adds a tag still hits it.")]
      public void ATaggedTrapAddressIsATrapToo()
      {
         _domain.PlusAddressingEnabled = true;
         _domain.PlusAddressingCharacter = "+";
         _domain.Save();

         AddTrap(TrapAddress());

         string reply = RcptReply("harvester@spammer.example", TrapLocalPart + "+news@" + _domain.Name);
         LiftEverySpamTrapBan();

         StringAssert.Contains("Unknown user", reply, reply);

         List<Dictionary<string, object>> hits = Hits();
         ClassicAssert.AreEqual(1, hits.Count, "The tagged address did not reach the trap.");
         ClassicAssert.AreEqual(TrapAddress(), Text(hits[0], "address"));
      }

      // ---------------------------------------------------------------------- the ban

      [Test]
      [Description("The address that addressed a trap is banned through the server's own auto-ban: an expiring range, named so that the firewall reconciliation and the expiry sweep both know it.")]
      public void TheAddressThatAddressedATrapIsBanned()
      {
         AddTrap(TrapAddress());

         RcptReply("harvester@spammer.example", TrapAddress());

         SecurityRange ban = FindSpamTrapBan();

         try
         {
            ClassicAssert.IsNotNull(ban, "No auto-ban range was created for the address that addressed the trap.");
            ClassicAssert.IsTrue(ban.Expires, "A spam trap ban must expire, as an auto-ban from failed logons does.");
            StringAssert.StartsWith("Auto-ban: ", ban.Name, ban.Name);
            StringAssert.Contains(TrapAddress(), ban.Name, ban.Name);
         }
         finally
         {
            LiftEverySpamTrapBan();
         }

         List<Dictionary<string, object>> hits = Hits();
         ClassicAssert.AreEqual(1, hits.Count, "The hit was not recorded.");
         ClassicAssert.IsTrue(Flag(hits[0], "banned"), "The recorded hit says the address was not banned: " + Text(hits[0], "note"));
      }

      [Test]
      [Description("An address on AutoBanNeverBan is refused the trap and not banned - the list that exists so that a ban cannot lock an administrator out of the machine.")]
      public void AnAddressOnTheNeverBanListIsNotBanned()
      {
         AddTrap(TrapAddress());

         IniFileSetting.Write("AutoBanNeverBan", "127.0.0.1");
         _application.Reinitialize();

         try
         {
            string reply = RcptReply("harvester@spammer.example", TrapAddress());
            StringAssert.Contains("Unknown user", reply, reply);

            ClassicAssert.IsNull(FindSpamTrapBan(),
               "An address on AutoBanNeverBan was banned for addressing a spam trap.");

            List<Dictionary<string, object>> hits = Hits();
            ClassicAssert.AreEqual(1, hits.Count, "The hit was not recorded, so nothing says why the address was not banned.");
            ClassicAssert.IsFalse(Flag(hits[0], "banned"), Text(hits[0], "note"));
            StringAssert.Contains("AutoBanNeverBan", Text(hits[0], "note"), Text(hits[0], "note"));
         }
         finally
         {
            LiftEverySpamTrapBan();
            IniFileSetting.Delete("AutoBanNeverBan");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("One of our own people, signed in, is not banned for writing to a trap: that is a mistake in an address book, not an attack.")]
      public void AnAuthenticatedSessionIsNotBanned()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sender@" + _domain.Name, Password);

         AddTrap(TrapAddress());

         string reply = RcptReply(account.Address, TrapAddress(), account.Address, Password);

         try
         {
            StringAssert.Contains("Unknown user", reply, reply);

            ClassicAssert.IsNull(FindSpamTrapBan(), "An authenticated session was banned for addressing a spam trap.");

            List<Dictionary<string, object>> hits = Hits();
            ClassicAssert.AreEqual(1, hits.Count, "The hit was not recorded.");
            ClassicAssert.IsFalse(Flag(hits[0], "banned"), Text(hits[0], "note"));
            ClassicAssert.IsFalse(Flag(hits[0], "blocked"), Text(hits[0], "note"));
            StringAssert.Contains("authenticated", Text(hits[0], "note"), Text(hits[0], "note"));
         }
         finally
         {
            LiftEverySpamTrapBan();
         }
      }

      [Test]
      [Description("A client repeating one RCPT TO is refused every time and counted, recorded and banned once - the promise the metric's HELP text, ServerStatus.h and the documentation all make.")]
      public void RepeatingOneRcptToIsRecordedOnce()
      {
         AddTrap(TrapAddress());

         List<string> replies = RcptRepliesInOneSession("harvester@spammer.example", TrapAddress(), 3);

         try
         {
            ClassicAssert.AreEqual(3, replies.Count);

            foreach (string reply in replies)
               StringAssert.Contains("Unknown user", reply, reply);

            ClassicAssert.AreEqual(1, Hits().Count,
               "One trap addressed three times in one session was recorded more than once, so a client repeating one RCPT TO inflates the record - and the counter beside it.");

            ClassicAssert.AreEqual(1, CountSpamTrapBans(),
               "The same session was banned more than once for the same trap.");
         }
         finally
         {
            LiftEverySpamTrapBan();
         }
      }

      // -------------------------------------------------------------------- the sender

      [Test]
      [Description("The envelope sender goes on the server's block list with an expiry, so its mail to a real mailbox is held or marked rather than delivered unremarked.")]
      public void TheSenderIsBlockedForAWhile()
      {
         AddTrap(TrapAddress());

         RcptReply("harvester@spammer.example", TrapAddress());
         LiftEverySpamTrapBan();

         dynamic entry = FindServerListEntry("harvester@spammer.example");

         ClassicAssert.IsNotNull(entry, "The envelope sender was not added to the server's block list.");
         ClassicAssert.IsTrue((bool) entry.Blocked, "The sender was added to the list but not as blocked.");
         ClassicAssert.IsTrue((bool) entry.Expires, "A block a trap made must expire; SpamTrapBlockDays says when.");
         StringAssert.Contains(TrapAddress(), (string) entry.Reason, (string) entry.Reason);
      }

      [Test]
      [Description("A sender claiming one of this server's own domains is not blocked: that is a forgery, and the entry would block the real user's mail.")]
      public void ASenderInOneOfOurOwnDomainsIsNotBlocked()
      {
         AddTrap(TrapAddress());

         string forged = "postmaster@" + _domain.Name;

         RcptReply(forged, TrapAddress());
         LiftEverySpamTrapBan();

         ClassicAssert.IsNull(FindServerListEntry(forged),
            "A sender claiming one of our own domains was blocked, which blocks the real user rather than the spammer.");

         List<Dictionary<string, object>> hits = Hits();
         ClassicAssert.AreEqual(1, hits.Count);
         ClassicAssert.IsFalse(Flag(hits[0], "blocked"), Text(hits[0], "note"));
      }

      [Test]
      [Description("An allow entry an administrator wrote is not turned into a block by a trap hit: a decision somebody made outranks an automatic one.")]
      public void AnAllowedSenderStaysAllowed()
      {
         AddTrap(TrapAddress());

         dynamic list = ((dynamic) _settings.AntiSpam).SenderList;
         dynamic allowed = list.Add();
         allowed.Value = "spammer.example";
         allowed.Blocked = false;
         allowed.Reason = "the regression suite allowed it";
         allowed.Save();

         RcptReply("harvester@spammer.example", TrapAddress());
         LiftEverySpamTrapBan();

         dynamic entry = FindServerListEntry("spammer.example");

         ClassicAssert.IsNotNull(entry, "The allow entry disappeared.");
         ClassicAssert.IsFalse((bool) entry.Blocked, "A trap hit turned an administrator's allow entry into a block.");

         List<Dictionary<string, object>> hits = Hits();
         ClassicAssert.AreEqual(1, hits.Count);
         ClassicAssert.IsFalse(Flag(hits[0], "blocked"), Text(hits[0], "note"));
         StringAssert.Contains("allow list", Text(hits[0], "note"), Text(hits[0], "note"));
      }

      // ------------------------------------------------------- traps against real names

      [Test]
      [Description("An account, an alias and a distribution list cannot be given a trap's address.")]
      public void NothingRealCanBeGivenATrapsAddress()
      {
         AddTrap(TrapAddress());

         COMException refusedAccount = Assert.Throws<COMException>(() =>
         {
            Account account = _domain.Accounts.Add();
            account.Address = TrapAddress();
            account.Password = Password;
            account.Save();
         });

         StringAssert.Contains("spam trap", refusedAccount.Message, refusedAccount.Message);

         COMException refusedAlias = Assert.Throws<COMException>(() =>
         {
            Alias alias = _domain.Aliases.Add();
            alias.Name = TrapAddress();
            alias.Value = "somebody@" + _domain.Name;
            alias.Save();
         });

         StringAssert.Contains("spam trap", refusedAlias.Message, refusedAlias.Message);

         COMException refusedList = Assert.Throws<COMException>(() =>
         {
            DistributionList list = _domain.DistributionLists.Add();
            list.Address = TrapAddress();
            list.Active = true;
            list.Save();
         });

         StringAssert.Contains("spam trap", refusedList.Message, refusedList.Message);
      }

      [Test]
      [Description("And an address that is already an account cannot be made a trap - the other direction of the same rule.")]
      public void AnAddressSomebodyUsesCannotBeMadeATrap()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "real-person@" + _domain.Name, Password);

         COMException refused = Assert.Throws<COMException>(() => AddTrap(account.Address));

         StringAssert.Contains("already an account", refused.Message, refused.Message);

         ClassicAssert.AreEqual(string.Empty, Traps(), "The refused address was made a trap anyway.");
      }

      [Test]
      [Description("A trap must be an address in the domain it is declared on.")]
      public void ATrapBelongsToItsDomain()
      {
         COMException refused = Assert.Throws<COMException>(() => AddTrap("someone@somewhere-else.example"));

         StringAssert.Contains("must be an address in", refused.Message, refused.Message);
      }

      // -------------------------------------------------------------------- the record

      [Test]
      [Description("The listing says what was addressed, from where, by whom and what was done; a trap that has been removed records nothing more.")]
      public void TheHitIsListedAndATrapThatIsGoneRecordsNothing()
      {
         AddTrap(TrapAddress());

         RcptReply("harvester@spammer.example", TrapAddress());
         LiftEverySpamTrapBan();

         List<Dictionary<string, object>> hits = Hits();

         ClassicAssert.AreEqual(1, hits.Count, "The hit was not listed.");
         ClassicAssert.AreEqual(TrapAddress(), Text(hits[0], "address"));
         ClassicAssert.AreEqual(_domain.Name, Text(hits[0], "domain"));
         ClassicAssert.AreEqual("127.0.0.1", Text(hits[0], "ip_address"));
         ClassicAssert.AreEqual("harvester@spammer.example", Text(hits[0], "sender"));
         ClassicAssert.IsTrue(Text(hits[0], "note").Length > 0, "The hit says nothing about what was done.");

         // Removed through the domain, which is where a trap lives.
         bool removed = (bool) ((dynamic) _domain).RemoveSpamTrap(TrapAddress());
         ClassicAssert.IsTrue(removed, "The trap was not removed.");

         RcptReply("harvester@spammer.example", TrapAddress());
         LiftEverySpamTrapBan();

         ClassicAssert.AreEqual(1, Hits().Count,
            "An address that is no longer a trap was recorded as a hit.");
      }

      [Test]
      [Description("The record can be cleared, and clearing it removes rows rather than undoing what the hits did.")]
      public void TheRecordCanBeCleared()
      {
         AddTrap(TrapAddress());

         RcptReply("harvester@spammer.example", TrapAddress());
         LiftEverySpamTrapBan();

         ClassicAssert.AreEqual(1, Hits().Count);

         dynamic utilities = _application.Utilities;
         int removed = (int) utilities.ClearSpamTrapHits("");

         ClassicAssert.AreEqual(1, removed, "The clear did not say how many rows it removed.");
         ClassicAssert.AreEqual(0, Hits().Count);

         // What the hit made is not undone by clearing the record of it.
         ClassicAssert.IsNotNull(FindServerListEntry("harvester@spammer.example"),
            "Clearing the record of the hits unblocked the sender, which it must not do.");
      }

      // ------------------------------------------------------------------ the settings

      [Test]
      [Description("The settings refuse a value outside their range in the sentence an administrator reads, and take one inside it.")]
      public void TheSettingsJudgeWhatTheyAreGiven()
      {
         dynamic settings = _settings;

         COMException refusedBan = Assert.Throws<COMException>(() => { settings.SpamTrapBanMinutes = -1; });
         StringAssert.Contains("SpamTrapBanMinutes", refusedBan.Message, refusedBan.Message);

         COMException refusedBlock = Assert.Throws<COMException>(() => { settings.SpamTrapBlockDays = 4000; });
         StringAssert.Contains("SpamTrapBlockDays", refusedBlock.Message, refusedBlock.Message);

         COMException refusedRetention = Assert.Throws<COMException>(() => { settings.SpamTrapHitRetentionDays = 0; });
         StringAssert.Contains("SpamTrapHitRetentionDays", refusedRetention.Message, refusedRetention.Message);

         settings.SpamTrapBanMinutes = 120;
         settings.SpamTrapBlockDays = 0;
         settings.SpamTrapHitRetentionDays = 30;

         ClassicAssert.AreEqual(120, (int) settings.SpamTrapBanMinutes);
         ClassicAssert.AreEqual(0, (int) settings.SpamTrapBlockDays);
         ClassicAssert.AreEqual(30, (int) settings.SpamTrapHitRetentionDays);
      }

      [Test]
      [Description("SpamTrapBlockDays at 0 leaves the sender alone and still bans and records, which is what 0 is for.")]
      public void BlockDaysAtZeroBlocksNobody()
      {
         dynamic settings = _settings;
         settings.SpamTrapBlockDays = 0;

         AddTrap(TrapAddress());

         RcptReply("harvester@spammer.example", TrapAddress());
         LiftEverySpamTrapBan();

         ClassicAssert.IsNull(FindServerListEntry("harvester@spammer.example"),
            "SpamTrapBlockDays is 0 and the sender was blocked anyway.");

         List<Dictionary<string, object>> hits = Hits();
         ClassicAssert.AreEqual(1, hits.Count);
         ClassicAssert.IsTrue(Flag(hits[0], "banned"), Text(hits[0], "note"));
      }

      // ------------------------------------------------------------------ the lesson

      [Test]
      [Description("A session that addressed a trap and then sent a message to a real mailbox teaches SpamAssassin that message as spam: one TELL with Message-class: spam, to the spamd the anti-spam settings name. Against the suite's own spamd, which accepts TELL, so the bench's spamd needs no --allow-tell for this to be proven.")]
      public void TheMessageASessionSentAfterAddressingATrapTeachesSpamAssassin()
      {
         Account victim = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "victim@" + _domain.Name, Password);
         AddTrap(TrapAddress());

         dynamic settings = _settings;
         settings.SpamTrapTeachSpamAssassin = true;

         hMailServer.AntiSpam antiSpam = _settings.AntiSpam;
         bool enabledBefore = antiSpam.SpamAssassinEnabled;
         string hostBefore = antiSpam.SpamAssassinHost;
         int portBefore = antiSpam.SpamAssassinPort;

         using (var spamd = new SpamdSimulator())
         {
            antiSpam.SpamAssassinEnabled = true;
            antiSpam.SpamAssassinHost = "127.0.0.1";
            antiSpam.SpamAssassinPort = spamd.Port;

            try
            {
               // One session: the trap first (refused), then a real mailbox, then
               // the message. The lesson is given when the message is accepted.
               var socket = new TcpConnection();
               ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
               try
               {
                  socket.ReadUntil("220");
                  socket.Send("EHLO test\r\n");
                  socket.ReadUntil("250 ");
                  socket.Send("MAIL FROM:<harvester@spammer.example>\r\n");
                  socket.ReadUntil("250");
                  socket.Send("RCPT TO:<" + TrapAddress() + ">\r\n");
                  string refused = socket.Receive();
                  StringAssert.Contains("550", refused, refused);
                  socket.Send("RCPT TO:<" + victim.Address + ">\r\n");
                  string accepted = socket.Receive();
                  StringAssert.Contains("250", accepted, accepted);
                  socket.Send("DATA\r\n");
                  socket.ReadUntil("354");
                  socket.Send("From: harvester@spammer.example\r\nTo: " + victim.Address + "\r\nSubject: From a harvested list\r\n\r\nThe body of the lesson.\r\n.\r\n");
                  string queued = socket.ReadUntil("250");
                  StringAssert.Contains("250", queued, queued);
                  socket.Send("QUIT\r\n");
               }
               finally
               {
                  socket.Disconnect();
               }

               // The scan of the message (PROCESS) and the lesson (TELL) reach the
               // fake spamd in whichever order the work queue runs them.
               Dictionary<string, string> tell = null;
               for (int i = 0; i < 4 && tell == null; i++)
               {
                  Dictionary<string, string> request = spamd.WaitForRequest(i);
                  if (request["Request"].StartsWith("TELL", StringComparison.OrdinalIgnoreCase))
                     tell = request;
               }

               ClassicAssert.IsNotNull(tell, "SpamAssassin was never told about the message a trap-addressing session sent.");
               ClassicAssert.AreEqual("spam", tell["Message-class"], "The lesson's Message-class");
               ClassicAssert.AreEqual("local", tell["Set"], "Into spamd's local Bayes store, as the move-to-Junk lesson goes.");
            }
            finally
            {
               LiftEverySpamTrapBan();
               antiSpam.SpamAssassinEnabled = enabledBefore;
               antiSpam.SpamAssassinHost = hostBefore;
               antiSpam.SpamAssassinPort = portBefore;
            }
         }
      }

      // ---------------------------------------------------------------------- helpers

      private string TrapAddress()
      {
         return TrapLocalPart + "@" + _domain.Name;
      }

      private void AddTrap(string address)
      {
         ((dynamic) _domain).AddSpamTrap(address);
      }

      private string Traps()
      {
         return (string) ((dynamic) _domain).SpamTraps;
      }

      private void RemoveEveryTrap()
      {
         string traps = Traps();

         foreach (string line in traps.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries))
            ((dynamic) _domain).RemoveSpamTrap(line.Trim());
      }

      private List<Dictionary<string, object>> Hits()
      {
         dynamic utilities = _application.Utilities;
         string json = (string) utilities.GetSpamTrapHits("", 100);

         Dictionary<string, object> document = MiniJson.Object(json);
         var hits = new List<Dictionary<string, object>>();

         foreach (object item in MiniJson.List(document, "items"))
            hits.Add((Dictionary<string, object>) item);

         return hits;
      }

      private void ClearHits()
      {
         dynamic utilities = _application.Utilities;
         utilities.ClearSpamTrapHits("");
      }

      private static string Text(Dictionary<string, object> hit, string field)
      {
         object value;
         return hit.TryGetValue(field, out value) && value != null ? value.ToString() : string.Empty;
      }

      private static bool Flag(Dictionary<string, object> hit, string field)
      {
         object value;
         return hit.TryGetValue(field, out value) && value is bool && (bool) value;
      }

      /// <summary>
      ///    One RCPT TO, and the reply to it. Raw, because the point of most of these
      ///    tests is the text of that reply and what the server did before sending it.
      /// </summary>
      private static string RcptReply(string sender, string recipient, string username = null, string password = null)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP. An auto-ban left behind by an earlier test refuses this.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("EHLO test\r\n");
            socket.ReadUntil("250 ");

            if (username != null)
            {
               string plain = Convert.ToBase64String(Encoding.ASCII.GetBytes("\0" + username + "\0" + password));
               socket.Send("AUTH PLAIN " + plain + "\r\n");
               string authenticated = socket.ReadUntil("235");
               StringAssert.Contains("235", authenticated, authenticated);
            }

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            socket.ReadUntil("250");

            socket.Send("RCPT TO:<" + recipient + ">\r\n");
            string reply = socket.Receive();

            socket.Send("QUIT\r\n");

            return reply;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      /// <summary>
      ///    One session, one MAIL FROM, the same RCPT TO several times, and every
      ///    reply to it.
      /// </summary>
      private static List<string> RcptRepliesInOneSession(string sender, string recipient, int times)
      {
         var replies = new List<string>();

         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP. An auto-ban left behind by an earlier test refuses this.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("EHLO test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            socket.ReadUntil("250");

            for (int i = 0; i < times; i++)
            {
               socket.Send("RCPT TO:<" + recipient + ">\r\n");
               replies.Add(socket.Receive());
            }

            socket.Send("QUIT\r\n");
         }
         finally
         {
            socket.Disconnect();
         }

         return replies;
      }

      private int CountSpamTrapBans()
      {
         _settings.SecurityRanges.Refresh();

         int bans = 0;

         for (int i = 0; i < _settings.SecurityRanges.Count; i++)
         {
            if (_settings.SecurityRanges[i].Name.StartsWith("Auto-ban: spam trap", StringComparison.OrdinalIgnoreCase))
               bans++;
         }

         return bans;
      }

      private SecurityRange FindSpamTrapBan()
      {
         _settings.SecurityRanges.Refresh();

         for (int i = 0; i < _settings.SecurityRanges.Count; i++)
         {
            SecurityRange range = _settings.SecurityRanges[i];

            if (range.Name.StartsWith("Auto-ban: spam trap", StringComparison.OrdinalIgnoreCase))
               return range;
         }

         return null;
      }

      private void LiftEverySpamTrapBan()
      {
         while (true)
         {
            SecurityRange ban = FindSpamTrapBan();

            if (ban == null)
               return;

            ban.Delete();
         }
      }

      private dynamic FindServerListEntry(string value)
      {
         dynamic list = ((dynamic) _settings.AntiSpam).SenderList;
         list.Refresh();

         for (int i = 0; i < (int) list.Count; i++)
         {
            dynamic entry = list[i];

            if (string.Equals((string) entry.Value, value, StringComparison.OrdinalIgnoreCase))
               return entry;
         }

         return null;
      }

      /// <summary>
      ///    The entries these tests make, and only those: the two senders they use
      ///    by name, and anything a trap hit wrote (its reason names the trap). The
      ///    server's list belongs to whoever runs this server, and a test that
      ///    empties it has no way to put back what was on it.
      /// </summary>
      private void RemoveOurServerListEntries()
      {
         dynamic list = ((dynamic) _settings.AntiSpam).SenderList;
         list.Refresh();

         var ours = new List<int>();

         for (int i = 0; i < (int) list.Count; i++)
         {
            dynamic entry = list[i];

            string value = (string) entry.Value ?? string.Empty;
            string reason = (string) entry.Reason ?? string.Empty;

            bool mine = reason.IndexOf("the spam trap", StringComparison.OrdinalIgnoreCase) >= 0;

            foreach (string known in OurSenders)
            {
               if (string.Equals(value, known, StringComparison.OrdinalIgnoreCase))
                  mine = true;
            }

            if (mine)
               ours.Add((int) entry.ID);
         }

         foreach (int id in ours)
            list.DeleteByDBID(id);
      }
   }
}
