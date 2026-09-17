// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Contact groups (ContactGroups.h): a group in the same address book as the
   ///    contacts, with no schema of its own - a row of hm_contacts whose card is a
   ///    vCard group and whose address column holds the marker "group:&lt;uid&gt;".
   ///
   ///    Both listeners are started here, because the whole claim of the row is that
   ///    the two surfaces are one address book: a group the REST routes make is
   ///    fetched over CardDAV as a vCard whose kind is group, and a group card a
   ///    phone PUTs is read back over REST with its members resolved. Nothing below
   ///    is asserted from the response that wrote it alone.
   ///
   ///    The listener is plain HTTP on this bench and CardDAV refuses Basic over
   ///    plain HTTP, so the DAV requests say what a TLS-terminating proxy in front
   ///    of the listener would say: X-Forwarded-Proto: https.
   /// </summary>
   [TestFixture]
   public class RestApiContactGroups : TestFixtureBase
   {
      private const int WebServicesPort = 9647;
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9646;
      private const string Password = "Groups-Passw0rd!";
      private const string AdminPassword = "testar";

      // The ceilings ContactGroups.h sets, named here so that a test that
      // walks up to one says which it is walking up to.
      private const int MaximumMembers = 500;
      private const int MaximumGroups = 200;

      private Account _account;

      private string Address
      {
         get { return "groups@" + _domain.Name; }
      }

      private string Book
      {
         get { return "/dav/addressbooks/" + Address + "/contacts/"; }
      }

      [SetUp]
      public void StartListeners()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, Password);
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

         Assert.AreEqual(200, Rest("GET", "/api/v1/me/contacts").status, "The REST listener did not answer /api/v1/me/contacts.");
         Assert.AreEqual(200, Dav("OPTIONS", "/dav/", null).Status, "The CardDAV endpoint did not answer OPTIONS.");
      }

      [TearDown]
      public void StopListeners()
      {
         IniFileSetting.Write("WebServicesHttpPort", "0");
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("A group made over REST holds the contact and the address it was given, is listed among the contacts with kind group, is served over CardDAV as a vCard whose kind is group, and stands for its members when its name alone is put in To")]
      public void AGroupIsMadeListedServedAsAVCardGroupAndAddressedByItsName()
      {
         string alice = "alice@" + _domain.Name;
         string bob = "bob@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, alice, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, bob, Password);
         long aliceId = AddContact("Alice Example", alice);
         AddContact("Bob Builder", bob);

         // One contact by id, one by address: the address that is a contact is
         // written as that contact, so both come back with their names.
         (int status, string body) made = Rest("POST", "/api/v1/me/contacts/groups",
            "{\"name\":\"Family\",\"members\":[{\"id\":" + aliceId + "},{\"address\":\"" + bob + "\"}]}");
         Assert.AreEqual(201, made.status, made.body);
         long groupId = long.Parse(Between(made.body, "\"id\":", ","));
         string uid = Between(made.body, "\"uid\":\"", "\"");
         StringAssert.Contains("\"kind\":\"group\"", made.body);
         StringAssert.Contains("\"address\":\"\"", made.body, "A group has no address of its own over the API.");
         StringAssert.Contains("\"name\":\"Alice Example\",\"address\":\"" + alice + "\",\"kind\":\"person\"", made.body);
         StringAssert.Contains("\"name\":\"Bob Builder\",\"address\":\"" + bob + "\",\"kind\":\"person\"", made.body);
         StringAssert.Contains("\"recipients\":[", made.body);

         // The address book lists it beside the people, which is what the To
         // field's completion reads; kind= answers one kind alone.
         (int status, string body) listed = Rest("GET", "/api/v1/me/contacts");
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"name\":\"Family\"", listed.body);
         StringAssert.Contains("\"kind\":\"group\"", listed.body);
         (int status, string body) groupsOnly = Rest("GET", "/api/v1/me/contacts?kind=group");
         StringAssert.Contains("\"name\":\"Family\"", groupsOnly.body);
         // Alice's name IS in this body: she is a member of Family, and the
         // listing carries a group's members so the completion can expand it
         // without asking again. What says the filter worked is that no ENTRY
         // is a person, which is what the row promises and what this assertion
         // meant before it was written as a search for a name.
         Assert.AreEqual(0, EntriesOfKind(groupsOnly.body, "person"), "kind=group answers groups alone: " + groupsOnly.body);
         Assert.GreaterOrEqual(EntriesOfKind(groupsOnly.body, "group"), 1, groupsOnly.body);
         (int status, string body) peopleOnly = Rest("GET", "/api/v1/me/contacts?kind=person");
         StringAssert.Contains("Alice Example", peopleOnly.body);
         StringAssert.DoesNotContain("\"name\":\"Family\"", peopleOnly.body);
         Assert.AreEqual(0, EntriesOfKind(peopleOnly.body, "group"), "kind=person answers people alone: " + peopleOnly.body);
         Assert.AreEqual(400, Rest("GET", "/api/v1/me/contacts?kind=nonsense").status, "kind is person or group");

         // What the completion asks for: the name, narrowed.
         (int status, string body) completed = Rest("GET", "/api/v1/me/contacts?limit=8&q=fam");
         StringAssert.Contains("\"name\":\"Family\"", completed.body);
         StringAssert.Contains("\"kind\":\"group\"", completed.body);

         // And the same row over CardDAV: a card whose kind is group, with a
         // member line per member - the contact by the UID it is served under,
         // the address that is a contact by its UID too.
         Response card = Dav("GET", Book + groupId + ".vcf", null);
         Assert.AreEqual(200, card.Status, card.Body);
         StringAssert.Contains("X-ADDRESSBOOKSERVER-KIND:group", card.Body);
         StringAssert.Contains("FN:Family", card.Body);
         Assert.AreEqual(2, Regex.Matches(card.Body, "X-ADDRESSBOOKSERVER-MEMBER:").Count, card.Body);
         StringAssert.DoesNotContain("EMAIL", card.Body, "A group has no address of its own.");
         StringAssert.Contains("UID:" + uid, card.Body);
         StringAssert.DoesNotContain("group:" + uid, card.Body, "The marker the row holds is never written into the card.");

         // A client that asks the book for its groups is given this one, and a
         // client that asks for a card with that EMAIL is not.
         Response report = Dav("REPORT", Book,
            "<C:addressbook-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:carddav\"><D:prop><D:getetag/></D:prop>" +
            "<C:filter><C:prop-filter name=\"X-ADDRESSBOOKSERVER-KIND\"><C:text-match collation=\"i;unicode-casemap\" match-type=\"equals\">group</C:text-match></C:prop-filter></C:filter></C:addressbook-query>",
            "Depth: 1\r\n");
         Assert.AreEqual(207, report.Status, report.Body);
         StringAssert.Contains(Book + groupId + ".vcf", report.Body);

         // The name alone in To stands for the people it reaches - in the
         // envelope, so both mailboxes get it, and in the header, so the
         // reader sees who it went to and not the group's name.
         (int status, string body) sent = Rest("POST", "/api/v1/me/messages",
            "{\"to\":\"Family\",\"subject\":\"The outing\",\"text\":\"Saturday.\"}");
         Assert.AreEqual(201, sent.status, sent.body);
         string toAlice = Pop3ClientSimulator.AssertGetFirstMessageText(alice, Password);
         string toBob = Pop3ClientSimulator.AssertGetFirstMessageText(bob, Password);
         StringAssert.Contains("Saturday.", toAlice);
         StringAssert.Contains("Saturday.", toBob);
         StringAssert.Contains(alice, toAlice);
         StringAssert.Contains(bob, toAlice);
         StringAssert.DoesNotContain("To: Family", toAlice, "The header carries the people, not the group's name.");
         StringAssert.DoesNotContain("To: Family", toBob);

         // A name that is no group's is left as it was, and refused as not an
         // address a moment later - it does not quietly send to nobody.
         (int status, string body) nobody = Rest("POST", "/api/v1/me/messages",
            "{\"to\":\"Nobody At All\",\"subject\":\"Hello\",\"text\":\"Body.\"}");
         Assert.AreEqual(400, nobody.status, nobody.body);
      }

      [Test]
      [Description("A group card a phone PUTs is the group the page sees: the members it names by UID and by address are resolved, one that names nothing is kept in the card and left out of the answer, and a rename over REST rewrites the card in its own vCard 4.0 dialect")]
      public void AGroupAPhoneMakesIsTheGroupThePageSees()
      {
         string bob = "bob-phone@" + _domain.Name;
         AddContact("Bob Builder", bob);

         // The phone's own contact, with a UID of its own, and then a group
         // card naming it, an address that is not a contact, and a UID this
         // address book has never had.
         Response carol = Put(Book + "carol.vcf",
            "BEGIN:VCARD\r\nVERSION:4.0\r\nUID:carol-of-the-phone\r\nFN:Carol Card\r\nN:Card;Carol;;;\r\nEMAIL:carol-phone@" + _domain.Name + "\r\nEND:VCARD\r\n");
         Assert.AreEqual(201, carol.Status, carol.Body);

         Response group = Put(Book + "the-team.vcf",
            "BEGIN:VCARD\r\nVERSION:4.0\r\nUID:the-team-of-the-phone\r\nFN:The team\r\nKIND:group\r\n" +
            "MEMBER:urn:uuid:carol-of-the-phone\r\nMEMBER:mailto:" + bob + "\r\nMEMBER:urn:uuid:never-here\r\nEND:VCARD\r\n");
         Assert.AreEqual(201, group.Status, group.Body);

         (int status, string body) groups = Rest("GET", "/api/v1/me/contacts/groups");
         Assert.AreEqual(200, groups.status, groups.body);
         StringAssert.Contains("\"name\":\"The team\"", groups.body);
         StringAssert.Contains("\"uid\":\"the-team-of-the-phone\"", groups.body);
         StringAssert.Contains("Carol Card", groups.body);
         StringAssert.Contains(bob, groups.body);
         StringAssert.DoesNotContain("never-here", groups.body, "A reference that names nothing is left out of the answer.");
         StringAssert.Contains("\"count\":1", groups.body);
         long groupId = long.Parse(Between(groups.body, "\"id\":", ","));

         // A rename over REST keeps what the card carries besides, and writes
         // the member lines back in the card's own dialect - MEMBER, since the
         // phone sent a 4.0 card - including the one that names nothing.
         (int status, string body) renamed = Rest("PUT", "/api/v1/me/contacts/groups/" + groupId, "{\"name\":\"The whole team\"}");
         Assert.AreEqual(200, renamed.status, renamed.body);
         Response after = Dav("GET", Book + "the-team.vcf", null);
         Assert.AreEqual(200, after.Status, after.Body);
         StringAssert.Contains("FN:The whole team", after.Body);
         StringAssert.Contains("KIND:group", after.Body);
         StringAssert.DoesNotContain("X-ADDRESSBOOKSERVER-MEMBER", after.Body, "A 4.0 card keeps the 4.0 spelling.");
         Assert.AreEqual(3, Regex.Matches(after.Body, "\r\nMEMBER:").Count, after.Body);
         StringAssert.Contains("MEMBER:urn:uuid:never-here", after.Body, "The reference stays in the card, which is the client's.");
         StringAssert.Contains("UID:the-team-of-the-phone", after.Body);

         // A card whose kind is group and which has no FN is refused, since a
         // group with no name could not be addressed or told apart.
         Response nameless = Put(Book + "nameless.vcf",
            "BEGIN:VCARD\r\nVERSION:4.0\r\nUID:nameless-group\r\nKIND:group\r\nMEMBER:mailto:" + bob + "\r\nEND:VCARD\r\n");
         Assert.AreEqual(403, nameless.Status, nameless.Body);
         StringAssert.Contains("FN", nameless.Body);
      }

      [Test]
      [Description("A contact deleted is taken out of every group that named it, a group in a group is expanded once when the outer one is addressed, and a group deleted leaves its members and the groups that named it")]
      public void ADeletedMemberLeavesTheGroupsAndAGroupInAGroupIsExpandedOnce()
      {
         string alice = "alice-nested@" + _domain.Name;
         string bob = "bob-nested@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, alice, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, bob, Password);
         long aliceId = AddContact("Alice Example", alice);
         long bobId = AddContact("Bob Builder", bob);

         long inner = MakeGroup("Inner", "{\"id\":" + aliceId + "}");
         long outer = MakeGroup("Outer", "{\"id\":" + inner + "},{\"id\":" + bobId + "}");

         // The outer group names the inner one as a group, and reaches the
         // people of both, each once.
         (int status, string body) read = Rest("GET", "/api/v1/me/contacts/groups/" + outer);
         Assert.AreEqual(200, read.status, read.body);
         StringAssert.Contains("\"name\":\"Inner\",\"address\":\"\",\"kind\":\"group\"", read.body);
         string recipients = Between(read.body + "]}", "\"recipients\":[", "]");
         StringAssert.Contains(alice, recipients);
         StringAssert.Contains(bob, recipients);
         Assert.AreEqual(2, Regex.Matches(recipients, "\"address\":").Count, recipients);

         // Addressed by name, the outer group reaches both, once each.
         Assert.AreEqual(201, Rest("POST", "/api/v1/me/messages",
            "{\"to\":\"Outer\",\"subject\":\"Both\",\"text\":\"Once each.\"}").status);
         Pop3ClientSimulator.AssertMessageCount(alice, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(bob, Password, 1);

         // A contact deleted goes out of the group that named it, and out of
         // the card, so that the next sync does not carry a member that is gone.
         Assert.AreEqual(200, Rest("DELETE", "/api/v1/me/contacts/" + aliceId).status);
         (int status, string body) innerNow = Rest("GET", "/api/v1/me/contacts/groups/" + inner);
         Assert.AreEqual(200, innerNow.status, innerNow.body);
         StringAssert.DoesNotContain(alice, innerNow.body);
         StringAssert.Contains("\"members\":[]", innerNow.body);
         Response innerCard = Dav("GET", Book + inner + ".vcf", null);
         Assert.AreEqual(200, innerCard.Status, innerCard.Body);
         StringAssert.DoesNotContain("MEMBER", innerCard.Body, "The card no longer names the contact that is gone.");

         // A group deleted leaves its members contacts, and the group that
         // named it no longer does.
         Assert.AreEqual(200, Rest("DELETE", "/api/v1/me/contacts/groups/" + inner).status);
         (int status, string body) outerNow = Rest("GET", "/api/v1/me/contacts/groups/" + outer);
         StringAssert.DoesNotContain("Inner", outerNow.body);
         StringAssert.Contains(bob, outerNow.body);
         (int status, string body) stillThere = Rest("GET", "/api/v1/me/contacts?kind=person");
         StringAssert.Contains(bob, stillThere.body);
         Assert.AreEqual(404, Rest("GET", "/api/v1/me/contacts/groups/" + inner).status);
      }

      [Test]
      [Description("One member added and removed at a time, and what the routes refuse: a second group with one name, a group as its own member, a member that is neither a contact nor an address, a name that could never be typed into To, the contact routes on a group, and another account's group")]
      public void MembersAreAddedOneAtATimeAndWhatCannotBeAGroupIsRefused()
      {
         string dana = "dana@" + _domain.Name;
         long danaId = AddContact("Dana Doe", dana);
         long groupId = MakeGroup("Walkers", "");

         // Added by address, then by id; the same member twice is left as it is.
         (int status, string body) added = Rest("POST", "/api/v1/me/contacts/groups/" + groupId + "/members", "{\"address\":\"" + dana + "\"}");
         Assert.AreEqual(200, added.status, added.body);
         StringAssert.Contains("Dana Doe", added.body);
         (int status, string body) again = Rest("POST", "/api/v1/me/contacts/groups/" + groupId + "/members", "{\"id\":" + danaId + "}");
         Assert.AreEqual(200, again.status, again.body);
         Assert.AreEqual(1, Regex.Matches(again.body, "\"kind\":\"person\"").Count, again.body);

         // An address that is nobody's contact is held as itself.
         (int status, string body) stranger = Rest("POST", "/api/v1/me/contacts/groups/" + groupId + "/members", "{\"address\":\"stranger@example.org\"}");
         Assert.AreEqual(200, stranger.status, stranger.body);
         StringAssert.Contains("stranger@example.org", stranger.body);

         // Removed by address, then by id; and what is not a member is 404.
         (int status, string body) gone = Rest("DELETE", "/api/v1/me/contacts/groups/" + groupId + "/members/stranger%40example.org", null);
         Assert.AreEqual(200, gone.status, gone.body);
         StringAssert.DoesNotContain("stranger@example.org", gone.body);
         Assert.AreEqual(404, Rest("DELETE", "/api/v1/me/contacts/groups/" + groupId + "/members/stranger%40example.org", null).status);
         Assert.AreEqual(200, Rest("DELETE", "/api/v1/me/contacts/groups/" + groupId + "/members/" + danaId, null).status);
         (int status, string body) empty = Rest("GET", "/api/v1/me/contacts/groups/" + groupId);
         StringAssert.Contains("\"members\":[]", empty.body);

         // What is refused, and why.
         Assert.AreEqual(409, Rest("POST", "/api/v1/me/contacts/groups", "{\"name\":\"walkers\"}").status, "A group is addressed by its name.");
         Assert.AreEqual(400, Rest("POST", "/api/v1/me/contacts/groups", "{\"name\":\"\"}").status);
         (int status, string body) comma = Rest("POST", "/api/v1/me/contacts/groups", "{\"name\":\"Walkers, the\"}");
         Assert.AreEqual(400, comma.status, comma.body);
         StringAssert.Contains("addressed by its name", comma.body);
         (int status, string body) itself = Rest("PUT", "/api/v1/me/contacts/groups/" + groupId, "{\"members\":[{\"id\":" + groupId + "}]}");
         Assert.AreEqual(400, itself.status, itself.body);
         StringAssert.Contains("member of itself", itself.body);
         Assert.AreEqual(400, Rest("POST", "/api/v1/me/contacts/groups/" + groupId + "/members", "{\"address\":\"not an address\"}").status);
         Assert.AreEqual(400, Rest("POST", "/api/v1/me/contacts/groups/" + groupId + "/members", "{\"id\":999999}").status);

         // A group is not a contact: the contact routes say where to go.
         (int status, string body) wrongRoute = Rest("PUT", "/api/v1/me/contacts/" + groupId, "{\"name\":\"Walkers\"}");
         Assert.AreEqual(400, wrongRoute.status, wrongRoute.body);
         StringAssert.Contains("/api/v1/me/contacts/groups/" + groupId, wrongRoute.body);

         // And another account's address book is another address book: not 403,
         // which would say the id exists somewhere, but 404.
         string other = "other-book@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, other, Password);
         Assert.AreEqual(404, RestAs(other, "GET", "/api/v1/me/contacts/groups/" + groupId, null).status);
         Assert.AreEqual(404, RestAs(other, "DELETE", "/api/v1/me/contacts/groups/" + groupId, null).status);
      }

      [Test]
      [Description("The two ceilings hold on both surfaces, not on the API alone: a card naming more members than a group may hold is refused over CardDAV, and a book already holding all the groups it may takes no more from either")]
      public void TheCeilingsOnAGroupsMembersAndOnABooksGroupsHoldOnBothSurfaces()
      {
         // Reading a group costs what its members cost, and every read of the
         // book - the listing, the To field's completion, a send addressed by
         // name - reads every group in it. A card of a megabyte could name
         // some fifteen thousand members, so the ceiling the routes enforce is
         // enforced where a client writes too.
         var many = new StringBuilder();
         for (int i = 0; i <= MaximumMembers; i++)
            many.Append("X-ADDRESSBOOKSERVER-MEMBER:mailto:member" + i + "@example.org\r\n");
         Response tooMany = Put(Book + "too-many.vcf",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:too-many-members\r\nFN:Everyone\r\nX-ADDRESSBOOKSERVER-KIND:group\r\n" +
            many + "END:VCARD\r\n");
         Assert.AreEqual(403, tooMany.Status, tooMany.Body);
         StringAssert.Contains(MaximumMembers.ToString(), tooMany.Body);
         Assert.AreEqual(404, Dav("GET", Book + "too-many.vcf", null).Status, "The card past the ceiling was not stored.");
         Assert.AreEqual("{\"groups\":[],\"count\":0}", Rest("GET", "/api/v1/me/contacts/groups").body);

         // One under the ceiling is kept, so what is refused is the ceiling
         // and not the shape of the card.
         var enough = new StringBuilder();
         for (int i = 0; i < MaximumMembers; i++)
            enough.Append("X-ADDRESSBOOKSERVER-MEMBER:mailto:member" + i + "@example.org\r\n");
         Response full = Put(Book + "full.vcf",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:a-full-group\r\nFN:A full group\r\nX-ADDRESSBOOKSERVER-KIND:group\r\n" +
            enough + "END:VCARD\r\n");
         Assert.AreEqual(201, full.Status, full.Body);

         // And the book's own ceiling: the groups are resolved all together,
         // so how many there may be is bounded as well.
         for (int i = 1; i < MaximumGroups; i++)
         {
            (int status, string body) made = Rest("POST", "/api/v1/me/contacts/groups", "{\"name\":\"Cap " + i + "\",\"members\":[]}");
            Assert.AreEqual(201, made.status, made.body);
         }

         (int status, string body) past = Rest("POST", "/api/v1/me/contacts/groups", "{\"name\":\"One too many\",\"members\":[]}");
         Assert.AreEqual(400, past.status, past.body);
         StringAssert.Contains(MaximumGroups.ToString(), past.body);

         // A phone is held to it too, and told in the WebDAV way that there is
         // no room rather than that its card is wrong.
         Response phone = Put(Book + "one-more.vcf",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:one-more-group\r\nFN:One more\r\nX-ADDRESSBOOKSERVER-KIND:group\r\nEND:VCARD\r\n");
         Assert.AreEqual(507, phone.Status, phone.Body);

         // The book still answers, with what it holds and no more.
         (int status, string body) all = Rest("GET", "/api/v1/me/contacts/groups");
         Assert.AreEqual(200, all.status, all.body);
         StringAssert.Contains("\"count\":" + MaximumGroups, all.body);
      }

      [Test]
      [Description("A name two groups of one book answer to - which the routes refuse but two cards from a client can leave - is not guessed between: the send is refused as it would be for any name that is no group's, and the same name reaches the people once only one group answers to it")]
      public void ANameTwoGroupsAnswerToIsRefusedRatherThanGuessedAt()
      {
         string alice = "alice-twice@" + _domain.Name;
         string bob = "bob-twice@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, alice, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, bob, Password);

         // Two cards, each a group called Team: the card is the client's and
         // its UID is its identity, so neither PUT is refused.
         Assert.AreEqual(201, Put(Book + "team-one.vcf",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:team-one\r\nFN:Team\r\nX-ADDRESSBOOKSERVER-KIND:group\r\n" +
            "X-ADDRESSBOOKSERVER-MEMBER:mailto:" + alice + "\r\nEND:VCARD\r\n").Status);
         Assert.AreEqual(201, Put(Book + "team-two.vcf",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:team-two\r\nFN:Team\r\nX-ADDRESSBOOKSERVER-KIND:group\r\n" +
            "X-ADDRESSBOOKSERVER-MEMBER:mailto:" + bob + "\r\nEND:VCARD\r\n").Status);

         (int status, string body) groups = Rest("GET", "/api/v1/me/contacts/groups");
         Assert.AreEqual(200, groups.status, groups.body);
         Assert.AreEqual(2, Regex.Matches(groups.body, "\"name\":\"Team\"").Count, groups.body);

         // The name reaches neither: refused as not an address, which the
         // sender sees, rather than sent to whichever the store answered
         // first, which the sender could not see.
         (int status, string body) sent = Rest("POST", "/api/v1/me/messages",
            "{\"to\":\"Team\",\"subject\":\"Which of you\",\"text\":\"Neither.\"}");
         Assert.AreEqual(400, sent.status, sent.body);
         Pop3ClientSimulator.AssertMessageCount(alice, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(bob, Password, 0);

         // One of the two cards taken away by the client that made it, one
         // group answers to Team, and the name stands for its people again:
         // what was refused was the ambiguity, not the name.
         Assert.AreEqual(204, Dav("DELETE", Book + "team-two.vcf", null).Status);
         (int status, string body) now = Rest("POST", "/api/v1/me/messages",
            "{\"to\":\"Team\",\"subject\":\"You then\",\"text\":\"Just you.\"}");
         Assert.AreEqual(201, now.status, now.body);
         StringAssert.Contains("Just you.", Pop3ClientSimulator.AssertGetFirstMessageText(alice, Password));
         Pop3ClientSimulator.AssertMessageCount(bob, Password, 0);
      }

      [Test]
      [Description("A send later addressed by a group's name reaches the people when the minute comes: the expansion is on the path the scheduled task takes as well as on the immediate one, and the header carries the people")]
      public void AScheduledSendAddressedByAGroupsNameReachesThePeople()
      {
         string alice = "alice-later@" + _domain.Name;
         string bob = "bob-later@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, alice, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, bob, Password);
         long aliceId = AddContact("Alice Example", alice);
         long bobId = AddContact("Bob Builder", bob);
         MakeGroup("Saturday", "{\"id\":" + aliceId + "},{\"id\":" + bobId + "}");

         // The group's name typed into To and not picked from the completion,
         // which would have written the people out; then saved as a draft and
         // scheduled, which is what Send later does.
         (int status, string body) draft = Rest("POST", "/api/v1/me/drafts",
            "{\"to\":\"Saturday\",\"subject\":\"The outing\",\"text\":\"At ten.\"}");
         Assert.AreEqual(201, draft.status, draft.body);
         long draftId = long.Parse(Between(draft.body, "\"id\":", ","));

         string at = DateTime.Now.AddSeconds(2).ToString("yyyy-MM-dd HH:mm:ss");
         Assert.AreEqual(201, Rest("POST", "/api/v1/me/drafts/" + draftId + "/schedule", "{\"send_at\":\"" + at + "\"}").status);

         Thread.Sleep(3000);
         (int status, string body) ran = Admin("POST", "/api/v1/scheduled/run", null);
         Assert.AreEqual(200, ran.status, ran.body);

         string toAlice = Pop3ClientSimulator.AssertGetFirstMessageText(alice, Password);
         string toBob = Pop3ClientSimulator.AssertGetFirstMessageText(bob, Password);
         StringAssert.Contains("At ten.", toAlice);
         StringAssert.Contains("At ten.", toBob);
         StringAssert.Contains(alice, toAlice);
         StringAssert.Contains(bob, toAlice);
         StringAssert.DoesNotContain("To: Saturday", toAlice, "The header carries the people, as it does on the immediate path.");
      }

      [Test]
      [Description("An address erased is taken out of a group card that named it in a shape the resolution never reads - Name <address>, which is no URI and so no member - so that the erasure's report is true rather than merely successful")]
      public void AnErasedAddressIsTakenOutOfAGroupCardThatNamedItAnyWay()
      {
         string fred = "fred-erased@" + _domain.Name;

         Response made = Put(Book + "the-old-crowd.vcf",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:the-old-crowd\r\nFN:The old crowd\r\nX-ADDRESSBOOKSERVER-KIND:group\r\n" +
            "X-ADDRESSBOOKSERVER-MEMBER:Fred Flint <" + fred + ">\r\nEND:VCARD\r\n");
         Assert.AreEqual(201, made.Status, made.Body);

         // Not a member: a MEMBER value is a URI, and this is not one, so the
         // group answers without it and the card keeps it.
         (int status, string body) groups = Rest("GET", "/api/v1/me/contacts/groups");
         StringAssert.Contains("\"name\":\"The old crowd\"", groups.body);
         StringAssert.DoesNotContain(fred, groups.body);

         // The address is still on this server's disk, though, which is what
         // an erasure is about.
         (int status, string body) erased = Admin("POST", "/api/v1/addresses/erase",
            "{\"address\":\"" + fred + "\",\"include_archive\":false}");
         Assert.AreEqual(200, erased.status, erased.body);

         Response after = Dav("GET", Book + "the-old-crowd.vcf", null);
         Assert.AreEqual(200, after.Status, after.Body);
         StringAssert.Contains("FN:The old crowd", after.Body, "The rest of the card is the client's and stays.");
         StringAssert.DoesNotContain(fred, after.Body, "The erased address is out of the card, not merely out of the members.");
      }

      [Test]
      [Description("A book whose groups all name one another is answered in one pass: the rows are indexed once and each card read once, so fifty groups naming the other forty-nine are listed with every one of them reaching the one person, once, and the request does not cost the square of the book")]
      public void ABookOfGroupsThatAllNameOneAnotherIsListedInOnePass()
      {
         const int howMany = 50;
         string sole = "sole@" + _domain.Name;
         long soleId = AddContact("Sole Person", sole);

         var ids = new List<long>();
         for (int i = 0; i < howMany; i++)
            ids.Add(MakeGroup("Ring " + i, ""));

         // Every group names every other, and the first names the one person
         // as well, so that each of the fifty reaches somebody through the
         // ring and the expansion has to walk it.
         for (int i = 0; i < howMany; i++)
         {
            var members = new StringBuilder();
            if (i == 0)
               members.Append("{\"id\":" + soleId + "}");
            for (int j = 0; j < howMany; j++)
            {
               if (j == i)
                  continue;
               if (members.Length > 0)
                  members.Append(",");
               members.Append("{\"id\":" + ids[j] + "}");
            }

            (int status, string body) filled = Rest("PUT", "/api/v1/me/contacts/groups/" + ids[i], "{\"members\":[" + members + "]}");
            Assert.AreEqual(200, filled.status, filled.body);
         }

         var watch = System.Diagnostics.Stopwatch.StartNew();
         (int status, string body) listed = Rest("GET", "/api/v1/me/contacts/groups");
         watch.Stop();
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"count\":" + howMany, listed.body);

         // The one person is reached by every group, through however many
         // steps of the ring, and named once in each group's recipients: the
         // visited set ends the walk and the addresses are answered once each.
         Assert.AreEqual(howMany + 1, Regex.Matches(listed.body, Regex.Escape(sole)).Count,
            "Every group reaches the one person once, and the first group also names them among its members.");

         // A canary, not a benchmark: before the rows were indexed and each
         // card read once, a book of this shape cost its square with a
         // SHA-256 in the inner loop, and a request like this held a worker
         // thread the whole portal shares for minutes.
         Assert.Less(watch.Elapsed.TotalSeconds, 60,
            "A listing of fifty groups that name one another must not take a minute: " + watch.Elapsed);
      }

      // ------------------------------------------------------------- helpers

      private long AddContact(string name, string address)
      {
         (int status, string body) created = Rest("POST", "/api/v1/me/contacts",
            "{\"name\":\"" + name + "\",\"address\":\"" + address + "\"}");
         Assert.AreEqual(201, created.status, created.body);
         return long.Parse(Between(created.body, "\"id\":", ","));
      }

      private long MakeGroup(string name, string members)
      {
         (int status, string body) made = Rest("POST", "/api/v1/me/contacts/groups",
            "{\"name\":\"" + name + "\",\"members\":[" + members + "]}");
         Assert.AreEqual(201, made.status, made.body);
         return long.Parse(Between(made.body, "\"id\":", ","));
      }

      private (int status, string body) Rest(string method, string path)
      {
         return Rest(method, path, null);
      }

      private (int status, string body) Rest(string method, string path, string requestBody)
      {
         return RestAs(Address, method, path, requestBody);
      }

      private (int status, string body) RestAs(string address, string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Auth(address, Password), requestBody);
      }

      // The two routes here that are the operator's, not the account's: the
      // minute task run now, and an address's traces erased.
      private (int status, string body) Admin(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Auth("Administrator", AdminPassword), requestBody);
      }

      private Response Put(string path, string card)
      {
         return Raw(WebServicesPort, "PUT", path, Auth(Address, Password), card, "text/vcard; charset=utf-8", "X-Forwarded-Proto: https\r\n");
      }

      private Response Dav(string method, string path, string body)
      {
         return Dav(method, path, body, null);
      }

      private Response Dav(string method, string path, string body, string extraHeaders)
      {
         return Raw(WebServicesPort, method, path, Auth(Address, Password), body, "application/xml; charset=utf-8",
            "X-Forwarded-Proto: https\r\n" + (extraHeaders ?? ""));
      }

      private static string Auth(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static string Between(string text, string start, string end)
      {
         int from = text.IndexOf(start, StringComparison.Ordinal);
         Assert.IsTrue(from >= 0, "Missing " + start + " in: " + text);
         from += start.Length;
         int to = text.IndexOf(end, from, StringComparison.Ordinal);
         Assert.IsTrue(to >= 0, "Missing " + end + " after " + start + " in: " + text);
         return text.Substring(from, to - from);
      }

      // ------------------------------------------------------------- raw HTTP

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public string HeaderText = "";
         public readonly Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
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

               var memory = new System.IO.MemoryStream();
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
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out response.Status);
               }

               for (int i = 1; i < lines.Length; i++)
               {
                  int colon = lines[i].IndexOf(':');
                  if (colon > 0)
                     response.Headers[lines[i].Substring(0, colon).Trim()] = lines[i].Substring(colon + 1).Trim();
               }

               return response;
            }
         }
      }

      /// <summary>
      ///    How many ENTRIES of the listing are of that kind. A group entry
      ///    carries its members, and a member has a kind of its own, so counting
      ///    the word in the whole body counts the members too; an entry is what
      ///    "id" opens, and its own kind is the one that follows its address.
      /// </summary>
      private static int EntriesOfKind(string body, string kind)
      {
         int found = 0;
         foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(body), "contacts").Cast<Dictionary<string, object>>())
         {
            object value;
            if (entry.TryGetValue("kind", out value) && (string) value == kind)
               found += 1;
         }

         return found;
      }
   }
}
