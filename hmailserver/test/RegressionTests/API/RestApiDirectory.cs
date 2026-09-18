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
   ///    The organisation's directory (DirectoryStore.h): every active, not-opted-out
   ///    account and every enabled distribution list of ONE domain - the reader's own -
   ///    with the title, department, telephone numbers and photo the administrator or
   ///    the person set.
   ///
   ///    Both listeners are started here, because the claim of the row is that the
   ///    same rows are the completion, the page and a read-only address book a phone
   ///    syncs: what an administrator writes over
   ///    PUT /api/v1/accounts/{address}/directory is read by a colleague over
   ///    GET /api/v1/me/directory and fetched as a vCard under /dav/. Nothing below
   ///    is asserted from the response that wrote it alone.
   ///
   ///    The domain boundary is the test that matters most: an account of a second
   ///    domain on the same server is in its own directory and in no other, and
   ///    there is no spelling of the reader's route that names a domain.
   ///
   ///    The listener is plain HTTP on this bench and CardDAV refuses Basic over
   ///    plain HTTP, so the DAV requests say what a TLS-terminating proxy in front
   ///    of the listener would say: X-Forwarded-Proto: https.
   /// </summary>
   [TestFixture]
   public class RestApiDirectory : TestFixtureBase
   {
      private const int WebServicesPort = 9747;
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9746;
      private const string Password = "Directory-Passw0rd!";
      private const string AdminPassword = "testar";

      // A one-pixel PNG, as a photo a person or an administrator uploads. The
      // bytes decide what is kept, so this is a real PNG and not a string that
      // says it is one.
      private const string PhotoBase64 =
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

      private Account _ada;

      private string AdaAddress
      {
         get { return "ada@" + _domain.Name; }
      }

      private string ColleagueAddress
      {
         get { return "colleague@" + _domain.Name; }
      }

      private string DirectoryCollection
      {
         get { return "/dav/addressbooks/" + ColleagueAddress + "/directory/"; }
      }

      [SetUp]
      public void StartListeners()
      {
         _ada = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, AdaAddress, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, ColleagueAddress, Password);
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

         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).Status, "The REST listener did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopListeners()
      {
         try
         {
            // Whatever a test turned off goes back on, whether it reached its
            // own last line or not: a switch left down here would empty the
            // directory for every fixture after this one.
            Admin("PUT", "/api/v1/settings", "{\"directory_enabled\":true,\"directory_user_editable\":true}");
         }
         finally
         {
            IniFileSetting.Write("WebServicesHttpPort", "0");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The administrator sets a title, a department and a telephone number on one account; a colleague of the same domain reads them in the directory, and the administrator's note is not in what the colleague is answered.")]
      public void TheAdministratorSetsAnEntryAndTheDomainSeesIt()
      {
         Response set = Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory",
            "{\"title\":\"Head of Engines\",\"department\":\"Engineering\",\"phone\":\"+44 20 7946 0000\",\"office\":\"Marylebone, 2nd floor\",\"note\":\"Asked to be listed as Ada.\"}");
         Assert.AreEqual(200, set.Status, set.Body);

         Response read = Admin("GET", "/api/v1/accounts/" + AdaAddress + "/directory", null);
         Assert.AreEqual(200, read.Status, read.Body);
         StringAssert.Contains("\"note\":\"Asked to be listed as Ada.\"", read.Body, "The note is answered to the administrator.");

         Response listed = As(ColleagueAddress, "GET", "/api/v1/me/directory", null);
         Assert.AreEqual(200, listed.Status, listed.Body);
         StringAssert.Contains("\"enabled\":true", listed.Body);
         StringAssert.Contains("\"address\":\"" + AdaAddress + "\"", listed.Body);
         StringAssert.Contains("\"title\":\"Head of Engines\"", listed.Body);
         StringAssert.Contains("\"department\":\"Engineering\"", listed.Body);
         StringAssert.Contains("\"phone\":\"+44 20 7946 0000\"", listed.Body);
         StringAssert.Contains("\"office\":\"Marylebone, 2nd floor\"", listed.Body);
         StringAssert.DoesNotContain("Asked to be listed as Ada.", listed.Body,
            "The administrator's note is not answered to a reader.");
         StringAssert.DoesNotContain("\"note\"", listed.Body, "And the key is not there either.");

         // The reader is in their own directory as well, from hm_accounts alone,
         // with no row of their own.
         StringAssert.Contains("\"address\":\"" + ColleagueAddress + "\"", listed.Body);

         // A field too long for its column is refused by name rather than
         // truncated into something nobody typed.
         Response toolong = Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory",
            "{\"title\":\"" + new string('x', 101) + "\"}");
         Assert.AreEqual(400, toolong.Status, toolong.Body);
         StringAssert.Contains("title", toolong.Body);
         StringAssert.Contains("\"title\":\"Head of Engines\"",
            Admin("GET", "/api/v1/accounts/" + AdaAddress + "/directory", null).Body, "And nothing was written.");

         // An unknown key is refused rather than dropped with a 200.
         Assert.AreEqual(400, Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory", "{\"nickname\":\"Ada\"}").Status);

         // The one field of the entry that goes on the audit trail. A title and
         // a telephone number are the person's own as often as the
         // administrator's and decide nothing about mail, so hm_directory is
         // exempt; the opt-out is a decision of the person's that somebody else
         // can reverse, and an application-log line is not a record anybody can
         // read back or revert.
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":true}").Status);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory", "{\"opt_out\":false}").Status);

         Response trail = Admin("GET", "/api/v1/audit?action=directory-opt-out-changed&limit=1", null);
         Assert.AreEqual(200, trail.Status, trail.Body);
         StringAssert.Contains(AdaAddress, trail.Body,
            "Clearing somebody's opt-out left nothing on the audit trail: " + trail.Body);
         StringAssert.Contains("listed again", trail.Body, trail.Body);
      }

      [Test]
      [Description("The person sets their own title, telephone number and photo; a colleague sees them and fetches the photo from its own route. The opt-out takes the person out of the listing, out of the completion and out of the photo route - and leaves their own photo reachable to themselves.")]
      public void ThePersonSetsTheirOwnAndTheOptOutHides()
      {
         Response own = As(AdaAddress, "PUT", "/api/v1/me/directory/me",
            "{\"title\":\"Countess of Lovelace\",\"phone\":\"+44 20 7946 0001\",\"photo\":\"" + PhotoBase64 + "\"}");
         Assert.AreEqual(200, own.Status, own.Body);
         StringAssert.Contains("\"editable\":true", own.Body);
         StringAssert.Contains("\"has_photo\":true", own.Body);
         StringAssert.Contains("\"photo_type\":\"image/png\"", own.Body,
            "The media type is read from the bytes, not from anything the request said.");

         Response listed = As(ColleagueAddress, "GET", "/api/v1/me/directory", null);
         Assert.AreEqual(200, listed.Status, listed.Body);
         StringAssert.Contains("\"title\":\"Countess of Lovelace\"", listed.Body);
         StringAssert.Contains("\"photo_url\":\"/api/v1/me/directory/" + _ada.ID + "/photo\"", listed.Body);

         Response photo = As(ColleagueAddress, "GET", "/api/v1/me/directory/" + _ada.ID + "/photo", null);
         Assert.AreEqual(200, photo.Status, photo.Body);
         Assert.AreEqual("image/png", photo.Header("Content-Type"), photo.HeaderText);
         StringAssert.Contains("nosniff", photo.HeaderText);

         // The opt-out, which is the person's own whatever else is set.
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":true}").Status);

         Response after = As(ColleagueAddress, "GET", "/api/v1/me/directory", null);
         StringAssert.DoesNotContain(AdaAddress, after.Body, "An account that has opted out is not in the listing.");
         StringAssert.DoesNotContain("Countess of Lovelace", after.Body);
         StringAssert.Contains("\"address\":\"" + ColleagueAddress + "\"", after.Body, "The rest of the domain is still there.");

         StringAssert.DoesNotContain(AdaAddress,
            As(ColleagueAddress, "GET", "/api/v1/me/directory?q=lovelace", null).Body,
            "Nor in what the completion asks for.");

         Assert.AreEqual(404, As(ColleagueAddress, "GET", "/api/v1/me/directory/" + _ada.ID + "/photo", null).Status,
            "And the photo route says no more than the listing does.");

         // The person can still see the photo they chose: their own route is not
         // answered from the listing they are no longer in.
         Response mine = As(AdaAddress, "GET", "/api/v1/me/directory/me/photo", null);
         Assert.AreEqual(200, mine.Status, mine.Body);

         // And mail still arrives: the opt-out is "leave me out of the list",
         // not "hide my mailbox".
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", AdaAddress, "Still arriving", "Body.");
         StringAssert.Contains("Still arriving", Pop3ClientSimulator.AssertGetFirstMessageText(AdaAddress, Password));

         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":false}").Status);
         StringAssert.Contains(AdaAddress, As(ColleagueAddress, "GET", "/api/v1/me/directory", null).Body);
      }

      [Test]
      [Description("A disabled account is not in the directory, an enabled distribution list is, and an account of another domain on the same server is in its own directory and in no other.")]
      public void TheDirectoryIsOneDomainAndNothingElse()
      {
         Account disabled = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "gone@" + _domain.Name, Password);
         disabled.Active = false;
         disabled.Save();

         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "sales@" + _domain.Name,
            new List<string> { AdaAddress });

         Domain otherDomain = SingletonProvider<TestSetup>.Instance.AddDomain("directory-other.test");
         Account stranger = SingletonProvider<TestSetup>.Instance.AddAccount(otherDomain, "stranger@directory-other.test", Password);

         Response listed = As(ColleagueAddress, "GET", "/api/v1/me/directory", null);
         Assert.AreEqual(200, listed.Status, listed.Body);
         StringAssert.Contains("\"domain\":\"" + _domain.Name.ToLowerInvariant() + "\"", listed.Body);
         StringAssert.DoesNotContain("gone@", listed.Body, "An account that cannot sign in is not offered as a colleague.");
         StringAssert.Contains("\"address\":\"sales@" + _domain.Name + "\"", listed.Body);
         StringAssert.Contains("\"kind\":\"list\"", listed.Body);
         StringAssert.DoesNotContain("stranger@directory-other.test", listed.Body,
            "An account of another domain is not in this domain's directory.");

         // And the boundary holds from the other side: the stranger is answered
         // their own domain, which does not hold this one's people.
         Response theirs = As(stranger.Address, "GET", "/api/v1/me/directory", null);
         Assert.AreEqual(200, theirs.Status, theirs.Body);
         StringAssert.Contains("\"address\":\"stranger@directory-other.test\"", theirs.Body);
         StringAssert.DoesNotContain(ColleagueAddress, theirs.Body);

         // The completion's question, against the name, the title and the
         // department alike.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory",
            "{\"department\":\"Engineering\"}").Status);
         _ada.PersonFirstName = "Ada";
         _ada.PersonLastName = "Lovelace";
         _ada.Save();

         StringAssert.Contains(AdaAddress, As(ColleagueAddress, "GET", "/api/v1/me/directory?q=lovel", null).Body);
         StringAssert.Contains(AdaAddress, As(ColleagueAddress, "GET", "/api/v1/me/directory?q=engineer", null).Body);
         Response none = As(ColleagueAddress, "GET", "/api/v1/me/directory?q=nobody-by-that-name", null);
         StringAssert.Contains("\"count\":0", none.Body, none.Body);

         // And the same question asked about a name that is not ASCII. The
         // query string is UTF-8; decoded with the machine's code page instead,
         // "Ingénieure" arrives as "IngÃ©nieure" and matches nothing - so the
         // completion would fail for exactly the colleagues whose names are
         // hardest to type, and say "count":0 while doing it. The body here is
         // UTF-8 and so is the query, and both are written as escapes so that
         // this file stays ASCII.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory",
            "{\"title\":\"Ingénieure en chef\",\"department\":\"Motörenbau\"}").Status);

         Response accented = As(ColleagueAddress, "GET", "/api/v1/me/directory?q=Ing%C3%A9nieure", null);
         Assert.AreEqual(200, accented.Status, accented.Body);
         StringAssert.Contains(AdaAddress, accented.Body, "A title with an accent is found by typing the accent.");
         StringAssert.Contains("\"count\":1", accented.Body, accented.Body);

         // Lower-cased on both sides, which is where a byte-wise fold would
         // also come apart.
         StringAssert.Contains(AdaAddress, As(ColleagueAddress, "GET", "/api/v1/me/directory?q=mot%C3%B6renbau", null).Body,
            "A department with an umlaut is found whatever the case.");
      }

      [Test]
      [Description("With directory_user_editable off, a person's own PUT of a field is refused 403 and nothing is written, while the opt-out still goes through - and the administrator can set the fields either way.")]
      public void TheLockedServerKeepsTheFieldsAndLeavesTheOptOut()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"directory_user_editable\":false}").Status);

         Response refused = As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"title\":\"Whatever I like\"}");
         Assert.AreEqual(403, refused.Status, refused.Body);
         StringAssert.Contains("opt_out", refused.Body, "The refusal says what is still theirs.");

         Response mine = As(AdaAddress, "GET", "/api/v1/me/directory/me", null);
         StringAssert.Contains("\"editable\":false", mine.Body);
         StringAssert.DoesNotContain("Whatever I like", mine.Body, "Nothing was written.");

         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":true}").Status,
            "Leaving the list is the person's whatever the switch says.");
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":false}").Status);

         // The administrator is not what the switch is about.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory",
            "{\"title\":\"Head of Engines\"}").Status);
         StringAssert.Contains("\"title\":\"Head of Engines\"", As(AdaAddress, "GET", "/api/v1/me/directory/me", null).Body);
      }

      [Test]
      [Description("With directory_enabled off the listing is empty and says so, the photo route is 404, and the CardDAV collection is still there with nothing in it - so a phone that subscribed syncs to nothing rather than failing every poll.")]
      public void TheSwitchOffEmptiesItWithoutBreakingAClient()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"directory_enabled\":false}").Status);

         Response listed = As(ColleagueAddress, "GET", "/api/v1/me/directory", null);
         Assert.AreEqual(200, listed.Status, listed.Body);
         StringAssert.Contains("\"enabled\":false", listed.Body);
         StringAssert.Contains("\"entries\":[]", listed.Body);
         Assert.AreEqual(404, As(ColleagueAddress, "GET", "/api/v1/me/directory/" + _ada.ID + "/photo", null).Status);

         Response collection = Dav("PROPFIND", DirectoryCollection,
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/></D:prop></D:propfind>", "Depth: 1\r\n");
         Assert.AreEqual(207, collection.Status, collection.Body);
         StringAssert.Contains("addressbook", collection.Body, "The collection is still there.");
         StringAssert.DoesNotContain(".vcf", collection.Body, "And holds nothing.");
      }

      [Test]
      [Description("CardDAV: the home lists the Directory collection beside the account's own book, the collection lists a card per person and per list, a card carries the name, address, title and telephone number, and PUT and DELETE are refused - it is the organisation's, read-only.")]
      public void TheDirectoryIsServedOverCardDav()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/accounts/" + AdaAddress + "/directory",
            "{\"title\":\"Head of Engines\",\"department\":\"Engineering\",\"phone\":\"+44 20 7946 0000\"}").Status);
         _ada.PersonFirstName = "Ada";
         _ada.PersonLastName = "Lovelace";
         _ada.Save();

         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "everyone@" + _domain.Name,
            new List<string> { AdaAddress });

         Response home = Dav("PROPFIND", "/dav/addressbooks/" + ColleagueAddress + "/",
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:displayname/><D:resourcetype/></D:prop></D:propfind>", "Depth: 1\r\n");
         Assert.AreEqual(207, home.Status, home.Body);
         StringAssert.Contains("/contacts/", home.Body, "The account's own book is still listed.");
         StringAssert.Contains("/directory/", home.Body, "And the organisation's directory beside it.");
         StringAssert.Contains("Directory", home.Body);

         Response cards = Dav("PROPFIND", DirectoryCollection,
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getetag/><D:displayname/></D:prop></D:propfind>", "Depth: 1\r\n");
         Assert.AreEqual(207, cards.Status, cards.Body);
         StringAssert.Contains("account-" + _ada.ID + ".vcf", cards.Body);
         StringAssert.Contains("Ada Lovelace", cards.Body);
         StringAssert.Contains("list-", cards.Body, "A distribution list is a card of the collection too.");

         Response card = Dav("GET", DirectoryCollection + "account-" + _ada.ID + ".vcf", null, null);
         Assert.AreEqual(200, card.Status, card.Body);
         StringAssert.Contains("BEGIN:VCARD", card.Body);
         StringAssert.Contains("FN:Ada Lovelace", card.Body);
         StringAssert.Contains(AdaAddress, card.Body);
         StringAssert.Contains("TITLE:Head of Engines", card.Body);
         StringAssert.Contains("+44 20 7946 0000", card.Body);
         Assert.IsNotNull(card.Header("ETag"), card.HeaderText);

         // The collection answers the reports a phone polls it with, and the
         // privileges say plainly that it may only be read.
         Response privileges = Dav("PROPFIND", DirectoryCollection,
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:current-user-privilege-set/><CS:getctag xmlns:CS=\"http://calendarserver.org/ns/\"/></D:prop></D:propfind>", "Depth: 0\r\n");
         Assert.AreEqual(207, privileges.Status, privileges.Body);
         StringAssert.Contains("read", privileges.Body);
         StringAssert.DoesNotContain("<D:write/>", privileges.Body, "Nothing here says a client may write.");
         StringAssert.Contains("getctag", privileges.Body);

         Response sync = Dav("REPORT", DirectoryCollection,
            "<D:sync-collection xmlns:D=\"DAV:\"><D:sync-token/><D:prop><D:getetag/></D:prop></D:sync-collection>", "Depth: 1\r\n");
         Assert.AreEqual(207, sync.Status, sync.Body);
         StringAssert.Contains("sync-token", sync.Body);
         StringAssert.Contains("account-" + _ada.ID + ".vcf", sync.Body);

         // Read-only means refused, and says why.
         Response put = Raw(WebServicesPort, "PUT", DirectoryCollection + "mine.vcf", Auth(ColleagueAddress, Password),
            "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Mine\r\nEMAIL:mine@example.net\r\nEND:VCARD\r\n",
            "text/vcard; charset=utf-8", "X-Forwarded-Proto: https\r\n");
         Assert.AreEqual(403, put.Status, put.Body);
         StringAssert.Contains("read-only", put.Body);

         Assert.AreEqual(403, Dav("DELETE", DirectoryCollection + "account-" + _ada.ID + ".vcf", null, null).Status);

         // The third way of writing to a collection, refused like the other
         // two and for the same stated reason. It used to answer a bare 405
         // while the doc, the roadmap row and the commit all said 403 with the
         // reason; nothing asked, so nothing noticed.
         Response proppatch = Dav("PROPPATCH", DirectoryCollection,
            "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop><D:displayname>Mine now</D:displayname></D:prop></D:set></D:propertyupdate>",
            null);
         Assert.AreEqual(403, proppatch.Status, proppatch.Body);
         StringAssert.Contains("read-only", proppatch.Body);
         Assert.AreEqual(403, Dav("PROPPATCH", DirectoryCollection + "account-" + _ada.ID + ".vcf",
            "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop><D:displayname>Mine now</D:displayname></D:prop></D:set></D:propertyupdate>",
            null).Status, "A card of it, the same.");

         // The photo: in the card a client fetches, and NOT read to answer a
         // listing. The collection listing reads no photograph at all - a
         // domain's worth of base64 on every ctag poll was the cost of doing
         // otherwise - so the ETag must not depend on having read one: the
         // tag the listing gives and the tag the fetched card carries are one
         // tag, before and after the photo is there to read.
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"photo\":\"" + PhotoBase64 + "\"}").Status);

         Response tags = Dav("PROPFIND", DirectoryCollection,
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getetag/><D:getcontentlength/></D:prop></D:propfind>", "Depth: 1\r\n");
         Assert.AreEqual(207, tags.Status, tags.Body);
         string cardName = "account-" + _ada.ID + ".vcf";
         string listedEtag = DavPropertyFor(tags.Body, cardName, "getetag");
         Assert.IsNotNull(listedEtag, tags.Body);

         Response withPhoto = Dav("GET", DirectoryCollection + cardName, null, null);
         Assert.AreEqual(200, withPhoto.Status, withPhoto.Body);
         StringAssert.Contains("PHOTO;ENCODING=b;TYPE=PNG:", withPhoto.Body, "The card a client fetches carries the photo.");
         Assert.AreEqual(listedEtag, withPhoto.Header("ETag"),
            "The listing and the fetched card agree about the ETag, though only one of them read the photo.");
         Assert.AreEqual(withPhoto.Body.Length.ToString(), DavPropertyFor(tags.Body, cardName, "getcontentlength"),
            "And the length the listing gave is the length of what a GET serves - photo and all.");

         // An account that has opted out is out of the collection as it is out
         // of the listing.
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":true}").Status);
         Response afterOptOut = Dav("PROPFIND", DirectoryCollection,
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getetag/></D:prop></D:propfind>", "Depth: 1\r\n");
         StringAssert.DoesNotContain("account-" + _ada.ID + ".vcf", afterOptOut.Body);
         Assert.AreEqual(404, Dav("GET", DirectoryCollection + "account-" + _ada.ID + ".vcf", null, null).Status);
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"opt_out\":false}").Status);
      }

      [Test]
      [Description("A photo that is not an image is refused by its bytes, whatever it is called, and one past 100 KB is refused by its size; neither is stored.")]
      public void APhotoIsJudgedByItsBytes()
      {
         string notAnImage = Convert.ToBase64String(Encoding.ASCII.GetBytes("MZ this is an executable, not a photograph"));
         Response refused = As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"photo\":\"" + notAnImage + "\"}");
         Assert.AreEqual(400, refused.Status, refused.Body);
         StringAssert.Contains("JPEG", refused.Body);

         string large = Convert.ToBase64String(MakeLargePng());
         Response toobig = As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"photo\":\"" + large + "\"}");
         Assert.AreEqual(400, toobig.Status, toobig.Body);

         StringAssert.Contains("\"has_photo\":false", As(AdaAddress, "GET", "/api/v1/me/directory/me", null).Body,
            "Neither was stored.");

         // And a photo removed by sending null goes.
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"photo\":\"" + PhotoBase64 + "\"}").Status);
         StringAssert.Contains("\"has_photo\":true", As(AdaAddress, "GET", "/api/v1/me/directory/me", null).Body);
         Assert.AreEqual(200, As(AdaAddress, "PUT", "/api/v1/me/directory/me", "{\"photo\":null}").Status);
         StringAssert.Contains("\"has_photo\":false", As(AdaAddress, "GET", "/api/v1/me/directory/me", null).Body);
         Assert.AreEqual(404, As(AdaAddress, "GET", "/api/v1/me/directory/me/photo", null).Status);
      }

      // A PNG header followed by enough bytes to pass the ceiling: the size is
      // judged before the format, so this proves the size rule rather than the
      // format one.
      private static byte[] MakeLargePng()
      {
         byte[] header = { 0x89, (byte) 'P', (byte) 'N', (byte) 'G', 0x0D, 0x0A, 0x1A, 0x0A };
         var bytes = new byte[110 * 1024];
         Array.Copy(header, bytes, header.Length);
         return bytes;
      }

      /// <summary>
      ///    One property of one resource out of a multistatus: the D:response
      ///    whose href names the card, then the property's text, unescaped.
      ///    Null when the response has no such property.
      /// </summary>
      private static string DavPropertyFor(string xml, string hrefFragment, string property)
      {
         foreach (string block in xml.Split(new[] { "<D:response>" }, StringSplitOptions.None))
         {
            if (block.IndexOf(hrefFragment, StringComparison.Ordinal) < 0)
               continue;

            int open = block.IndexOf("<D:" + property + ">", StringComparison.Ordinal);
            int close = block.IndexOf("</D:" + property + ">", StringComparison.Ordinal);
            if (open < 0 || close < open)
               continue;

            open += property.Length + 4;
            return block.Substring(open, close - open)
               .Replace("&quot;", "\"").Replace("&lt;", "<").Replace("&gt;", ">").Replace("&amp;", "&");
         }

         return null;
      }

      private Response Admin(string method, string path, string body)
      {
         return Raw(RestPort, method, path, Auth("Administrator", AdminPassword), body, "application/json", null);
      }

      private Response As(string address, string method, string path, string body)
      {
         return Raw(RestPort, method, path, Auth(address, Password), body, "application/json", null);
      }

      private Response Dav(string method, string path, string body, string extraHeaders)
      {
         return Raw(WebServicesPort, method, path, Auth(ColleagueAddress, Password), body,
            "application/xml; charset=utf-8", "X-Forwarded-Proto: https\r\n" + (extraHeaders ?? ""));
      }

      private static string Auth(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      // ------------------------------------------------------------- raw HTTP

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public string HeaderText = "";
         public readonly Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : null;
         }
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
