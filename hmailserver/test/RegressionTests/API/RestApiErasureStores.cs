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
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    An address's erasure (POST /api/v1/addresses/erase, AddressTraceEraser.h)
   ///    reaching the stores that name an address beside the mailbox:
   ///
   ///    - a message held for review is discarded, as the quarantine's entries are -
   ///      its queue row and its queue file gone - whether the address is among its
   ///      recipients, reached through an alias (which only the queue's recipient
   ///      rows say), or its sender; and the sending account forgets the address as
   ///      one it has written to;
   ///    - a message that expires loses the address from its recipients and from who
   ///      opened it, and goes on being read by the others; one sent to the address
   ///      alone, and one the address sent, is ended with its kept copy; the one-time
   ///      codes the address asked for are forgotten; an address with a [ in it is
   ///      found as well;
   ///    - a password-protected message to it, with its ciphertext, and the reply it
   ///      wrote, are ended, while one the address's own account sent is not;
   ///    - the S/MIME certificate and the OpenPGP key another account keeps of it as a
   ///      correspondent are removed, beside another correspondent's that stay, and
   ///      the address's own certificate and key (kind 1) stay;
   ///    - an out-of-office reply that answered it answers it again.
   ///
   ///    Each beside its negative control: an address that is only part of the erased
   ///    one - ann inside joann, which LIKE alone would match - removes nothing.
   ///
   ///    And a legal hold outranks the erasure: a message held for review, a message
   ///    that expires and a password-protected message of a held account stay, the
   ///    answer counts them as held and the application log says so; the case closed,
   ///    the same erasure ends them.
   ///
   ///    Every case this fixture opens is closed in its teardown, which releases its
   ///    holds - a hold left standing would keep the next test's setup from deleting
   ///    the test domain - and its rows are removed, as the discovery fixture removes
   ///    its own. What enforce mode held is discarded and the accounts it locked are
   ///    let go, as the compromised-account fixture does; the settings go back first,
   ///    each step on its own, so that nothing a step throws leaves enforce mode on.
   /// </summary>
   [TestFixture]
   public class RestApiErasureStores : TestFixtureBase
   {
      // The ports this fixture may use are 10160 to 10169; the REST listener is the first.
      private static int RestPort = 10160;
      private const string AdminPassword = "testar";
      private const string Password = "Erasure-Passw0rd!";

      // The shipped values, put back after every test.
      private const string CompromiseDefaults = "{\"compromise_detection_mode\":\"watch\",\"compromise_volume_floor\":30,\"compromise_recipient_floor\":20}";

      // A symmetric OpenPGP message, as RestApiProtectedMessages takes it: the server
      // checks only its shape.
      private const string Vector =
         "-----BEGIN PGP MESSAGE-----\r\n" +
         "\r\n" +
         "wy4ECQMIDhJlWZpqtOn//QKmQPPvj1yaROry6YGBUB8MzkD0MHUdgdp+pnBWPeY0\r\n" +
         "0sBgAYVwPMhCdjpNBsnsq5HpCBOwwtnQOyOtMs8Uy30X4BirtxKdzuGSLDIf6vYd\r\n" +
         "D11hoBg1x9yzbY1TMs5uYgVBwr1g7aEwvF69+Da2fLm+RgKilQRD1pKRrIQ/0Tud\r\n" +
         "L8JDlqOySn1rpaZRHxoj3CaueWA23l3AOgKE8GgnRiEQP4k6xaveUVsfrNAHn1vh\r\n" +
         "YI+E9cqDSR2S+HRl+Up3Fl9OiaJhhwVEfqWthGduKCy0/A7G08kK5b14gjXXRN4u\r\n" +
         "32MUpgsD1B6krUyOIvvuoAmBE3pUfxAkboQO1UqKZnsP1INxNQwkyNWBXmczqr0j\r\n" +
         "iaU94FyU6HQmh+UWyJFf+b+FsHdHlPd8RjrRoxYRKeHwVpO/D790yPWnbj9Wy5QD\r\n" +
         "9soF\r\n" +
         "=hiVe\r\n" +
         "-----END PGP MESSAGE-----\r\n";

      // A key the S/MIME store takes as the page wraps one; the server stores it and
      // cannot open it.
      private const string WrappedKey = "{\"v\":1,\"kdf\":\"PBKDF2-SHA256\",\"iterations\":600000,\"salt\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"iv\":\"AAAAAAAAAAAAAAAA\",\"data\":\"AQIDBA==\"}";

      private readonly List<string> _locked = new List<string>();
      private readonly List<long> _cases = new List<long>();

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         ClassicAssert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         _locked.Clear();
         _cases.Clear();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // The settings first, each on its own: a later step that throws must not
            // leave enforce mode, with its floors of three, on for every fixture after.
            Tidy(() => Admin("PUT", "/api/v1/settings", CompromiseDefaults));
            Tidy(() => Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"\"}"));
            Tidy(() => Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}"));

            foreach (long caseId in _cases)
            {
               Tidy(() => Admin("POST", "/api/v1/discovery/cases/" + caseId + "/close", "{}"));
               Tidy(() => _application.Database.ExecuteSQL("delete from hm_discoveryholds where holdcaseid = " + caseId));
               Tidy(() => _application.Database.ExecuteSQL("delete from hm_discoverysearches where searchcaseid = " + caseId));
               Tidy(() => _application.Database.ExecuteSQL("delete from hm_discoverycases where caseid = " + caseId));
            }
            _cases.Clear();

            Tidy(() =>
            {
               (int status, string body) held = Admin("GET", "/api/v1/review", null);
               if (held.status == 200)
               {
                  foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(held.body), "messages").Cast<Dictionary<string, object>>())
                     Tidy(() => Admin("DELETE", "/api/v1/review/" + Convert.ToInt64(entry["id"]), null));
               }
            });

            foreach (string address in _locked)
               Tidy(() => Admin("POST", "/api/v1/accounts/" + Uri.EscapeDataString(address) + "/unlock", null));
            _locked.Clear();

            Tidy(() => CustomAsserts.AssertRecipientsInDeliveryQueue(0));
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---------------------------------------------------------------- held for review

      [Test]
      [Description("A message held for review whose recipients name the address is discarded by its erasure, as the quarantine's entries are - its queue row and its queue file gone - and the sending account forgets the address as one it has written to; an address that is only part of it touches neither.")]
      public void AHeldMessageAndTheRememberedRecipientGoWithTheWholeAddressOnly()
      {
         EnforceWithLowFloors();

         Account sender = AddAccount("erase-sender");
         Account first = AddAccount("erase-one");
         Account second = AddAccount("erase-two");
         Account erased = AddAccount("joann.erase");
         string part = "ann.erase@" + _domain.Name;
         _locked.Add(sender.Address);

         Held held = HoldTheThird(sender, first, second, erased.Address);
         string queueFile = QueueFileOf(held.MessageId);
         ClassicAssert.IsTrue(File.Exists(queueFile), "The held message is queued, with its file: " + queueFile);
         ClassicAssert.AreEqual(3, RecipientsKnown(sender.Address), "The account remembers writing to all three.");

         // ann.erase@ is inside joann.erase@: LIKE finds it there, the whole-entry match does not.
         Dictionary<string, object> control = Erase(part);
         ClassicAssert.AreEqual(0, MiniJson.Number(control, "removed"), "Nothing names the shorter address.");
         CollectionAssert.AreEqual(new[] { held.Id }, HeldIds(sender.Address), "The held message is not the shorter address's.");
         ClassicAssert.AreEqual(3, RecipientsKnown(sender.Address), "Nor is the remembered recipient.");

         Dictionary<string, object> answer = Erase(erased.Address);
         ClassicAssert.IsNotNull(MiniJson.At(answer, "held"), "The answer says what a hold kept, even when it is nothing.");
         ClassicAssert.GreaterOrEqual(MiniJson.Number(answer, "removed"), 2, "The held message and the remembered recipient at least.");
         ClassicAssert.AreEqual(0, MiniJson.Number(answer, "held"), "Nothing here is under a legal hold.");
         CollectionAssert.IsEmpty(HeldIds(sender.Address), "The held message was discarded.");
         ClassicAssert.AreEqual(404, Admin("POST", "/api/v1/review/" + held.Id + "/release", null).status, "Discarded, it cannot be released to the erased address.");
         ClassicAssert.AreEqual("", _application.Database.UtilGetFileNameByMessageID(held.MessageId), "The message's row left the queue.");
         ClassicAssert.IsFalse(File.Exists(queueFile), "And its file, which names the address, went with it: " + queueFile);
         ClassicAssert.AreEqual(2, RecipientsKnown(sender.Address), "The account forgot the erased address and remembers the other two.");
      }

      [Test]
      [Description("A message held for review is discarded when the erased address sent it, and when it reached the erased address through an alias, which the review row's recipients do not name and the queue's recipient rows do; the shorter address inside each touches neither.")]
      public void AHeldMessageTheAddressSentOrThatReachedItThroughAnAliasIsDiscarded()
      {
         EnforceWithLowFloors();

         // The address as the sender of the held message.
         Account sends = AddAccount("joann.sends");
         Account first = AddAccount("sends-one");
         Account second = AddAccount("sends-two");
         Account third = AddAccount("sends-three");
         _locked.Add(sends.Address);

         Held fromErased = HoldTheThird(sends, first, second, third.Address);
         ClassicAssert.AreEqual(0, MiniJson.Number(Erase("ann.sends@" + _domain.Name), "removed"), "Nothing names the shorter address.");
         CollectionAssert.AreEqual(new[] { fromErased.Id }, HeldIds(sends.Address));
         Erase(sends.Address);
         CollectionAssert.IsEmpty(HeldIds(sends.Address), "The message the erased address sent was discarded.");
         ClassicAssert.AreEqual("", _application.Database.UtilGetFileNameByMessageID(fromErased.MessageId), "Its row left the queue.");

         // The address reached through an alias: the review row names the alias only.
         Account aliasSender = AddAccount("alias-sender");
         Account aliasOne = AddAccount("alias-one");
         Account aliasTwo = AddAccount("alias-two");
         Account target = AddAccount("joann.target");
         string alias = "joann.alias@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, alias, target.Address);
         _locked.Add(aliasSender.Address);

         Held throughAlias = HoldTheThird(aliasSender, aliasOne, aliasTwo, alias);
         ClassicAssert.IsFalse(throughAlias.Recipients.ToLowerInvariant().Contains(target.Address.ToLowerInvariant()),
            "The review row names the alias and not the account behind it: " + throughAlias.Recipients);
         ClassicAssert.AreEqual(0, MiniJson.Number(Erase("ann.target@" + _domain.Name), "removed"), "Nothing names the shorter address.");
         CollectionAssert.AreEqual(new[] { throughAlias.Id }, HeldIds(aliasSender.Address));
         Erase(target.Address);
         CollectionAssert.IsEmpty(HeldIds(aliasSender.Address), "The message that would have reached the erased address through the alias was discarded.");
      }

      [Test]
      [Description("A legal hold outranks the erasure: a message held for review from an account under a hold stays, the answer counts it as held and the application log says so; the case closed, the same erasure discards it.")]
      public void ALegalHoldKeepsTheHeldMessageAndTheAnswerSaysSo()
      {
         EnforceWithLowFloors();

         Account sender = AddAccount("hold-sender");
         Account first = AddAccount("hold-one");
         Account second = AddAccount("hold-two");
         Account erased = AddAccount("hold-third");
         _locked.Add(sender.Address);

         Held held = HoldTheThird(sender, first, second, erased.Address);

         long caseId = OpenCase("Erasure under a legal hold");
         (int status, string body) placed = Admin("POST", "/api/v1/discovery/cases/" + caseId + "/holds",
            "{\"scope\":\"account\",\"target\":\"" + sender.Address + "\"}");
         ClassicAssert.AreEqual(201, placed.status, placed.body);

         Dictionary<string, object> kept = Erase(erased.Address);
         ClassicAssert.GreaterOrEqual(MiniJson.Number(kept, "held"), 1, "The held account's message is counted as kept by the hold.");
         CollectionAssert.AreEqual(new[] { held.Id }, HeldIds(sender.Address), "It is still there, for the review and for the case.");
         StringAssert.Contains("in place under a legal hold", LogHandler.ReadCurrentDefaultLog(), "The application log says what the hold kept.");

         (int status, string body) closed = Admin("POST", "/api/v1/discovery/cases/" + caseId + "/close", "{}");
         ClassicAssert.AreEqual(200, closed.status, closed.body);

         Dictionary<string, object> released = Erase(erased.Address);
         ClassicAssert.AreEqual(0, MiniJson.Number(released, "held"), "No hold stands now.");
         ClassicAssert.GreaterOrEqual(MiniJson.Number(released, "removed"), 1, "The held message is discarded now.");
         CollectionAssert.IsEmpty(HeldIds(sender.Address), "The hold released, the erasure discards it.");
         ClassicAssert.AreEqual("", _application.Database.UtilGetFileNameByMessageID(held.MessageId), "Its row left the queue.");
      }

      [Test]
      [Description("A legal hold outranks the erasure for the stores beside the mailbox too: a message that expires and a password-protected message the held account sent to the address stay as they are and are counted as held; the case closed, the same erasure ends them.")]
      public void ALegalHoldKeepsTheHeldAccountsExpiringAndProtectedMessages()
      {
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"http://127.0.0.1:" + RestPort + "\"}").status);
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}").status);

         Account sender = AddAccount("kept-sender");
         Account erased = AddAccount("kept-recipient");
         string me = Basic(sender.Address, Password);
         string erasedLower = erased.Address.ToLowerInvariant();

         ClassicAssert.AreEqual(201, User("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + erased.Address + "\",\"subject\":\"Held expiring\",\"text\":\"Kept by the hold.\",\"expires_in\":3600}").status);
         ClassicAssert.AreEqual(201, User("POST", "/api/v1/me/protected", me,
            "{\"recipient\":\"" + erased.Address + "\",\"hint\":\"the usual word\",\"size\":100}").status);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         string kept = KeptPath(sender, TokenOf(ConfidentialItem(me, "Held expiring")));
         ClassicAssert.IsTrue(File.Exists(kept), kept);

         long caseId = OpenCase("Erasure beside the mailbox under a legal hold");
         (int status, string body) placed = Admin("POST", "/api/v1/discovery/cases/" + caseId + "/holds",
            "{\"scope\":\"account\",\"target\":\"" + sender.Address + "\"}");
         ClassicAssert.AreEqual(201, placed.status, placed.body);

         Dictionary<string, object> held = Erase(erased.Address);
         ClassicAssert.GreaterOrEqual(MiniJson.Number(held, "held"), 2, "The message that expires and the password-protected one are each counted as kept by the hold.");
         CollectionAssert.AreEqual(new[] { erasedLower }, RecipientsOf(ConfidentialItem(me, "Held expiring")), "The message that expires is left as it was.");
         ClassicAssert.IsTrue(File.Exists(kept), "With its kept copy: " + kept);
         CollectionAssert.AreEqual(new[] { erasedLower }, ProtectedRecipients(me), "And the password-protected one.");

         (int status, string body) closed = Admin("POST", "/api/v1/discovery/cases/" + caseId + "/close", "{}");
         ClassicAssert.AreEqual(200, closed.status, closed.body);

         Dictionary<string, object> released = Erase(erased.Address);
         ClassicAssert.AreEqual(0, MiniJson.Number(released, "held"), "No hold stands now.");
         ClassicAssert.GreaterOrEqual(MiniJson.Number(released, "removed"), 2, "Both are ended now.");
         ClassicAssert.IsNull(ConfidentialItem(me, "Held expiring"), "The message that expires, sent to the erased address alone, is ended.");
         ClassicAssert.IsFalse(File.Exists(kept), "Its kept copy was deleted: " + kept);
         CollectionAssert.IsEmpty(ProtectedRecipients(me), "The password-protected one is ended.");
      }

      // ---------------------------------------------------------------- messages that expire

      [Test]
      [Description("A message that expires to the address and another loses the address from its recipients and from who opened it, and still opens for the other; one to the address alone and one the address sent are ended with their kept copies; the address's one-time codes are forgotten; an address with a [ in it is found; an address that is only part of it changes nothing.")]
      public void AMessageThatExpiresLosesTheAddressOrEndsWhenNobodyIsLeft()
      {
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"http://127.0.0.1:" + RestPort + "\"}").status);

         Account sender = AddAccount("expires-sender");
         Account keeper = AddAccount("expires-keeper");
         Account erased = AddAccount("joann.expires");
         string part = "ann.expires@" + _domain.Name;
         string me = Basic(sender.Address, Password);
         string asErased = Basic(erased.Address, Password);
         string erasedLower = erased.Address.ToLowerInvariant();

         ClassicAssert.AreEqual(201, User("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + erased.Address + ", " + keeper.Address + "\",\"subject\":\"To both\",\"text\":\"For the two of you.\",\"expires_in\":3600}").status);
         ClassicAssert.AreEqual(201, User("POST", "/api/v1/me/messages", me,
            "{\"to\":\"" + erased.Address + "\",\"subject\":\"Just you\",\"text\":\"For you alone.\",\"expires_in\":3600}").status);
         ClassicAssert.AreEqual(201, User("POST", "/api/v1/me/messages", asErased,
            "{\"to\":\"" + keeper.Address + "\",\"subject\":\"From joann\",\"text\":\"Sent by the erased address.\",\"expires_in\":3600}").status);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         string sharedToken = TokenOf(ConfidentialItem(me, "To both"));
         string ownToken = TokenOf(ConfidentialItem(me, "Just you"));
         string sentToken = TokenOf(ConfidentialItem(asErased, "From joann"));
         string sharedKept = KeptPath(sender, sharedToken);
         string ownKept = KeptPath(sender, ownToken);
         string sentKept = KeptPath(erased, sentToken);
         ClassicAssert.IsTrue(File.Exists(sharedKept), sharedKept);
         ClassicAssert.IsTrue(File.Exists(ownKept), ownKept);
         ClassicAssert.IsTrue(File.Exists(sentKept), sentKept);

         // The erased address opened the shared message (as MarkOpened records it),
         // and asked for a code to open it: the second ask inside a minute is refused,
         // which is how the code's entry is seen.
         _application.Database.ExecuteSQL("update hm_confidential set cfopened = 1700000000, cfopenedby = '" + erasedLower + "' where cftoken = '" + sharedToken + "'");
         ClassicAssert.AreEqual(200, AskForCode(sharedToken, erased.Address), "A code is sent to a recipient's address.");
         ClassicAssert.AreEqual(429, AskForCode(sharedToken, erased.Address), "A second inside a minute is refused: the address's entry is there.");

         // An address with a [ in its local part, which SQL Server's LIKE reads as a
         // character class: the row is made as ConfidentialMessages::Create writes one.
         string bracket = "x[1].expires@" + _domain.Name.ToLowerInvariant();
         string bracketToken = Guid.NewGuid().ToString("N") + Guid.NewGuid().ToString("N");
         _application.Database.ExecuteSQL(
            "insert into hm_confidential (cfaccountid, cftoken, cfsubject, cfsender, cfrecipients, cffile, cfpasscode, cfcreated, cfexpires, cfopened, cfopenedby, cfrevoked) " +
            "values (" + sender.ID + ", '" + bracketToken + "', 'Bracketed', '" + sender.Address + "', '" + bracket + "," + keeper.Address.ToLowerInvariant() + "', '', '', 1700000000, 4000000000, 0, '', 0)");
         CollectionAssert.Contains(RecipientsOf(ConfidentialItem(me, "Bracketed")), bracket);

         Dictionary<string, object> control = Erase(part);
         ClassicAssert.AreEqual(0, MiniJson.Number(control, "removed"), "Nothing names the shorter address.");
         CollectionAssert.Contains(RecipientsOf(ConfidentialItem(me, "To both")), erasedLower);
         CollectionAssert.Contains(RecipientsOf(ConfidentialItem(me, "Just you")), erasedLower);
         ClassicAssert.AreEqual(erasedLower, MiniJson.Text(ConfidentialItem(me, "To both"), "opened_by"));
         ClassicAssert.IsNotNull(ConfidentialItem(asErased, "From joann"));
         ClassicAssert.AreEqual(429, AskForCode(sharedToken, erased.Address), "The shorter address's erasure left the longer address's code alone.");

         Dictionary<string, object> answer = Erase(erased.Address);
         ClassicAssert.GreaterOrEqual(MiniJson.Number(answer, "removed"), 3, "Three messages that expire were changed or ended.");

         Dictionary<string, object> shared = ConfidentialItem(me, "To both");
         ClassicAssert.IsNotNull(shared, "The message to both is still the sender's.");
         CollectionAssert.AreEqual(new[] { keeper.Address.ToLowerInvariant() }, RecipientsOf(shared), "The erased address is out of its recipients; the other stays.");
         ClassicAssert.AreEqual("[erased]", MiniJson.Text(shared, "opened_by"), "Who opened it says a recipient did, and that the name was erased.");
         ClassicAssert.IsTrue(File.Exists(sharedKept), "Its kept copy stays for the other recipient: " + sharedKept);
         ClassicAssert.AreEqual(200, Page(sharedToken), "The other recipient's link still opens the page.");
         ClassicAssert.AreEqual(200, AskForCode(sharedToken, erased.Address), "The erased address's code entry was forgotten: asking again is a first ask.");

         ClassicAssert.IsNull(ConfidentialItem(me, "Just you"), "The message to the erased address alone is gone from the sender's list.");
         ClassicAssert.IsFalse(File.Exists(ownKept), "Its kept copy was deleted: " + ownKept);
         ClassicAssert.AreEqual(404, Page(ownToken), "Its link names nothing now.");

         ClassicAssert.IsNull(ConfidentialItem(asErased, "From joann"), "The message the erased address sent is ended.");
         ClassicAssert.IsFalse(File.Exists(sentKept), "Its kept copy was deleted: " + sentKept);

         Erase(bracket);
         CollectionAssert.AreEqual(new[] { keeper.Address.ToLowerInvariant() }, RecipientsOf(ConfidentialItem(me, "Bracketed")), "The address with a [ was found and taken out.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      // ---------------------------------------------------------------- password-protected

      [Test]
      [Description("A password-protected message to the address, with its ciphertext, and the reply the address wrote on its reading page are ended; one to somebody else stays; one the address's own account sent is not reached; an address that is only part of it ends nothing.")]
      public void APasswordProtectedMessageToTheAddressAndItsReplyEnd()
      {
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"confidential_base_url\":\"http://127.0.0.1:" + RestPort + "\"}").status);
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}").status);

         Account sender = AddAccount("protected-sender");
         Account keeper = AddAccount("protected-keeper");
         Account erased = AddAccount("joann.protected");
         string part = "ann.protected@" + _domain.Name;
         string me = Basic(sender.Address, Password);
         string asErased = Basic(erased.Address, Password);

         // The message to the erased address, uploaded whole, and the reply written
         // on its reading page - filed under the sender, naming the erased address.
         (int status, string body) toErased = User("POST", "/api/v1/me/protected", me,
            "{\"recipient\":\"" + erased.Address + "\",\"hint\":\"the usual word\",\"size\":" + Vector.Length + "}");
         ClassicAssert.AreEqual(201, toErased.status, toErased.body);
         long id = long.Parse(Regex.Match(toErased.body, "\"id\":(\\d+)").Groups[1].Value);
         string token = Regex.Match(toErased.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;
         (int status, string body) uploaded = User("PUT", "/api/v1/me/protected/" + id + "/content?offset=0", me, Vector);
         ClassicAssert.AreEqual(200, uploaded.status, uploaded.body);
         StringAssert.Contains("\"complete\":true", uploaded.body);
         (int status, string body) reply = User("POST", "/api/v1/protected/" + token + "/reply", null, "{\"message\":\"" + Escaped(Vector) + "\"}");
         ClassicAssert.AreEqual(201, reply.status, reply.body);
         string replyToken = Regex.Match(reply.body, "\"token\":\"([^\"]+)\"").Groups[1].Value;

         (int status, string body) toKeeper = User("POST", "/api/v1/me/protected", me,
            "{\"recipient\":\"" + keeper.Address + "\",\"hint\":\"the usual word\",\"size\":100}");
         ClassicAssert.AreEqual(201, toKeeper.status, toKeeper.body);

         // One the erased address's own account sent: its row names its recipient
         // and no sender, and goes with the account (docs/PersonalData.md).
         ClassicAssert.AreEqual(201, User("POST", "/api/v1/me/protected", asErased,
            "{\"recipient\":\"" + keeper.Address + "\",\"hint\":\"the usual word\",\"size\":100}").status);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         string stored = Path.Combine(_settings.Directories.DataDirectory, "Protected", sender.ID.ToString(), token + ".asc");
         string storedReply = Path.Combine(_settings.Directories.DataDirectory, "Protected", sender.ID.ToString(), replyToken + ".asc");
         ClassicAssert.IsTrue(File.Exists(stored), stored);
         ClassicAssert.IsTrue(File.Exists(storedReply), storedReply);
         string erasedLower = erased.Address.ToLowerInvariant();
         string keeperLower = keeper.Address.ToLowerInvariant();
         CollectionAssert.AreEquivalent(new[] { erasedLower, erasedLower, keeperLower }, ProtectedRecipients(me), "The message, the reply and the other message.");

         Dictionary<string, object> control = Erase(part);
         ClassicAssert.AreEqual(0, MiniJson.Number(control, "removed"), "Nothing names the shorter address.");
         CollectionAssert.AreEquivalent(new[] { erasedLower, erasedLower, keeperLower }, ProtectedRecipients(me));

         Dictionary<string, object> answer = Erase(erased.Address);
         ClassicAssert.GreaterOrEqual(MiniJson.Number(answer, "removed"), 2, "The message to the erased address and its reply were ended.");
         CollectionAssert.AreEqual(new[] { keeperLower }, ProtectedRecipients(me), "The message to the other recipient stays.");
         ClassicAssert.IsFalse(File.Exists(stored), "The ciphertext was deleted: " + stored);
         ClassicAssert.IsFalse(File.Exists(storedReply), "And the reply's: " + storedReply);
         CollectionAssert.AreEqual(new[] { keeperLower }, ProtectedRecipients(asErased), "The erased address's own account's message is not reached.");
      }

      // ---------------------------------------------------------------- keys and memories

      [Test]
      [Description("The S/MIME certificate and the OpenPGP key another account keeps of the address as a correspondent are removed and another correspondent's certificate and key stay; the address's own certificate and key (kind 1) stay; an out-of-office reply that answered the address once answers it again after the erasure; an address that is only part of it removes nothing.")]
      public void TheCorrespondentsKeysAndTheOutOfOfficeMemoryForgetTheAddress()
      {
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true,\"protected_message_days\":30}").status);

         Account holder = AddAccount("keys-holder");
         Account keeper = AddAccount("keys-keeper");
         Account erased = AddAccount("joann.keys");
         string part = "ann.keys@" + _domain.Name;
         string me = Basic(holder.Address, Password);
         string asErased = Basic(erased.Address, Password);
         string certificate = Convert.ToBase64String(new byte[] { 0x30, 0x03, 0x02, 0x01, 0x01 });
         string erasedLower = erased.Address.ToLowerInvariant();
         string keeperLower = keeper.Address.ToLowerInvariant();

         // Kept by the holder of its two correspondents: kind 2.
         (int status, string body) erasedCertificate = User("PUT", "/api/v1/me/smime/recipients", me,
            "{\"address\":\"" + erased.Address + "\",\"name\":\"Joann\",\"fingerprint\":\"" + new string('c', 64) + "\",\"certificate\":\"" + certificate + "\",\"not_after\":1900000000}");
         ClassicAssert.AreEqual(201, erasedCertificate.status, erasedCertificate.body);
         (int status, string body) keeperCertificate = User("PUT", "/api/v1/me/smime/recipients", me,
            "{\"address\":\"" + keeper.Address + "\",\"name\":\"Keeper\",\"fingerprint\":\"" + new string('d', 64) + "\",\"certificate\":\"" + certificate + "\",\"not_after\":1900000000}");
         ClassicAssert.AreEqual(201, keeperCertificate.status, keeperCertificate.body);
         (int status, string body) erasedKey = User("PUT", "/api/v1/me/openpgp/keys/correspondents", me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"address\":\"" + erased.Address + "\",\"source\":\"imported\"}");
         ClassicAssert.AreEqual(201, erasedKey.status, erasedKey.body);
         (int status, string body) keeperKey = User("PUT", "/api/v1/me/openpgp/keys/correspondents", me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\",\"address\":\"" + keeper.Address + "\",\"source\":\"imported\"}");
         ClassicAssert.AreEqual(201, keeperKey.status, keeperKey.body);

         // The erased address's own, under its own address: kind 1.
         (int status, string body) ownCertificate = User("PUT", "/api/v1/me/smime/own", asErased,
            "{\"address\":\"" + erased.Address + "\",\"name\":\"Joann\",\"fingerprint\":\"" + new string('e', 64) + "\",\"certificate\":\"" + certificate + "\",\"chain\":[],\"key\":" + WrappedKey + "}");
         ClassicAssert.AreEqual(201, ownCertificate.status, ownCertificate.body);
         (int status, string body) ownKey = User("PUT", "/api/v1/me/openpgp/keys/own", asErased,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\",\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"," +
            "\"address\":\"" + erased.Address + "\",\"name\":\"Joann\",\"source\":\"imported\"}");
         ClassicAssert.AreEqual(201, ownKey.status, ownKey.body);

         Dictionary<string, object> control = Erase(part);
         ClassicAssert.AreEqual(0, MiniJson.Number(control, "removed"), "Nothing names the shorter address.");
         CollectionAssert.AreEquivalent(new[] { erasedLower, keeperLower }, CertificateAddresses(me, "recipients"));
         CollectionAssert.AreEquivalent(new[] { erasedLower, keeperLower }, KeyAddresses(me, "correspondents"));

         // The out-of-office reply answers the address once, and not a second time.
         holder.VacationMessageIsOn = true;
         holder.VacationSubject = "Away";
         holder.VacationMessage = "Back next week.";
         holder.Save();

         new SmtpClientSimulator().Send(erased.Address, holder.Address, "One", "Body");
         Pop3ClientSimulator.AssertMessageCount(erased.Address, Password, 1);
         new SmtpClientSimulator().Send(erased.Address, holder.Address, "Two", "Body");
         Pop3ClientSimulator.AssertMessageCount(holder.Address, Password, 2);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(erased.Address, Password, 1);

         Dictionary<string, object> answer = Erase(erased.Address);
         ClassicAssert.GreaterOrEqual(MiniJson.Number(answer, "removed"), 3, "The certificate, the key and the out-of-office memory at least.");
         CollectionAssert.AreEqual(new[] { keeperLower }, CertificateAddresses(me, "recipients"), "The erased correspondent's certificate is gone; the other's stays.");
         CollectionAssert.AreEqual(new[] { keeperLower }, KeyAddresses(me, "correspondents"), "The erased correspondent's OpenPGP key is gone; the other's stays.");
         CollectionAssert.AreEqual(new[] { erasedLower }, CertificateAddresses(asErased, "own"), "The address's own certificate (kind 1) is the account's, and stays.");
         CollectionAssert.AreEqual(new[] { erasedLower }, KeyAddresses(asErased, "own"), "The address's own key (kind 1) is the account's, and stays.");

         // Forgotten, the address is a sender the reply has not answered.
         new SmtpClientSimulator().Send(erased.Address, holder.Address, "Three", "Body");
         Pop3ClientSimulator.AssertMessageCount(erased.Address, Password, 2);
      }

      // ---------------------------------------------------------------- helpers

      private sealed class Held
      {
         public long Id;
         public long MessageId;
         public string Recipients;
      }

      private Account AddAccount(string localPart)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, localPart + "@" + _domain.Name, Password);
      }

      // Three messages to three strangers are the shape once the floors are three.
      private static void EnforceWithLowFloors()
      {
         (int status, string body) set = Admin("PUT", "/api/v1/settings",
            "{\"compromise_detection_mode\":\"enforce\",\"compromise_volume_floor\":3,\"compromise_recipient_floor\":3}");
         ClassicAssert.AreEqual(200, set.status, set.body);
      }

      // Two messages delivered, the third - which completes the shape - held.
      private static Held HoldTheThird(Account sender, Account first, Account second, string third)
      {
         Submit(sender.Address, first.Address, "Erasure 1");
         Submit(sender.Address, second.Address, "Erasure 2");
         Pop3ClientSimulator.AssertMessageCount(first.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(second.Address, Password, 1);
         Submit(sender.Address, third, "Erasure 3");

         (int status, string body) listed = Admin("GET", "/api/v1/review", null);
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         List<Dictionary<string, object>> mine = MiniJson.List(MiniJson.Object(listed.body), "messages").Cast<Dictionary<string, object>>()
            .Where(m => string.Equals((string) m["account"], sender.Address, StringComparison.OrdinalIgnoreCase)).ToList();
         ClassicAssert.AreEqual(1, mine.Count, "The third message completed the shape and was held: " + listed.body);
         StringAssert.Contains(third.ToLowerInvariant(), ((string) mine[0]["recipients"]).ToLowerInvariant());
         return new Held
         {
            Id = Convert.ToInt64(mine[0]["id"]),
            MessageId = Convert.ToInt64(mine[0]["message_id"]),
            Recipients = (string) mine[0]["recipients"]
         };
      }

      private static void Submit(string from, string to, string subject)
      {
         string error;
         new SmtpClientSimulator().Send(false, from, Password, from, to, subject, "The text of " + subject + ".", out error);
      }

      // The queued message's file, by its row: empty once the row is gone.
      private string QueueFileOf(long messageId)
      {
         string name = _application.Database.UtilGetFileNameByMessageID(messageId);
         ClassicAssert.IsFalse(string.IsNullOrEmpty(name), "The held message " + messageId + " has a row in the queue.");
         return Path.Combine(_settings.Directories.DataDirectory, name);
      }

      private static List<long> HeldIds(string account)
      {
         (int status, string body) listed = Admin("GET", "/api/v1/review", null);
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         return MiniJson.List(MiniJson.Object(listed.body), "messages").Cast<Dictionary<string, object>>()
            .Where(m => string.Equals((string) m["account"], account, StringComparison.OrdinalIgnoreCase))
            .Select(m => Convert.ToInt64(m["id"])).ToList();
      }

      private static long RecipientsKnown(string account)
      {
         (int status, string body) review = Admin("GET", "/api/v1/accounts/" + Uri.EscapeDataString(account) + "/review", null);
         ClassicAssert.AreEqual(200, review.status, review.body);
         return MiniJson.Number(MiniJson.Object(review.body), "baseline.recipients_known");
      }

      private static Dictionary<string, object> Erase(string address)
      {
         (int status, string body) answer = Admin("POST", "/api/v1/addresses/erase", "{\"address\":\"" + address + "\",\"include_archive\":false}");
         ClassicAssert.AreEqual(200, answer.status, answer.body);
         return MiniJson.Object(answer.body);
      }

      private long OpenCase(string name)
      {
         (int status, string body) opened = Admin("POST", "/api/v1/discovery/cases", "{\"name\":\"" + name + "\",\"description\":\"Opened by the erasure fixture.\"}");
         ClassicAssert.AreEqual(201, opened.status, opened.body);
         long id = MiniJson.Number(MiniJson.Object(opened.body), "id");
         _cases.Add(id);
         return id;
      }

      // The account's message that expires with this subject, or null.
      private static Dictionary<string, object> ConfidentialItem(string authorization, string subject)
      {
         (int status, string body) listed = User("GET", "/api/v1/me/confidential", authorization, null);
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         return MiniJson.List(MiniJson.Object(listed.body), "items").Cast<Dictionary<string, object>>()
            .FirstOrDefault(item => (string) item["subject"] == subject);
      }

      private static string TokenOf(Dictionary<string, object> item)
      {
         ClassicAssert.IsNotNull(item);
         string link = (string) item["link"];
         Match token = Regex.Match(link, "/confidential/([0-9a-f]{64})$");
         ClassicAssert.IsTrue(token.Success, link);
         return token.Groups[1].Value;
      }

      private string KeptPath(Account sender, string token)
      {
         return Path.Combine(_settings.Directories.DataDirectory, "Confidential", sender.ID.ToString(), token + ".eml");
      }

      private static List<string> RecipientsOf(Dictionary<string, object> item)
      {
         ClassicAssert.IsNotNull(item);
         return MiniJson.List(item, "recipients").Cast<string>().Select(r => r.ToLowerInvariant()).ToList();
      }

      // The reading page, with no credential: 200 for its proof form, 404 for a
      // token that names nothing.
      private static int Page(string token)
      {
         return RestRequest.Send("127.0.0.1", RestPort, "GET", "/confidential/" + token, null, null).status;
      }

      // A one-time code asked for on the reading page, as its form posts it: 200
      // when one is issued (or a stranger is answered as if one were), 429 inside a
      // minute of the last one for the same address.
      private static int AskForCode(string token, string address)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);
            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] form = Encoding.UTF8.GetBytes("step=code&address=" + address);
               string headers = "POST /confidential/" + token + " HTTP/1.0\r\nHost: 127.0.0.1\r\n" +
                                "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + form.Length + "\r\nConnection: close\r\n\r\n";
               byte[] head = Encoding.ASCII.GetBytes(headers);
               stream.Write(head, 0, head.Length);
               stream.Write(form, 0, form.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               string[] status = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               int code;
               return status.Length >= 2 && int.TryParse(status[1], out code) ? code : 0;
            }
         }
      }

      private static List<string> ProtectedRecipients(string authorization)
      {
         (int status, string body) listed = User("GET", "/api/v1/me/protected", authorization, null);
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         return MiniJson.List(MiniJson.Object(listed.body), "items").Cast<Dictionary<string, object>>()
            .Select(item => ((string) item["recipient"]).ToLowerInvariant()).ToList();
      }

      // The addresses of one list of the account's S/MIME store: own or recipients.
      private static List<string> CertificateAddresses(string authorization, string list)
      {
         (int status, string body) listed = User("GET", "/api/v1/me/smime", authorization, null);
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         return MiniJson.List(MiniJson.Object(listed.body), list).Cast<Dictionary<string, object>>()
            .Select(row => ((string) row["address"]).ToLowerInvariant()).ToList();
      }

      // The addresses of one list of the account's OpenPGP ring: own or correspondents.
      private static List<string> KeyAddresses(string authorization, string list)
      {
         (int status, string body) listed = User("GET", "/api/v1/me/openpgp/keys", authorization, null);
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         return MiniJson.List(MiniJson.Object(listed.body), list).Cast<Dictionary<string, object>>()
            .Select(row => ((string) row["address"]).ToLowerInvariant()).ToList();
      }

      private static string Escaped(string armoured)
      {
         return armoured.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");
      }

      private static void Tidy(Action step)
      {
         try
         {
            step();
         }
         catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
         {
            Console.WriteLine("Erasure teardown: " + tidying.Message);
         }
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) User(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
