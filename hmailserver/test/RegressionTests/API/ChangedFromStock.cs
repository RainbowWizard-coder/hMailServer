// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    What this server has changed from stock: GET /api/v1/settings/changed and
   ///    COM's Settings.GetChangedFromDefault, which are the same document built by
   ///    the same function.
   ///
   ///    The four things worth pinning, and the reason each one is here rather than
   ///    left to a reading of the code:
   ///
   ///    A setting that is not the shipped value is listed WITH the shipped value.
   ///    Listing the name alone would be half an answer - "LogLevel is 5" means
   ///    nothing to somebody who does not know that 9 is what it ships as - and the
   ///    shipped value comes from a generated header, so the test is also what
   ///    proves the generator put the right number in it.
   ///
   ///    A setting put back to the shipped value LEAVES the list, both ways round:
   ///    stored with that value, and removed altogether. Without that the list
   ///    grows by one every time anybody touches a setting and is useless within a
   ///    month.
   ///
   ///    A secret is named and never shown. This list is what somebody pastes into
   ///    a support message.
   ///
   ///    The list is the SERVER'S state, not the caller's. A test that stores a
   ///    setting and then finds it in the list would pass against an answer built
   ///    from nothing but the last write, and on a server built from a create
   ///    script and an installer's own ini the honest list is EMPTY - so there is
   ///    no non-empty baseline to lean on either. What is asserted instead is that
   ///    the list is the one from a moment ago plus exactly the one setting the
   ///    test changed, with the count moved by exactly one.
   ///
   ///    A value the file's [SettingsOverride] section is forcing is in the list,
   ///    even with no row and no [Settings] line of its own. That section is the
   ///    door for a database that is unreachable or holds a value the server will
   ///    not start on; it is what the server is RUNNING on, and it is invisible to
   ///    every page and every backup.
   ///
   ///    The command line and the API agree, because they are the same bytes: the
   ///    COM answer and the REST answer are compared as whole documents, so that
   ///    one interface cannot come to describe the server differently from another.
   ///
   ///    The Settings members are reached through dynamic on purpose: the suite
   ///    compiles against the REGISTERED type library, which does not carry a
   ///    member until somebody has run an elevated registration, and a fixture that
   ///    bound early would not compile until then. Roles.cs and BounceTestSupport
   ///    do the same for the same reason.
   /// </summary>
   [TestFixture]
   public class ChangedFromStock : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9138;
      private const string AdminPassword = "testar";

      // A [Settings] key with a shipped default that is NOT empty, which is the
      // case worth testing: an empty default would let a list that simply reported
      // every stored value pass. Inert unless DMARC aggregate reports are sent,
      // and put back in TearDown either way.
      private const string OrganizationKey = "DmarcRptOrganizationName";
      private const string ShippedOrganization = "hMailServer";

      // A [Settings] key no shipped setting uses, as IniSettingsOverCom's probe is:
      // a name an administrator might store for a script of their own, which the
      // API and COM accept on purpose and which the server ships no default for.
      private const string ProbeKey = "ChangedFromStockProbe";

      // A key whose name says its value is a secret. Empty by default, so removing
      // it in TearDown is what puts it back.
      private const string SecretKey = "OAuth2HmacSecret";
      private const string SecretValue = "correct-horse-battery-staple";

      // Set by the one test that writes a [SettingsOverride] line. The server
      // announces an override by name at EVERY start, so the report has to be
      // consumed and the log cleared, and only that test may clear it - a fixture
      // that deleted the ERROR log after each of its tests would be hiding the
      // faults the next fixture's SetUp exists to find.
      private bool forcedFromTheFile_;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // The override first: while the line is in the file it shadows the
            // stored value and is announced at every start.
            if (forcedFromTheFile_)
               IniFileSetting.DeleteFileOnly("SettingsOverride", OrganizationKey);

            IniFileSetting.Delete(OrganizationKey);
            IniFileSetting.Delete(ProbeKey);
            IniFileSetting.Delete(SecretKey);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();

            if (forcedFromTheFile_)
            {
               // After the line has gone and the server has re-read the file there
               // is nothing left to announce, so what is cleared here is the report
               // from the starts while it was there - including one this teardown
               // has just provoked.
               forcedFromTheFile_ = false;
               LogHandler.DeleteErrorLog();
            }
         }
      }

      [Test]
      [Description("A setting whose value is not the shipped one is listed with the shipped value beside it, and says which store holds it.")]
      public void ASettingThatIsNotTheShippedValueIsListedWithItBesideIt()
      {
         Assert.IsFalse(Names(Changed()).Contains(OrganizationKey),
            OrganizationKey + " was already listed before this test changed it.");

         IniFileSetting.Write(OrganizationKey, "Acme Mail");

         string entry = EntryFor(Changed(), OrganizationKey);
         StringAssert.Contains("\"store\":\"ini\"", entry);
         StringAssert.Contains("\"value\":\"Acme Mail\"", entry);
         StringAssert.Contains("\"default\":\"" + ShippedOrganization + "\"", entry,
            "The shipped value comes from IniSettingsBaseline.h, which build/check-ini-coverage.py generates "
            + "from the third argument of the read in IniFileSettings.cpp.");
         StringAssert.Contains("\"known\":true", entry);
         StringAssert.Contains("\"secret\":false", entry);
         StringAssert.Contains("\"overridden\":false", entry,
            "Nothing is forcing it from the file; it was stored.");
      }

      [Test]
      [Description("A setting stored with the shipped value, and a setting removed altogether, both leave the list.")]
      public void ASettingReturnedToTheShippedValueLeavesTheList()
      {
         IniFileSetting.Write(OrganizationKey, "Acme Mail");
         Assert.IsTrue(Names(Changed()).Contains(OrganizationKey), "It should be listed once it has been changed.");

         // Stored, with the value the server ships. The row is still there, and
         // the setting is still not a difference from stock.
         IniFileSetting.Write(OrganizationKey, ShippedOrganization);
         CollectionAssert.DoesNotContain(Names(Changed()), OrganizationKey,
            "A setting stored with the value this version ships with is not a change to anything.");

         IniFileSetting.Write(OrganizationKey, "Acme Mail");
         Assert.IsTrue(Names(Changed()).Contains(OrganizationKey));

         IniFileSetting.Delete(OrganizationKey);
         CollectionAssert.DoesNotContain(Names(Changed()), OrganizationKey,
            "A setting removed altogether is back at its default and out of the list.");
      }

      [Test]
      [Description("A setting whose name says its value is a secret is listed by name, marked secret, and its value is nowhere in the document.")]
      public void ASecretIsNamedAndNeverShown()
      {
         IniFileSetting.Write(SecretKey, SecretValue);

         string document = Changed();
         string entry = EntryFor(document, SecretKey);
         StringAssert.Contains("\"secret\":true", entry);
         StringAssert.Contains("\"value\":\"(set)\"", entry);
         StringAssert.DoesNotContain(SecretValue, document,
            "This list is what somebody pastes into a support message.");
      }

      [Test]
      [Description("A name the server ships no default for is listed as a difference all the same, and says it has no default rather than claiming the default is empty.")]
      public void ANameTheServerDoesNotReadIsListedWithNoDefault()
      {
         IniFileSetting.Write(ProbeKey, "yes");

         string entry = EntryFor(Changed(), ProbeKey);
         StringAssert.Contains("\"known\":false", entry,
            "Stock has no such value at all, so it is a difference - but the server cannot say what it should be.");
         StringAssert.Contains("\"value\":\"yes\"", entry);
      }

      [Test]
      [Description("The list is what the server holds and not what the caller just wrote: it is the list from before, plus the one setting this test changed, and it is short beside the number of settings the server has.")]
      public void TheListIsShortAndCountedAgainstEverySettingTheServerHolds()
      {
         // What the bench itself has changed, before this test changes anything.
         // Written out as well as asserted on: this is an installation like any
         // other, the suite's own fixtures have settings of their own, and a gate
         // that can read the list is worth having.
         string before = Changed();
         List<string> beforeNames = Names(before);
         Console.WriteLine("Changed from stock on this bench, before this test: "
                           + string.Join(", ", beforeNames));

         CollectionAssert.DoesNotContain(beforeNames, OrganizationKey,
            OrganizationKey + " was already listed before this test changed it.");

         // A key the server READS, deliberately: a name it does not read is
         // counted into considered as well as into changed, and this test is
         // about one setting moving one number.
         IniFileSetting.Write(OrganizationKey, "Acme Mail");

         string document = Changed();
         int changed = Number(document, "changed");
         int considered = Number(document, "considered");
         List<string> afterNames = Names(document);

         // THE assertion of this test. "changed >= 1" would be true of a server
         // that had changed nothing at all, since this test has just changed one -
         // and a server built from a create script and an installer's own ini
         // lists NOTHING, by design, so there is no non-empty baseline to lean on
         // either. What proves the list is built from the server's state rather
         // than from whatever the caller last wrote is that it is the list from a
         // moment ago, unchanged, with exactly this test's probe added to it.
         var expected = new List<string>(beforeNames) { OrganizationKey };
         expected.Sort(StringComparer.Ordinal);
         var actual = new List<string>(afterNames);
         actual.Sort(StringComparer.Ordinal);
         CollectionAssert.AreEqual(expected, actual,
            "The list after storing one setting is not the list before it plus that setting. Before: " + before
            + "  After: " + document);

         Assert.AreEqual(Number(before, "changed") + 1, changed,
            "Storing one setting moved the count by one and no more. " + document);

         Assert.Greater(considered, 300,
            "The two stores together hold several hundred settings; a count far below that means one of them was not read. "
            + document);
         Assert.AreEqual(Number(before, "considered"), considered,
            "Storing a setting the server reads changes nothing about how many settings there are.");
         Assert.AreEqual(changed, CountOf(document, "{\"store\":"),
            "changed must be the number of entries and not a number of its own.");

         // The point of the whole feature: what a person reads is a short list, not
         // a re-listing of the configuration. The bench is an installation like any
         // other - the suite has changed settings on it, and this says which.
         Assert.Less(changed, considered / 2,
            "The list is meant to be short enough to read. It holds: " + string.Join(", ", Names(document)));
      }

      [Test]
      [Description("A key the file's [SettingsOverride] section forces, which neither store holds a row or a line for, is in the list and says the file is forcing it.")]
      public void AValueForcedFromTheFileIsListedAndSaysWhereItComesFrom()
      {
         // Nothing has stored this key: the override is the only place it exists.
         // That is the case the section is FOR - a server started on a value its
         // database has never held, because the database was unreachable or held
         // one the server would not start on - and it is the case a list built
         // from the two stores alone cannot see.
         forcedFromTheFile_ = true;
         IniFileSetting.WriteFileOnly("SettingsOverride", OrganizationKey, "Forced Mail");
         _application.Reinitialize();

         string entry = EntryFor(Changed(), OrganizationKey);
         StringAssert.Contains("\"value\":\"Forced Mail\"", entry,
            "A value forced from the file is what this server is running on, and a list of what it has "
            + "changed that leaves it out is wrong in exactly the case the section exists for.");
         StringAssert.Contains("\"overridden\":true", entry,
            "Nothing else the server offers shows an override: not a page, not a backup, not an export.");
         StringAssert.Contains("\"default\":\"" + ShippedOrganization + "\"", entry);

         // Consumed here so that the standing report this test provoked does not
         // fail the SetUp of whatever runs next.
         CustomAsserts.AssertReportedError("SettingsOverride", OrganizationKey);
      }

      [Test]
      [Description("The COM call and the REST route answer the same document, byte for byte: they are one function.")]
      public void TheComCallAndTheRestRouteAnswerTheSameDocument()
      {
         IniFileSetting.Write(ProbeKey, "yes");

         string overRest = Changed();
         string overCom = ComChangedFromDefault(_settings);

         Assert.AreEqual(overRest, overCom,
            "Two interfaces describing the same server differently is the defect this feature exists to remove.");
      }

      [Test]
      [Description("One setting's shipped value is answered from either store, and a name the server ships no default for is refused by name.")]
      public void TheShippedValueOfOneSettingIsAnswered()
      {
         // A [Settings] key read as a number, one read as text, and a property of
         // the other store - the three shapes the answer has to cover.
         Assert.AreEqual("9", ComSettingDefault(_settings, "LogLevel"));
         Assert.AreEqual(ShippedOrganization, ComSettingDefault(_settings, OrganizationKey));
         Assert.AreEqual("4", ComSettingDefault(_settings, "smtpnoofretries"));

         // Matched without case, as the file matches keys.
         Assert.AreEqual("9", ComSettingDefault(_settings, "loglevel"));

         COMException refused = Assert.Throws<COMException>(() => ComSettingDefault(_settings, ProbeKey));
         StringAssert.Contains("no default", refused.Message);
      }

      [Test]
      [Description("openapi.json is JSON and describes the route, which is how a client that builds itself from the document finds it.")]
      public void TheOpenApiDocumentDescribesTheRoute()
      {
         (int status, string body) = Http("GET", "/api/v1/openapi.json");
         Assert.AreEqual(200, status, body);

         // Parsed rather than searched for a substring: a description with an
         // unescaped quote in it would leave a document that reads as JSON to a
         // grep and to nothing else, which this project has shipped once already.
         Dictionary<string, object> document = MiniJson.Object(body);
         var paths = (Dictionary<string, object>) document["paths"];

         Assert.IsTrue(paths.ContainsKey("/api/v1/settings/changed"),
            "openapi.json does not describe /api/v1/settings/changed");
      }

      // ---- reading the document ------------------------------------------------
      //
      // As text, the way every REST fixture in this suite reads its answers. The
      // entries are cut at the "store" field each one opens with rather than at a
      // brace, because a setting's VALUE may hold braces - BreachCheckUrl ships
      // with {prefix} in it - and cutting at the nearest brace would hand back
      // half an entry for a key like that.

      private static string Changed()
      {
         (int status, string body) = Http("GET", "/api/v1/settings/changed");
         Assert.AreEqual(200, status, body);
         return body;
      }

      private const string EntryStart = "{\"store\":";

      private static string EntryFor(string document, string name)
      {
         string marker = "\"name\":\"" + name + "\"";
         int at = document.IndexOf(marker, StringComparison.Ordinal);
         Assert.Greater(at, 0, name + " is not in the list. " + document);

         int open = document.LastIndexOf(EntryStart, at, StringComparison.Ordinal);
         Assert.Greater(open, -1, "The entry for " + name + " does not open with its store. " + document);

         int next = document.IndexOf(EntryStart, at, StringComparison.Ordinal);
         int end = next >= 0 ? next : document.LastIndexOf(']');
         Assert.Greater(end, open, "The entry for " + name + " is not one object. " + document);

         return document.Substring(open, end - open).TrimEnd(',');
      }

      private static List<string> Names(string document)
      {
         var names = new List<string>();
         const string marker = "\"name\":\"";
         int at = document.IndexOf(marker, StringComparison.Ordinal);
         while (at >= 0)
         {
            int start = at + marker.Length;
            int end = document.IndexOf('"', start);
            if (end < 0)
               break;
            names.Add(document.Substring(start, end - start));
            at = document.IndexOf(marker, end, StringComparison.Ordinal);
         }
         return names;
      }

      private static int Number(string document, string field)
      {
         string marker = "\"" + field + "\":";
         int at = document.IndexOf(marker, StringComparison.Ordinal);
         Assert.Greater(at, -1, field + " is not in the answer. " + document);

         int start = at + marker.Length;
         int end = start;
         while (end < document.Length && (char.IsDigit(document[end]) || document[end] == '-'))
            end++;

         return int.Parse(document.Substring(start, end - start));
      }

      private static int CountOf(string document, string what)
      {
         int count = 0;
         int at = document.IndexOf(what, StringComparison.Ordinal);
         while (at >= 0)
         {
            count++;
            at = document.IndexOf(what, at + what.Length, StringComparison.Ordinal);
         }
         return count;
      }

      // ---- the COM members, bound late ----------------------------------------

      private static string ComChangedFromDefault(Settings settings) =>
         (string) ((dynamic) settings).GetChangedFromDefault();

      private static string ComSettingDefault(Settings settings, string name) =>
         (string) ((dynamic) settings).GetSettingDefault(name);

      // ---- one HTTP request ----------------------------------------------------

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
