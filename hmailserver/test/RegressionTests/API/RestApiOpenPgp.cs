// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The OpenPGP public keys this server holds, over REST and over COM.
   ///
   ///    THE PROMISE THESE TESTS EXIST FOR: this server never holds a secret key.
   ///    A transferable secret key is refused at every entry point with a sentence
   ///    saying so, and there are three of them here - the account's own PUT, the
   ///    administrator's PUT and Utilities.ImportOpenPGPKey - each with its own
   ///    test, because "it is refused over REST" is not a promise about COM.
   ///
   ///    The other half is that what IS stored has been read rather than believed:
   ///    three letters, a key with no packets that add up and a key whose user ids
   ///    name somebody else are each refused, the last of them unless an
   ///    administrator waives that one rule for that one upload.
   ///
   ///    The serving of the keys is not here - it is on the web services listener,
   ///    and OpenPgpWebKeyDirectory covers it.
   /// </summary>
   [TestFixture]
   public class RestApiOpenPgp : TestFixtureBase
   {
      private static int RestPort = 9141;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Original-Passw0rd!";

      private Account _account;

      private string Address
      {
         get { return "pgp@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status", AdminHeader());
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         OpenPgpTestSupport.RestoreDefaults(_settings);
         RestListener.Stop();
         _application.Reinitialize();
      }

      // ---- what is stored, and what is not -----------------------------------

      [Test]
      [Description("The account uploads its own key: the fingerprint is the one GnuPG prints, and the answer says "
                   + "where a client would look for it")]
      public void AnAccountStoresItsOwnKey()
      {
         (int status, string body) stored = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");

         Assert.AreEqual(201, stored.status, "Body: " + stored.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.SuiteKeyFingerprint + "\"", stored.body);
         StringAssert.Contains("\"version\":4", stored.body);
         StringAssert.Contains("\"user_id\":\"Suite Key <" + Address + ">\"", stored.body);

         // The URL a correspondent's client would fetch, which is the whole point
         // of the row: the hashed local part of "pgp", and not the address.
         StringAssert.Contains("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp,
            stored.body);

         (int status, string body) listed = Http("GET", "/api/v1/me/openpgp", UserHeader());
         Assert.AreEqual(200, listed.status, "Body: " + listed.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.SuiteKeyFingerprint + "\"", listed.body);

         // Uploaded is not published: the account's own switch is still off, and
         // so are the two above it.
         StringAssert.Contains("\"publish\":false", listed.body);
         StringAssert.Contains("\"published\":false", listed.body);
      }

      [Test]
      [Description("The same key armoured, as a person pastes it out of gpg --armor --export, is the same key")]
      public void AnArmouredBlockIsTheSameKey()
      {
         string armoured = OpenPgpTestSupport.Armoured(OpenPgpTestSupport.SuiteKeyBase64).Replace("\n", "\\n");

         (int status, string body) stored = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + armoured + "\"}");

         Assert.AreEqual(201, stored.status, "Body: " + stored.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.SuiteKeyFingerprint + "\"", stored.body);
      }

      [Test]
      [Description("A SECRET key is refused by the account's own route, with a sentence saying it is one")]
      public void ASecretKeyIsRefusedFromTheAccount()
      {
         (int status, string body) refused = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SecretKeyBase64 + "\"}");

         Assert.AreEqual(400, refused.status, "A secret key must never be stored. Body: " + refused.body);
         StringAssert.Contains("SECRET", refused.body);

         (int status, string body) listed = Http("GET", "/api/v1/me/openpgp", UserHeader());
         StringAssert.Contains("\"keys\":[]", listed.body);
      }

      [Test]
      [Description("An armoured PRIVATE KEY BLOCK is refused by its header line, before a byte of it is decoded")]
      public void AnArmouredSecretKeyIsRefused()
      {
         string armoured = OpenPgpTestSupport.ArmouredSecretKey.Replace("\n", "\\n");

         (int status, string body) refused = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + armoured + "\"}");

         Assert.AreEqual(400, refused.status, "Body: " + refused.body);
         StringAssert.Contains("SECRET", refused.body);
      }

      [Test]
      [Description("A SECRET key is refused by the administrator's route as well, and by Utilities.ImportOpenPGPKey")]
      public void ASecretKeyIsRefusedEverywhereElse()
      {
         (int status, string body) refused = Http("PUT", "/api/v1/accounts/" + Address + "/openpgp", AdminHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SecretKeyBase64 + "\"}");

         Assert.AreEqual(400, refused.status, "Body: " + refused.body);
         StringAssert.Contains("SECRET", refused.body);

         COMException raised = null;
         try
         {
            OpenPgpTestSupport.ImportKey(_application, Address, OpenPgpTestSupport.SecretKeyBase64, false);
         }
         catch (COMException exception)
         {
            raised = exception;
         }

         Assert.IsNotNull(raised, "Utilities.ImportOpenPGPKey accepted a secret key.");
         StringAssert.Contains("SECRET", raised.Message);
      }

      [Test]
      [Description("Three letters are not a key, and the refusal says so rather than storing them")]
      public void AMalformedKeyIsRefused()
      {
         (int status, string body) refused = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.MalformedKeyBase64 + "\"}");

         Assert.AreEqual(400, refused.status, "Body: " + refused.body);
         StringAssert.Contains("OpenPGP", refused.body);
      }

      [Test]
      [Description("A key truncated part way through its last packet is refused: the packets have to add up")]
      public void ATruncatedKeyIsRefused()
      {
         byte[] whole = Convert.FromBase64String(OpenPgpTestSupport.SuiteKeyBase64);
         var cut = new byte[whole.Length - 20];
         Array.Copy(whole, cut, cut.Length);

         (int status, string body) refused = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + Convert.ToBase64String(cut) + "\"}");

         Assert.AreEqual(400, refused.status, "Body: " + refused.body);
      }

      [Test]
      [Description("A key whose user ids name somebody else is refused for this account - and an administrator can "
                   + "waive that one rule for that one upload")]
      public void AKeyForAnotherAddressIsRefusedUnlessAnAdministratorSaysOtherwise()
      {
         // The account itself has no such waiver, and its PUT has no field for one.
         (int status, string body) refused = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.OtherKeyBase64 + "\"}");

         Assert.AreEqual(400, refused.status, "Body: " + refused.body);
         StringAssert.Contains(Address, refused.body);

         (int status, string body) alsoRefused = Http("PUT", "/api/v1/accounts/" + Address + "/openpgp", AdminHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.OtherKeyBase64 + "\"}");

         Assert.AreEqual(400, alsoRefused.status, "Without the waiver the administrator is refused too. Body: " + alsoRefused.body);

         (int status, string body) allowed = Http("PUT", "/api/v1/accounts/" + Address + "/openpgp", AdminHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.OtherKeyBase64 + "\",\"allow_address_mismatch\":true}");

         Assert.AreEqual(201, allowed.status, "Body: " + allowed.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.OtherKeyFingerprint + "\"", allowed.body);
      }

      [Test]
      [Description("Uploading the same key again replaces the row rather than being refused, and the account still "
                   + "holds one key")]
      public void TheSameKeyAgainReplacesIt()
      {
         (int status, string body) first = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");
         Assert.AreEqual(201, first.status, "Body: " + first.body);

         (int status, string body) again = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");
         Assert.AreEqual(200, again.status, "A key the account already holds is replaced. Body: " + again.body);

         (int status, string body) listed = Http("GET", "/api/v1/me/openpgp", UserHeader());
         Assert.AreEqual(1, Occurrences(listed.body, "\"fingerprint\""),
            "The account should hold one key, not two. Body: " + listed.body);
      }

      [Test]
      [Description("The account removes its own key by fingerprint, and a fingerprint it does not hold is a 404")]
      public void AnAccountRemovesItsOwnKey()
      {
         Http("PUT", "/api/v1/me/openpgp", UserHeader(), "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");

         (int status, string body) missing = Http("DELETE",
            "/api/v1/me/openpgp/" + OpenPgpTestSupport.OtherKeyFingerprint, UserHeader());
         Assert.AreEqual(404, missing.status, "Body: " + missing.body);

         (int status, string body) removed = Http("DELETE",
            "/api/v1/me/openpgp/" + OpenPgpTestSupport.SuiteKeyFingerprint, UserHeader());
         Assert.AreEqual(200, removed.status, "Body: " + removed.body);

         (int status, string body) listed = Http("GET", "/api/v1/me/openpgp", UserHeader());
         StringAssert.Contains("\"keys\":[]", listed.body);
      }

      [Test]
      [Description("The account's own publishing switch is its own: PUT /api/v1/me/openpgp/publish turns it on and off")]
      public void AnAccountChoosesWhetherItsKeysArePublished()
      {
         Http("PUT", "/api/v1/me/openpgp", UserHeader(), "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");

         (int status, string body) on = Http("PUT", "/api/v1/me/openpgp/publish", UserHeader(), "{\"publish\":true}");
         Assert.AreEqual(200, on.status, "Body: " + on.body);
         StringAssert.Contains("\"publish\":true", on.body);

         // And it is the ACCOUNT's row that changed, which COM reads back.
         Account fresh = _domain.Accounts.ItemByAddress[Address];
         Assert.IsTrue(OpenPgpTestSupport.AccountPublishes(fresh), "The account's own switch was not saved.");

         (int status, string body) off = Http("PUT", "/api/v1/me/openpgp/publish", UserHeader(), "{\"publish\":false}");
         Assert.AreEqual(200, off.status, "Body: " + off.body);
         StringAssert.Contains("\"publish\":false", off.body);

         (int status, string body) refused = Http("PUT", "/api/v1/me/openpgp/publish", UserHeader(), "{}");
         Assert.AreEqual(400, refused.status, "Body: " + refused.body);
      }

      [Test]
      [Description("An administrator stores a key for an account and sees it in the domain's listing, with whether "
                   + "it is actually being served")]
      public void AnAdministratorStoresAKeyAndSeesTheDomainsListing()
      {
         (int status, string body) stored = Http("PUT", "/api/v1/accounts/" + Address + "/openpgp", AdminHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\",\"publish\":true}");

         Assert.AreEqual(201, stored.status, "Body: " + stored.body);

         (int status, string body) listing = Http("GET", "/api/v1/domains/" + _domain.Name + "/openpgp", AdminHeader());
         Assert.AreEqual(200, listing.status, "Body: " + listing.body);
         StringAssert.Contains("\"address\":\"" + Address + "\"", listing.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.SuiteKeyFingerprint + "\"", listing.body);

         // The account said yes; the server and the domain have not, so the key
         // is listed and is NOT being served. That distinction is what the page
         // is opened to see.
         StringAssert.Contains("\"server_enabled\":false", listing.body);
         StringAssert.Contains("\"published\":false", listing.body);

         (int status, string body) removed = Http("DELETE",
            "/api/v1/accounts/" + Address + "/openpgp/" + OpenPgpTestSupport.SuiteKeyFingerprint, AdminHeader());
         Assert.AreEqual(200, removed.status, "Body: " + removed.body);
      }

      [Test]
      [Description("The four server settings are a settings group like any other, and the ranges are refused rather "
                   + "than clamped")]
      public void TheSettingsGroupAnswersAndIsChecked()
      {
         (int status, string body) read = Http("GET", "/api/v1/settings/openpgp", AdminHeader());
         Assert.AreEqual(200, read.status, "Body: " + read.body);
         StringAssert.Contains("\"web_key_directory_enabled\":false", read.body);
         StringAssert.Contains("\"keys_per_account\":8", read.body);

         (int status, string body) written = Http("PUT", "/api/v1/settings/openpgp", AdminHeader(),
            "{\"keys_per_account\":2}");
         Assert.AreEqual(200, written.status, "Body: " + written.body);
         Assert.AreEqual(2, OpenPgpTestSupport.KeysPerAccount(_settings));

         (int status, string body) refused = Http("PUT", "/api/v1/settings/openpgp", AdminHeader(),
            "{\"keys_per_account\":0}");
         Assert.AreEqual(400, refused.status, "0 keys would refuse every key. Body: " + refused.body);
         Assert.AreEqual(2, OpenPgpTestSupport.KeysPerAccount(_settings), "Nothing should have been applied.");
      }

      [Test]
      [Description("An account holds at most OpenPGPKeysPerAccount keys, and the refusal says what the limit is")]
      public void AnAccountIsHeldToTheKeyLimit()
      {
         OpenPgpTestSupport.SetKeysPerAccount(_settings, 1);

         (int status, string body) first = Http("PUT", "/api/v1/accounts/" + Address + "/openpgp", AdminHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");
         Assert.AreEqual(201, first.status, "Body: " + first.body);

         (int status, string body) second = Http("PUT", "/api/v1/accounts/" + Address + "/openpgp", AdminHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.OtherKeyBase64 + "\",\"allow_address_mismatch\":true}");
         Assert.AreEqual(400, second.status, "Body: " + second.body);
         StringAssert.Contains("1", second.body);
      }

      [Test]
      [Description("A key larger than OpenPGPKeyMaximumBytes is refused, and the refusal names the cap")]
      public void AKeyOverTheSizeCapIsRefused()
      {
         // 1024 is above every key here, so the cap is not what refuses anything yet.
         OpenPgpTestSupport.SetKeyMaximumBytes(_settings, 1024);
         Assert.AreEqual(1024, OpenPgpTestSupport.KeyMaximumBytes(_settings));

         (int status, string body) accepted = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\"}");
         Assert.AreEqual(201, accepted.status, "232 bytes is under a cap of 1024. Body: " + accepted.body);

         Http("DELETE", "/api/v1/me/openpgp/" + OpenPgpTestSupport.SuiteKeyFingerprint, UserHeader());

         // And a key past it - 1024 is the lowest cap the setting allows, and
         // the RSA key is 1030 bytes - is refused with the cap in the sentence.
         (int status, string body) refused = Http("PUT", "/api/v1/me/openpgp", UserHeader(),
            "{\"key\":\"" + OpenPgpTestSupport.RsaKeyBase64 + "\"}");

         Assert.AreEqual(400, refused.status, "Body: " + refused.body);
         StringAssert.Contains("1024", refused.body);
      }

      // ---- COM ----------------------------------------------------------------

      [Test]
      [Description("Utilities.ImportOpenPGPKey stores a key for an account, Account.OpenPGPKeys reads it back, and "
                   + "RemoveOpenPGPKey takes it away")]
      public void TheKeyStoreIsReachedOverCom()
      {
         string stored = OpenPgpTestSupport.ImportKey(_application, Address, OpenPgpTestSupport.SuiteKeyBase64, false);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.SuiteKeyFingerprint + "\"", stored);

         Account account = _domain.Accounts.ItemByAddress[Address];
         string document = OpenPgpTestSupport.AccountKeys(account);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.SuiteKeyFingerprint + "\"", document);
         StringAssert.Contains("\"hashed_local_part\":\"" + OpenPgpTestSupport.HashedPgp + "\"", document);
         StringAssert.Contains("\"published\":false", document);

         Assert.IsFalse(OpenPgpTestSupport.RemoveKey(_application, Address, OpenPgpTestSupport.OtherKeyFingerprint),
            "A fingerprint the account does not hold is not removed.");
         Assert.IsTrue(OpenPgpTestSupport.RemoveKey(_application, Address, OpenPgpTestSupport.SuiteKeyFingerprint));

         StringAssert.Contains("\"keys\":[]", OpenPgpTestSupport.AccountKeys(_domain.Accounts.ItemByAddress[Address]));
      }

      [Test]
      [Description("The three switches are readable and writable over COM, and every one of them ships off")]
      public void TheThreeSwitchesAreOffAndReachableOverCom()
      {
         Assert.IsFalse(OpenPgpTestSupport.WebKeyDirectoryEnabled(_settings), "The server switch ships off.");
         Assert.IsFalse(OpenPgpTestSupport.DomainPublishes(_domain), "A domain ships not publishing.");
         Assert.IsFalse(OpenPgpTestSupport.DomainPublishesNewAccounts(_domain), "New accounts ship not publishing.");

         Account account = _domain.Accounts.ItemByAddress[Address];
         Assert.IsFalse(OpenPgpTestSupport.AccountPublishes(account), "An account ships not publishing.");

         OpenPgpTestSupport.SetWebKeyDirectoryEnabled(_settings, true);
         Assert.IsTrue(OpenPgpTestSupport.WebKeyDirectoryEnabled(_settings));

         OpenPgpTestSupport.SetDomainPublishes(_domain, true);
         _domain.Save();
         Assert.IsTrue(OpenPgpTestSupport.DomainPublishes(_application.Domains.ItemByName[_domain.Name]));

         OpenPgpTestSupport.SetAccountPublishes(account, true);
         account.Save();
         Assert.IsTrue(OpenPgpTestSupport.AccountPublishes(_domain.Accounts.ItemByAddress[Address]));

         OpenPgpTestSupport.SetDomainPublishes(_domain, false);
         _domain.Save();
      }

      [Test]
      [Description("A new account starts from its domain's OpenPGPPublishNewAccounts, and an account that already "
                   + "exists is never changed by it")]
      public void ANewAccountFollowsTheDomainsDefault()
      {
         OpenPgpTestSupport.SetDomainPublishesNewAccounts(_domain, true);
         _domain.Save();

         try
         {
            Account made = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "fresh@" + _domain.Name, UserPassword);
            Assert.IsTrue(OpenPgpTestSupport.AccountPublishes(_domain.Accounts.ItemByAddress[made.Address]),
               "An account created while the domain's default is on should start published.");

            // And the account that was here before the default was switched on is
            // untouched, which is what keeps this from publishing a whole domain.
            Assert.IsFalse(OpenPgpTestSupport.AccountPublishes(_domain.Accounts.ItemByAddress[Address]),
               "An account that already existed must not be changed by the domain's default.");
         }
         finally
         {
            OpenPgpTestSupport.SetDomainPublishesNewAccounts(_domain, false);
            _domain.Save();
         }
      }

      [Test]
      [Description("Utilities.ImportOpenPGPKey and RemoveOpenPGPKey answer a domain administrator of another domain "
                   + "with one refusal whether the address exists or not, so neither can be used to ask which addresses "
                   + "this server has")]
      public void AnotherDomainsAdministratorCannotTellAMissingAddressFromAnExistingOne()
      {
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("other-pgp.test");
         Account deputy = SingletonProvider<TestSetup>.Instance.AddAccount(other, "deputy@other-pgp.test", UserPassword);
         deputy.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         deputy.Save();

         var asDeputy = new Application();
         Assert.IsNotNull(asDeputy.Authenticate(deputy.Address, UserPassword));

         string forExisting = Refusal(() => OpenPgpTestSupport.ImportKey(asDeputy, Address, OpenPgpTestSupport.SuiteKeyBase64, false));
         string forMissing = Refusal(() => OpenPgpTestSupport.ImportKey(asDeputy, "nobody@" + _domain.Name, OpenPgpTestSupport.SuiteKeyBase64, false));
         Assert.AreEqual(forExisting, forMissing,
            "The import's refusal differs for an address that exists and one that does not, which tells another domain's administrator which addresses this server has.");
         StringAssert.DoesNotContain("No account", forMissing, "Another domain's administrator was told that an address does not exist.");

         string removeExisting = Refusal(() => OpenPgpTestSupport.RemoveKey(asDeputy, Address, OpenPgpTestSupport.SuiteKeyFingerprint));
         string removeMissing = Refusal(() => OpenPgpTestSupport.RemoveKey(asDeputy, "nobody@" + _domain.Name, OpenPgpTestSupport.SuiteKeyFingerprint));
         Assert.AreEqual(removeExisting, removeMissing, "The removal's refusal differs for an address that exists and one that does not.");
         StringAssert.DoesNotContain("No account", removeMissing);

         // In the deputy's own domain the honest answer is the honest answer.
         string ownDomain = Refusal(() => OpenPgpTestSupport.ImportKey(asDeputy, "nobody@other-pgp.test", OpenPgpTestSupport.SuiteKeyBase64, false));
         StringAssert.Contains("No account of that address exists", ownDomain);
      }

      private static string Refusal(Action call)
      {
         try
         {
            call();
         }
         catch (COMException exception)
         {
            return exception.Message;
         }
         catch (Exception exception) when (exception.InnerException is COMException inner)
         {
            return inner.Message;
         }

         Assert.Fail("The call was expected to be refused and was not.");
         return "";
      }

      // ---- the plumbing -------------------------------------------------------

      private static int Occurrences(string text, string needle)
      {
         int count = 0;
         int at = 0;
         while ((at = text.IndexOf(needle, at, StringComparison.Ordinal)) >= 0)
         {
            count++;
            at += needle.Length;
         }
         return count;
      }

      private static string AdminHeader()
      {
         return BasicHeader("Administrator", AdminPassword);
      }

      private string UserHeader()
      {
         return BasicHeader(Address, UserPassword);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
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
            using (var memory = new MemoryStream())
            {
               byte[] body = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.1\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               head.Append("Content-Type: application/json\r\n");
               head.Append("Content-Length: " + body.Length + "\r\n");
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (body.Length > 0)
                  stream.Write(body, 0, body.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int status = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out status);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
