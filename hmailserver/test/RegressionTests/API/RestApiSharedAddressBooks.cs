// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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
   ///    Shared address books (AddressBookStore.h, schema 6079): an account keeps
   ///    several books, and shares one with another account of this server or with
   ///    every account of one of its domains, read-only or writable.
   ///
   ///    Both listeners are started, because the claim of the row is that the
   ///    webmail's books and the CardDAV collections are one store: a book shared
   ///    over REST is a collection in the sharee's addressbook-home a moment later,
   ///    and a read-only one refuses a PUT there as it refuses a POST here.
   ///
   ///    The listener is plain HTTP on this bench and CardDAV refuses Basic over
   ///    plain HTTP, so the DAV requests say what a TLS-terminating proxy in front
   ///    of the listener would say: X-Forwarded-Proto: https.
   /// </summary>
   [TestFixture]
   public class RestApiSharedAddressBooks : TestFixtureBase
   {
      private const int WebServicesPort = 9757;
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9756;
      private const string Password = "Books-Passw0rd!";
      private const string AdminPassword = "testar";

      // The ceiling AddressBookStore.h sets, named here so that the test that
      // walks up to it says which it is walking up to.
      private const int MaximumBooks = 50;

      private Account _owner;
      private Account _mate;
      private Account _colleague;

      private string OwnerAddress
      {
         get { return "books@" + _domain.Name; }
      }

      private string MateAddress
      {
         get { return "mate@" + _domain.Name; }
      }

      private string ColleagueAddress
      {
         get { return "colleague@" + _domain.Name; }
      }

      [SetUp]
      public void StartListeners()
      {
         _owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, OwnerAddress, Password);
         _mate = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, MateAddress, Password);
         _colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, ColleagueAddress, Password);
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

         Assert.AreEqual(200, Rest("GET", "/api/v1/me/address-books").status, "The REST listener did not answer /api/v1/me/address-books.");
         Assert.AreEqual(200, Dav(OwnerAddress, "OPTIONS", "/dav/", null, null).Status, "The CardDAV endpoint did not answer OPTIONS.");
      }

      [TearDown]
      public void StopListeners()
      {
         IniFileSetting.Write("WebServicesHttpPort", "0");
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("An account starts with one book, may make more, renames them, cannot delete the default one, and deletes another with the contacts in it")]
      public void TheDefaultBookIsThereAndOthersAreMadeRenamedAndDeleted()
      {
         (int status, string body) listed = Rest("GET", "/api/v1/me/address-books");
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"name\":\"Contacts\"", listed.body, "The default book is made the first time the books are asked for.");
         StringAssert.Contains("\"is_default\":true", listed.body);
         StringAssert.Contains("\"mine\":true", listed.body);
         long defaultBook = long.Parse(Between(listed.body, "\"id\":", ","));

         // Every contact the account already had is in the default book, which
         // is what the 0 the upgrade wrote means.
         long ada = AddContact(OwnerAddress, "Ada Lovelace", "ada@example.net", 0);
         (int status, string body) inDefault = Rest("GET", "/api/v1/me/contacts?book=" + defaultBook);
         StringAssert.Contains("\"id\":" + ada, inDefault.body, inDefault.body);

         (int status, string body) made = Rest("POST", "/api/v1/me/address-books", "{\"name\":\"Suppliers\"}");
         Assert.AreEqual(201, made.status, made.body);
         long suppliers = long.Parse(Between(made.body, "\"id\":", ","));
         StringAssert.Contains("\"is_default\":false", made.body, "A book made later is never the default.");
         Assert.AreNotEqual(defaultBook, suppliers);

         Assert.AreEqual(409, Rest("POST", "/api/v1/me/address-books", "{\"name\":\"Suppliers\"}").status,
            "Two books of one account cannot share a name.");
         Assert.AreEqual(400, Rest("POST", "/api/v1/me/address-books", "{\"name\":\"\"}").status);

         (int status, string body) renamed = Rest("PUT", "/api/v1/me/address-books/" + suppliers, "{\"name\":\"Parts\"}");
         Assert.AreEqual(200, renamed.status, renamed.body);
         StringAssert.Contains("\"name\":\"Parts\"", renamed.body);

         // A contact goes in the book it names, and moves between the books of
         // one account.
         long bolt = AddContact(OwnerAddress, "Bolt Supply", "bolt@example.net", suppliers);
         (int status, string body) inSuppliers = Rest("GET", "/api/v1/me/contacts?book=" + suppliers);
         StringAssert.Contains("\"id\":" + bolt, inSuppliers.body, inSuppliers.body);
         StringAssert.DoesNotContain("ada@example.net", inSuppliers.body, "One book's listing is that book's contacts.");

         (int status, string body) moved = Rest("PUT", "/api/v1/me/contacts/" + bolt, "{\"book\":" + defaultBook + "}");
         Assert.AreEqual(200, moved.status, moved.body);
         StringAssert.Contains("\"book_id\":" + defaultBook, moved.body, moved.body);
         StringAssert.DoesNotContain("bolt@example.net", Rest("GET", "/api/v1/me/contacts?book=" + suppliers).body);

         // The default book cannot be deleted; another can, with what is in it.
         Assert.AreEqual(403, Rest("DELETE", "/api/v1/me/address-books/" + defaultBook).status);
         AddContact(OwnerAddress, "Nut Supply", "nut@example.net", suppliers);
         Assert.AreEqual(200, Rest("DELETE", "/api/v1/me/address-books/" + suppliers).status);
         StringAssert.DoesNotContain("\"name\":\"Parts\"", Rest("GET", "/api/v1/me/address-books").body);
         StringAssert.DoesNotContain("nut@example.net", Rest("GET", "/api/v1/me/contacts").body,
            "Deleting a book deletes the contacts in it.");

         // And a book nobody has is 404 rather than 403, whoever asks.
         Assert.AreEqual(404, Rest("GET", "/api/v1/me/contacts?book=" + suppliers).status);
         Assert.AreEqual(404, RestAs(MateAddress, "PUT", "/api/v1/me/address-books/" + defaultBook, "{\"name\":\"Mine now\"}").status,
            "Another account's book is not found, so its ids say nothing.");
      }

      [Test]
      [Description("A book shared read-only is listed to the sharee with its owner, its contacts are read, and every write into it is refused - over REST and over CardDAV, where the collection says so in current-user-privilege-set before the client tries")]
      public void AReadOnlyShareIsReadAndRefusedOnBothSurfaces()
      {
         long team = MakeBook("Team");
         AddContact(OwnerAddress, "Grace Hopper", "grace@example.net", team);
         Share(team, MateAddress, false);

         (int status, string body) books = RestAs(MateAddress, "GET", "/api/v1/me/address-books", null);
         Assert.AreEqual(200, books.status, books.body);
         StringAssert.Contains("\"name\":\"Team\"", books.body);
         StringAssert.Contains("\"owner\":\"" + OwnerAddress + "\"", books.body);
         StringAssert.Contains("\"mine\":false", books.body);
         StringAssert.Contains("\"writable\":false", books.body);
         // The sharee's own default book carries an empty shares array, because
         // it is theirs; what must not be here is any TARGET, since who else a
         // book is shared with is the owner's to know.
         StringAssert.DoesNotContain("\"target\":", books.body, "Only the owner is told who a book is shared with.");

         (int status, string body) contacts = RestAs(MateAddress, "GET", "/api/v1/me/contacts?book=" + team, null);
         Assert.AreEqual(200, contacts.status, contacts.body);
         StringAssert.Contains("grace@example.net", contacts.body);

         Assert.AreEqual(403, RestAs(MateAddress, "POST", "/api/v1/me/contacts",
            "{\"name\":\"Nobody\",\"address\":\"nobody@example.net\",\"book\":" + team + "}").status,
            "A book shared read-only refuses a contact.");

         // The owner's row, found through the share, cannot be changed or
         // deleted by the sharee either.
         long graceId = long.Parse(Between(contacts.body, "\"id\":", ","));
         Assert.AreEqual(403, RestAs(MateAddress, "PUT", "/api/v1/me/contacts/" + graceId, "{\"name\":\"Changed\"}").status);
         Assert.AreEqual(403, RestAs(MateAddress, "DELETE", "/api/v1/me/contacts/" + graceId, null).status);
         Assert.AreEqual(403, RestAs(MateAddress, "GET", "/api/v1/me/address-books/" + team + "/shares", null).status);

         // Over CardDAV: the collection is in the sharee's home, it says it may
         // not be written, and a PUT into it is refused.
         Response home = Dav(MateAddress, "PROPFIND", "/dav/addressbooks/" + MateAddress + "/", null, "Depth: 1\r\n");
         Assert.AreEqual(207, home.Status, home.Body);
         StringAssert.Contains("/dav/addressbooks/" + MateAddress + "/shared-" + team + "/", home.Body,
            "A book shared with the account is a collection of its home.");
         StringAssert.Contains("Team", home.Body);

         Response privileges = Dav(MateAddress, "PROPFIND", "/dav/addressbooks/" + MateAddress + "/shared-" + team + "/", null, "Depth: 0\r\n");
         Assert.AreEqual(207, privileges.Status, privileges.Body);
         StringAssert.Contains("<D:privilege><D:read/></D:privilege>", privileges.Body);
         StringAssert.DoesNotContain("<D:write/>", privileges.Body, "A read-only collection says so before the client tries.");

         Response refused = Put(MateAddress, "/dav/addressbooks/" + MateAddress + "/shared-" + team + "/new.vcf",
            Card("Mallory", "Mallory;;;;", "mallory@example.net"));
         Assert.AreEqual(403, refused.Status, refused.Body);
         StringAssert.Contains("read-only", refused.Body);

         // The owner's own listing is unchanged by any of it.
         StringAssert.DoesNotContain("mallory@example.net", Rest("GET", "/api/v1/me/contacts?book=" + team).body);
      }

      [Test]
      [Description("A book shared writable lets the other account add a contact, over REST and over CardDAV, and the row belongs to the book's owner rather than to whoever wrote it")]
      public void AWritableShareLetsTheOtherAccountWriteAndTheRowIsTheOwners()
      {
         long team = MakeBook("Team");
         Share(team, MateAddress, true);

         (int status, string body) added = RestAs(MateAddress, "POST", "/api/v1/me/contacts",
            "{\"name\":\"Alan Turing\",\"address\":\"alan@example.net\",\"book\":" + team + "}");
         Assert.AreEqual(201, added.status, added.body);

         // The owner reads it in their own book: the row is theirs.
         StringAssert.Contains("alan@example.net", Rest("GET", "/api/v1/me/contacts?book=" + team).body);

         // And it is NOT in the sharee's own books, which is the whole of
         // "the row belongs to the owner".
         StringAssert.DoesNotContain("alan@example.net", RestAs(MateAddress, "GET", "/api/v1/me/contacts", null).body,
            "A contact added to somebody else's book is not the writer's own contact.");

         // A card the sharee PUTs over CardDAV lands in the owner's book too.
         Response put = Put(MateAddress, "/dav/addressbooks/" + MateAddress + "/shared-" + team + "/edsger.vcf",
            Card("Edsger Dijkstra", "Dijkstra;Edsger;;;", "edsger@example.net"));
         Assert.AreEqual(201, put.Status, put.Body);
         StringAssert.Contains("edsger@example.net", Rest("GET", "/api/v1/me/contacts?book=" + team).body);

         // And the owner sees it at the same place in their own tree.
         Response ownerBook = Dav(OwnerAddress, "PROPFIND", "/dav/addressbooks/" + OwnerAddress + "/book-" + team + "/", null, "Depth: 1\r\n");
         Assert.AreEqual(207, ownerBook.Status, ownerBook.Body);
         StringAssert.Contains("edsger.vcf", ownerBook.Body);
      }

      [Test]
      [Description("Unsharing takes the book away from the sharee on both surfaces, and leaving one gives the share back without touching the owner's contacts")]
      public void UnsharingTakesItBackAndLeavingGivesItBack()
      {
         long team = MakeBook("Team");
         AddContact(OwnerAddress, "Grace Hopper", "grace@example.net", team);
         Share(team, MateAddress, false);
         StringAssert.Contains("\"name\":\"Team\"", RestAs(MateAddress, "GET", "/api/v1/me/address-books", null).body);

         (int status, string body) unshared = Rest("DELETE", "/api/v1/me/address-books/" + team + "/shares/" + MateAddress);
         Assert.AreEqual(200, unshared.status, unshared.body);

         StringAssert.DoesNotContain("\"name\":\"Team\"", RestAs(MateAddress, "GET", "/api/v1/me/address-books", null).body);
         Assert.AreEqual(404, RestAs(MateAddress, "GET", "/api/v1/me/contacts?book=" + team, null).status);
         Assert.AreEqual(404, Dav(MateAddress, "PROPFIND", "/dav/addressbooks/" + MateAddress + "/shared-" + team + "/", null, "Depth: 0\r\n").Status);
         Assert.AreEqual(404, Rest("DELETE", "/api/v1/me/address-books/" + team + "/shares/" + MateAddress).status,
            "A share that is not there is not found.");

         // Shared again, and left by the sharee: the share goes, the book and
         // its contacts stay the owner's.
         Share(team, MateAddress, true);
         (int status, string body) left = RestAs(MateAddress, "DELETE", "/api/v1/me/address-books/" + team, null);
         Assert.AreEqual(200, left.status, left.body);
         StringAssert.Contains("\"left\":true", left.body, "Leaving is not deleting.");
         StringAssert.DoesNotContain("\"name\":\"Team\"", RestAs(MateAddress, "GET", "/api/v1/me/address-books", null).body);
         StringAssert.Contains("\"name\":\"Team\"", Rest("GET", "/api/v1/me/address-books").body);
         StringAssert.Contains("grace@example.net", Rest("GET", "/api/v1/me/contacts?book=" + team).body);
      }

      [Test]
      [Description("A book shared with a whole domain reaches every account of it, and one of those accounts cannot take that share back - it is not theirs to take")]
      public void ADomainShareReachesEveryoneAndIsNotOneAccountsToLeave()
      {
         long team = MakeBook("Team");
         AddContact(OwnerAddress, "Grace Hopper", "grace@example.net", team);
         Share(team, _domain.Name, false);

         foreach (string address in new[] { MateAddress, ColleagueAddress })
         {
            (int status, string body) books = RestAs(address, "GET", "/api/v1/me/address-books", null);
            Assert.AreEqual(200, books.status, books.body);
            StringAssert.Contains("\"name\":\"Team\"", books.body, address + " should see the domain's shared book: " + books.body);
            StringAssert.Contains("grace@example.net", RestAs(address, "GET", "/api/v1/me/contacts?book=" + team, null).body);
         }

         // The owner's listing says what kind of share it is.
         (int status, string body) shares = Rest("GET", "/api/v1/me/address-books/" + team + "/shares");
         Assert.AreEqual(200, shares.status, shares.body);
         StringAssert.Contains("\"target\":\"" + _domain.Name + "\"", shares.body);
         StringAssert.Contains("\"kind\":\"domain\"", shares.body);

         Assert.AreEqual(403, RestAs(MateAddress, "DELETE", "/api/v1/me/address-books/" + team, null).status,
            "A share made to the whole domain is not one account's to take back.");
         Assert.AreEqual(403, Dav(MateAddress, "DELETE", "/dav/addressbooks/" + MateAddress + "/shared-" + team + "/", null, null).Status);
         StringAssert.Contains("\"name\":\"Team\"", RestAs(ColleagueAddress, "GET", "/api/v1/me/address-books", null).body,
            "And nobody else lost it either.");

         // A share to a domain this server does not host is 404, and a share to
         // the owner's own account is refused with the reason.
         Assert.AreEqual(404, Rest("PUT", "/api/v1/me/address-books/" + team + "/shares/nowhere.example", "{}").status);
         Assert.AreEqual(400, Rest("PUT", "/api/v1/me/address-books/" + team + "/shares/" + OwnerAddress, "{}").status);
      }

      [Test]
      [Description("The completion asks for every book the reader can see and says which book each contact came from, the reader's own first")]
      public void TheCompletionFindsAContactOfASharedBookAndLabelsIt()
      {
         long team = MakeBook("Team");
         AddContact(OwnerAddress, "Zoe Shared", "zoe@example.net", team);
         AddContact(MateAddress, "Zoe Own", "zoe-own@example.net", 0);
         Share(team, MateAddress, false);

         // Without book=, the sharee's own books alone - which is what the
         // exports mean by "my contacts".
         (int status, string body) own = RestAs(MateAddress, "GET", "/api/v1/me/contacts?q=zoe", null);
         Assert.AreEqual(200, own.status, own.body);
         StringAssert.Contains("zoe-own@example.net", own.body);
         StringAssert.DoesNotContain("\"address\":\"zoe@example.net\"", own.body);

         (int status, string body) all = RestAs(MateAddress, "GET", "/api/v1/me/contacts?q=zoe&book=all", null);
         Assert.AreEqual(200, all.status, all.body);
         StringAssert.Contains("zoe-own@example.net", all.body);
         StringAssert.Contains("\"address\":\"zoe@example.net\"", all.body);
         StringAssert.Contains("\"book_name\":\"Team\"", all.body);
         StringAssert.Contains("\"book_owner\":\"" + OwnerAddress + "\"", all.body);
         StringAssert.Contains("\"book_mine\":false", all.body);

         // The reader's own come first, which is what "labelled, but mine
         // first" means in an ordered answer.
         Assert.Less(all.body.IndexOf("zoe-own@example.net", StringComparison.Ordinal),
            all.body.IndexOf("\"address\":\"zoe@example.net\"", StringComparison.Ordinal), all.body);
      }

      [Test]
      [Description("An administrator lists an account's books and who each is shared with, and removes one share; the contacts of a book are not on any of those routes")]
      public void TheAdministratorSeesTheSharingAndRemovesOneShare()
      {
         long team = MakeBook("Team");
         AddContact(OwnerAddress, "Grace Hopper", "grace@example.net", team);
         Share(team, MateAddress, true);

         (int status, string body) listed = Admin("GET", "/api/v1/accounts/" + OwnerAddress + "/address-books", null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"name\":\"Team\"", listed.body);
         StringAssert.Contains("\"target\":\"" + MateAddress + "\"", listed.body);
         StringAssert.Contains("\"write\":true", listed.body);
         StringAssert.Contains("\"contacts\":1", listed.body, "How many, not who: " + listed.body);
         StringAssert.DoesNotContain("grace@example.net", listed.body,
            "An administrator sees the shape of the sharing and not the address book.");

         Assert.AreEqual(404, Admin("GET", "/api/v1/accounts/nobody@" + _domain.Name + "/address-books", null).status);

         (int status, string body) removed = Admin("DELETE", "/api/v1/accounts/" + OwnerAddress + "/address-books/" + team + "/shares/" + MateAddress, null);
         Assert.AreEqual(200, removed.status, removed.body);
         StringAssert.DoesNotContain("\"name\":\"Team\"", RestAs(MateAddress, "GET", "/api/v1/me/address-books", null).body);
         Assert.AreEqual(404, Admin("DELETE", "/api/v1/accounts/" + OwnerAddress + "/address-books/" + team + "/shares/" + MateAddress, null).status);

         // It is on the trail as the event it is, since the table is the
         // account's own and is not audited as rows.
         (int status, string body) trail = Admin("GET", "/api/v1/audit?limit=50", null);
         Assert.AreEqual(200, trail.status, trail.body);
         StringAssert.Contains("address-book-share-removed", trail.body, trail.body);
      }

      [Test]
      [Description("The ceilings hold: fifty books to one account, and the fifty-first is refused with the reason")]
      public void TheBookCeilingHolds()
      {
         // One is the default, so the account may make forty-nine more.
         for (int i = 1; i < MaximumBooks; i++)
         {
            (int status, string body) made = Rest("POST", "/api/v1/me/address-books", "{\"name\":\"Book " + i + "\"}");
            Assert.AreEqual(201, made.status, "Book " + i + ": " + made.body);
         }

         (int status, string body) refused = Rest("POST", "/api/v1/me/address-books", "{\"name\":\"One too many\"}");
         Assert.AreEqual(507, refused.status, refused.body);
         StringAssert.Contains(MaximumBooks.ToString(), refused.body);
      }

      [Test]
      [Description("No group crosses a share, in either direction: a group of a book somebody shares with you is not listed, not found by id and not served over CardDAV, and a group card cannot be written into somebody else's book - which is what keeps a writable share from being a way to ask for the contacts of the books that were never shared")]
      public void NoGroupCrossesAShare()
      {
         // The owner's private book, with one contact nobody is to see, and a
         // group in the DEFAULT book naming it. Every group is made in the
         // default book (ContactGroups::Create), so that is the book whose
         // sharing puts a group in front of somebody else.
         long defaultBook = long.Parse(Between(Rest("GET", "/api/v1/me/address-books").body, "\"id\":", ","));
         long privateBook = MakeBook("Private");
         AddContact(OwnerAddress, "Secret Friend", "secret@example.net", privateBook);
         AddContact(OwnerAddress, "Public Friend", "public@example.net", defaultBook);

         (int status, string body) made = Rest("POST", "/api/v1/me/contacts/groups",
            "{\"name\":\"Inner Circle\",\"members\":[{\"address\":\"secret@example.net\"}]}");
         Assert.AreEqual(201, made.status, made.body);
         long groupId = long.Parse(Between(made.body, "\"id\":", ","));
         StringAssert.Contains("secret@example.net", made.body, "The owner's own group resolves its member: " + made.body);

         // The owner shares the book the group is in, read-only. That is the
         // case nobody has to attack: the group is simply there.
         Share(defaultBook, MateAddress, false);

         (int status, string body) all = RestAs(MateAddress, "GET", "/api/v1/me/contacts?book=all&limit=1000", null);
         Assert.AreEqual(200, all.status, all.body);
         StringAssert.Contains("public@example.net", all.body, "The shared book's contacts are read: " + all.body);
         StringAssert.DoesNotContain("Inner Circle", all.body, "A group is not shared.");
         StringAssert.DoesNotContain("secret@example.net", all.body,
            "A group resolves against its owner's WHOLE address book, so listing one to a sharee would hand over the books that were never shared.");
         StringAssert.DoesNotContain("Inner Circle",
            RestAs(MateAddress, "GET", "/api/v1/me/contacts?book=all&kind=group", null).body);

         // Not by id either, on any route that takes one: the row is simply
         // not found, as a contact of a book nobody shared is.
         Assert.AreEqual(404, RestAs(MateAddress, "PUT", "/api/v1/me/contacts/" + groupId, "{\"name\":\"Theirs now\"}").status);
         Assert.AreEqual(404, RestAs(MateAddress, "DELETE", "/api/v1/me/contacts/" + groupId, null).status);
         StringAssert.Contains("Inner Circle", Rest("GET", "/api/v1/me/contacts?kind=group").body,
            "And the owner still has it.");

         // Nor over CardDAV: the collection the sharee syncs holds the
         // contacts of the book and not the group in it, while the owner's own
         // collection holds both.
         Response theirs = Dav(MateAddress, "PROPFIND", "/dav/addressbooks/" + MateAddress + "/shared-" + defaultBook + "/", null, "Depth: 1\r\n");
         Assert.AreEqual(207, theirs.Status, theirs.Body);
         StringAssert.DoesNotContain("/" + groupId + ".vcf", theirs.Body, theirs.Body);
         Assert.AreEqual(404, Dav(MateAddress, "GET", "/dav/addressbooks/" + MateAddress + "/shared-" + defaultBook + "/" + groupId + ".vcf", null, null).Status,
            "A group of somebody else's book has no card to fetch.");

         Response ours = Dav(OwnerAddress, "PROPFIND", "/dav/addressbooks/" + OwnerAddress + "/contacts/", null, "Depth: 1\r\n");
         Assert.AreEqual(207, ours.Status, ours.Body);
         StringAssert.Contains("/" + groupId + ".vcf", ours.Body, "The owner's own phone still syncs it: " + ours.Body);

         // And the other direction: a writable share is not a way to put a
         // group into somebody's account, which would be a way to ask for the
         // rows it names - by address, or by the UID a row of theirs is served
         // under - and to plant a name their own compose would expand.
         long team = MakeBook("Team");
         Share(team, MateAddress, true);
         Assert.AreEqual(201, RestAs(MateAddress, "POST", "/api/v1/me/contacts",
            "{\"name\":\"Allowed\",\"address\":\"allowed@example.net\",\"book\":" + team + "}").status,
            "A writable share does take a contact.");

         string groupCard = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:planted-group-uid\r\nFN:Planted\r\nX-ADDRESSBOOKSERVER-KIND:group\r\n" +
                            "X-ADDRESSBOOKSERVER-MEMBER:mailto:secret@example.net\r\nEND:VCARD\r\n";
         Response planted = Put(MateAddress, "/dav/addressbooks/" + MateAddress + "/shared-" + team + "/planted.vcf", groupCard);
         Assert.AreEqual(403, planted.Status, planted.Body);
         StringAssert.Contains("group", planted.Body, planted.Body);
         StringAssert.DoesNotContain("Planted", Rest("GET", "/api/v1/me/contacts?book=all&limit=1000").body,
            "Nothing of it reached the owner's address book.");
         StringAssert.DoesNotContain("Planted", Rest("GET", "/api/v1/me/contacts/groups").body);
      }

      [Test]
      [Description("A card PUT into a book shared writable does not take one of the owner's collected addresses out of the book it is in: it is refused 409, as the REST route refuses it, and the refusal carries no path into a book the writer cannot see")]
      public void APutIntoASharedBookDoesNotMoveTheOwnersCollectedContact()
      {
         // The owner writes to somebody, which collects that address into the
         // owner's own default book (ContactCard.cpp), never into a book they
         // share with a team.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(OwnerAddress, Password));
         imap.CreateFolder("Sent");
         imap.Disconnect();

         (int status, string body) sent = Rest("POST", "/api/v1/me/messages",
            "{\"to\":\"" + ColleagueAddress + "\",\"subject\":\"Collected\",\"text\":\"Body.\"}");
         Assert.AreEqual(201, sent.status, sent.body);

         long defaultBook = long.Parse(Between(Rest("GET", "/api/v1/me/address-books").body, "\"id\":", ","));
         StringAssert.Contains(ColleagueAddress, Rest("GET", "/api/v1/me/contacts?source=collected").body,
            "The address was collected.");

         long team = MakeBook("Team");
         Share(team, MateAddress, true);

         Response put = Put(MateAddress, "/dav/addressbooks/" + MateAddress + "/shared-" + team + "/theirs.vcf",
            Card("Taken Over", "Over;Taken;;;", ColleagueAddress));
         Assert.AreEqual(409, put.Status, put.Body);

         // The row stayed where it was, under the name it was collected as.
         StringAssert.Contains(ColleagueAddress, Rest("GET", "/api/v1/me/contacts?book=" + defaultBook + "&source=collected").body);
         StringAssert.DoesNotContain(ColleagueAddress, Rest("GET", "/api/v1/me/contacts?book=" + team).body,
            "A writable share is not a way to move one of the owner's contacts into the shared book.");
         StringAssert.DoesNotContain("Taken Over", Rest("GET", "/api/v1/me/contacts?book=all&limit=1000").body);
         StringAssert.DoesNotContain("/dav/addressbooks/" + OwnerAddress, put.Body,
            "The refusal does not hand the writer a path into a book they cannot see.");

         // The same card into a book the writer's own account owns is taken,
         // so the refusal above is about the boundary and not about the card.
         Assert.AreEqual(201, Put(MateAddress, "/dav/addressbooks/" + MateAddress + "/contacts/theirs.vcf",
            Card("Taken Over", "Over;Taken;;;", ColleagueAddress)).Status);
      }

      [Test]
      [Description("A book is shared with a whole domain only when it is the sharer's own domain, because nobody in a domain can leave a share that was not made to them and only the owner's domain administrator can remove one")]
      public void ADomainWideShareIsOnlyTheSharersOwnDomain()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("elsewhere-books.example");
         SingletonProvider<TestSetup>.Instance.AddAccount(other, "someone@" + other.Name, Password);

         long team = MakeBook("Team");
         AddContact(OwnerAddress, "Grace Hopper", "grace@example.net", team);

         (int status, string body) refused = Rest("PUT", "/api/v1/me/address-books/" + team + "/shares/" + other.Name, "{}");
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("own domain", refused.body, refused.body);
         StringAssert.DoesNotContain("\"name\":\"Team\"",
            RestAs("someone@" + other.Name, "GET", "/api/v1/me/address-books", null).body,
            "Nobody in the other domain was given a book they could not have refused or left.");

         // The sharer's own domain is allowed, and one account of that domain
         // is allowed whatever domain it is in.
         Share(team, _domain.Name, false);
         Assert.AreEqual(200, Rest("PUT", "/api/v1/me/address-books/" + team + "/shares/someone@" + other.Name, "{}").status,
            "An account of another domain is shared with by address, which that account can then leave.");
         (int status, string body) left = RestAs("someone@" + other.Name, "DELETE", "/api/v1/me/address-books/" + team, null);
         Assert.AreEqual(200, left.status, left.body);
         StringAssert.Contains("\"left\":true", left.body);
      }

      [Test]
      [Description("The administrator's listing of an account's address books makes nothing: an account that has never asked for its books is reported at the default book it will make, and the listing asked twice leaves the account exactly as it found it")]
      public void TheAdministratorsListingMakesNothing()
      {
         // The colleague has not been near the webmail, so nothing has asked
         // that account for its books yet. ViewAccounts is a permission the
         // auditor role holds, and an auditor changes nothing.
         (int status, string body) first = Admin("GET", "/api/v1/accounts/" + ColleagueAddress + "/address-books", null);
         Assert.AreEqual(200, first.status, first.body);
         StringAssert.Contains("\"id\":0", first.body, "The default book it has not made yet is reported at 0: " + first.body);
         StringAssert.Contains("\"is_default\":true", first.body);
         StringAssert.Contains("\"contacts\":0", first.body);
         StringAssert.Contains("\"shares\":[]", first.body);

         (int status, string body) again = Admin("GET", "/api/v1/accounts/" + ColleagueAddress + "/address-books", null);
         Assert.AreEqual(first.body, again.body, "A read that made a book would answer differently the second time.");

         // The account asking for its own books is what makes it, and the
         // administrator then sees the real one.
         (int status, string body) theirs = RestAs(ColleagueAddress, "GET", "/api/v1/me/address-books", null);
         Assert.AreEqual(200, theirs.status, theirs.body);
         long real = long.Parse(Between(theirs.body, "\"id\":", ","));
         Assert.Greater(real, 0, theirs.body);
         StringAssert.Contains("\"id\":" + real, Admin("GET", "/api/v1/accounts/" + ColleagueAddress + "/address-books", null).body);
      }

      // ------------------------------------------------------------- helpers

      private long MakeBook(string name)
      {
         (int status, string body) made = Rest("POST", "/api/v1/me/address-books", "{\"name\":\"" + name + "\"}");
         Assert.AreEqual(201, made.status, made.body);
         return long.Parse(Between(made.body, "\"id\":", ","));
      }

      private void Share(long book, string target, bool write)
      {
         (int status, string body) shared = Rest("PUT", "/api/v1/me/address-books/" + book + "/shares/" + target,
            "{\"write\":" + (write ? "true" : "false") + "}");
         Assert.AreEqual(200, shared.status, shared.body);
      }

      private long AddContact(string asAddress, string name, string address, long book)
      {
         string body = book > 0
            ? "{\"name\":\"" + name + "\",\"address\":\"" + address + "\",\"book\":" + book + "}"
            : "{\"name\":\"" + name + "\",\"address\":\"" + address + "\"}";
         (int status, string body) created = RestAs(asAddress, "POST", "/api/v1/me/contacts", body);
         Assert.AreEqual(201, created.status, created.body);
         return long.Parse(Between(created.body, "\"id\":", ","));
      }

      private (int status, string body) Rest(string method, string path)
      {
         return RestAs(OwnerAddress, method, path, null);
      }

      private (int status, string body) Rest(string method, string path, string requestBody)
      {
         return RestAs(OwnerAddress, method, path, requestBody);
      }

      private (int status, string body) RestAs(string address, string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Auth(address, Password), requestBody);
      }

      private (int status, string body) Admin(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Auth("Administrator", AdminPassword), requestBody);
      }

      private Response Put(string asAddress, string path, string card)
      {
         return Raw(WebServicesPort, "PUT", path, Auth(asAddress, Password), card, "text/vcard; charset=utf-8", "X-Forwarded-Proto: https\r\n");
      }

      private Response Dav(string asAddress, string method, string path, string body, string extraHeaders)
      {
         return Raw(WebServicesPort, method, path, Auth(asAddress, Password), body, "application/xml; charset=utf-8",
            "X-Forwarded-Proto: https\r\n" + (extraHeaders ?? ""));
      }

      private static string Card(string fullName, string structuredName, string address)
      {
         return "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:" + fullName + "\r\nN:" + structuredName +
                "\r\nEMAIL;TYPE=INTERNET:" + address + "\r\nEND:VCARD\r\n";
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
   }
}
