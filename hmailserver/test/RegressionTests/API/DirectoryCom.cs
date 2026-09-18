// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The directory entry over COM: Account.DirectoryTitle and the eight properties
   ///    beside it, which are what the desktop Control Panel's Directory entry tab
   ///    reads and writes.
   ///
   ///    The entry is a row of hm_directory rather than a column of hm_accounts, so
   ///    what is proved here is that it behaves like every other property of the
   ///    object: read at the first get, kept in the object until Save(), written by
   ///    Save() and not before, and refused by name when it is too long for its
   ///    column. The photo is judged by its bytes, as it is over REST.
   ///
   ///    The properties are reached through <c>dynamic</c> rather than through the
   ///    interop's own type, as the quarantine digest's fixture reaches the settings
   ///    it added: the interop assembly is generated from the REGISTERED type library,
   ///    so a member added to the IDL is not in it until the server has been rebuilt
   ///    and registered, and a fixture that would not compile until then is a fixture
   ///    nobody can build. Late binding is IDispatch, which is registered for
   ///    everything, so this compiles today and exercises the real properties the
   ///    moment the build carries them.
   ///
   ///    Windows only - it is COM - and so linked into RegressionTests alone. What the
   ///    Linux suite covers of this row is RestApiDirectory, over the REST and DAV
   ///    listeners, which is where a Linux server's administrator reaches it.
   /// </summary>
   [TestFixture]
   public class DirectoryCom : TestFixtureBase
   {
      private const string Password = "Directory-Com-Passw0rd!";

      // The one-pixel PNG the REST fixture uses, for the same reason: the bytes
      // decide, so this is a real PNG.
      private const string PhotoBase64 =
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

      [Test]
      [Description("The nine directory properties are read, written and kept by Save(); nothing is written before Save; a field too long for its column is refused by name; a photo that is not an image is refused by its bytes; and the row goes with the account.")]
      public void TheEntryIsAPropertyOfTheAccount()
      {
         Account created = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "comdir@" + _domain.Name, Password);
         string address = created.Address;
         dynamic account = created;

         // An account that has never had an entry has no row, and every field
         // reads empty rather than failing.
         ClassicAssert.AreEqual("", (string) account.DirectoryTitle);
         ClassicAssert.AreEqual("", (string) account.DirectoryDepartment);
         ClassicAssert.AreEqual("", (string) account.DirectoryPhone);
         ClassicAssert.AreEqual("", (string) account.DirectoryMobile);
         ClassicAssert.AreEqual("", (string) account.DirectoryOffice);
         ClassicAssert.AreEqual("", (string) account.DirectoryNote);
         ClassicAssert.AreEqual("", (string) account.DirectoryPhoto);
         ClassicAssert.AreEqual("", (string) account.DirectoryPhotoType);
         ClassicAssert.IsFalse((bool) account.DirectoryOptOut);

         account.DirectoryTitle = "Head of Engines";
         account.DirectoryDepartment = "Engineering";
         account.DirectoryPhone = "+44 20 7946 0000";
         account.DirectoryMobile = "+44 7700 900000";
         account.DirectoryOffice = "Marylebone, 2nd floor";
         account.DirectoryNote = "Asked to be listed as Ada.";
         account.DirectoryOptOut = true;

         // Not written until Save: a second object opened on the same account
         // sees the stored row, which is still empty.
         dynamic before = _domain.Accounts.ItemByAddress[address];
         ClassicAssert.AreEqual("", (string) before.DirectoryTitle, "A property write reaches the row only at Save().");

         created.Save();

         dynamic after = _domain.Accounts.ItemByAddress[address];
         ClassicAssert.AreEqual("Head of Engines", (string) after.DirectoryTitle);
         ClassicAssert.AreEqual("Engineering", (string) after.DirectoryDepartment);
         ClassicAssert.AreEqual("+44 20 7946 0000", (string) after.DirectoryPhone);
         ClassicAssert.AreEqual("+44 7700 900000", (string) after.DirectoryMobile);
         ClassicAssert.AreEqual("Marylebone, 2nd floor", (string) after.DirectoryOffice);
         ClassicAssert.AreEqual("Asked to be listed as Ada.", (string) after.DirectoryNote);
         ClassicAssert.IsTrue((bool) after.DirectoryOptOut);

         // A field too long for its column is refused by name rather than
         // truncated into something nobody typed.
         try
         {
            after.DirectoryTitle = new string('x', 101);
            Assert.Fail("A title of 101 characters should have been refused.");
         }
         catch (Exception ex) when (!(ex is AssertionException))
         {
            StringAssert.Contains("Title", ex.Message);
         }

         // The photo: kept as base64, its media type read from the bytes.
         after.DirectoryPhoto = PhotoBase64;
         after.Save();

         dynamic withPhoto = _domain.Accounts.ItemByAddress[address];
         ClassicAssert.AreEqual("image/png", (string) withPhoto.DirectoryPhotoType);
         ClassicAssert.AreEqual(PhotoBase64, (string) withPhoto.DirectoryPhoto);

         try
         {
            withPhoto.DirectoryPhoto = Convert.ToBase64String(Encoding.ASCII.GetBytes("MZ not a photograph"));
            Assert.Fail("Bytes that are not an image should have been refused.");
         }
         catch (Exception ex) when (!(ex is AssertionException))
         {
            StringAssert.Contains("JPEG", ex.Message);
         }

         // An empty string removes the photo, and the type goes with it.
         withPhoto.DirectoryPhoto = "";
         withPhoto.Save();
         dynamic withoutPhoto = _domain.Accounts.ItemByAddress[address];
         ClassicAssert.AreEqual("", (string) withoutPhoto.DirectoryPhotoType);
         ClassicAssert.AreEqual("", (string) withoutPhoto.DirectoryPhoto);

         // The row goes with the account, as the foreign key says it does:
         // a new account at the same address starts with nothing.
         withoutPhoto.Delete();
         dynamic again = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         ClassicAssert.AreEqual("", (string) again.DirectoryTitle, "The entry went with the account it belonged to.");
         ClassicAssert.IsFalse((bool) again.DirectoryOptOut);
      }
   }
}
