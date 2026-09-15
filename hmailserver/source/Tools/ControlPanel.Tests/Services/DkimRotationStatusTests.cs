// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The DKIM tab's reading of the status document the server's key rotation
   /// answers (Domain.DKIMRotationStatus, GET /api/v1/domains/{domain}/dkim/rotation).
   ///
   /// The server decides every word and the Control Panel only says it, so what can
   /// go wrong here is saying it wrongly: a field read under the wrong name draws an
   /// empty slot, a word missing from the tables shows the administrator the server's
   /// own token in English, and a newer server's new word must not throw in an older
   /// Control Panel's dialog. The document below is the shape DkimRotation::StatusJson
   /// writes, field for field.
   /// </summary>
   [Collection("Loc")]
   public class DkimRotationStatusTests : IDisposable
   {
      private const string Document = @"{
  ""domain"": ""example.com"", ""now"": 1789000000,
  ""policy"": {""enabled"": true, ""interval_days"": 180, ""propagation_hours"": 72, ""retention_days"": 7, ""key_bits"": 2048},
  ""phase"": ""propagating"",
  ""current"": {""selector"": ""s20260101"", ""key_file"": ""C:\\Data\\DKIM\\s20260101._domainkey.example.com.pem"", ""generated"": true,
                ""since"": 1767225600, ""next_rotation_due"": 1782777600,
                ""records"": [{""host"": ""s20260101._domainkey.example.com"", ""type"": ""TXT"", ""value"": ""v=DKIM1; k=rsa; p=AAA"", ""action"": ""keep""}],
                ""check"": {""status"": ""matches"", ""checked"": 1789000000, ""detail"": """"}},
  ""next"": {""selector"": ""s20260915"", ""key_file"": ""C:\\Data\\DKIM\\s20260915._domainkey.example.com.pem"", ""generated"": true,
             ""created"": 1788990000, ""first_seen"": 1788995000, ""switch_due"": 1789254200,
             ""records"": [{""host"": ""s20260915._domainkey.example.com"", ""type"": ""TXT"", ""value"": ""v=DKIM1; k=rsa; p=BBB"", ""action"": ""publish""},
                          {""host"": ""s20260915._domainkey.alias.example"", ""type"": ""TXT"", ""value"": ""v=DKIM1; k=rsa; p=BBB"", ""action"": ""publish""}],
             ""check"": {""status"": ""not_found"", ""checked"": 1789000000, ""detail"": ""alias.example: no TXT record""}},
  ""retiring"": null,
  ""retired"": {""selector"": ""s20250601"", ""at"": 1760000000, ""key_file_kept"": ""D:\\keys\\old.pem"",
                ""records"": [{""host"": ""s20250601._domainkey.example.com"", ""type"": ""TXT"", ""action"": ""remove""}]},
  ""waiting"": [{""step"": ""switch"", ""reason"": ""propagation"", ""until"": 1789254200, ""detail"": ""The record is propagating.""}],
  ""last_error"": {""time"": 1788000000, ""text"": ""The key file could not be written.""}
}";

      public void Dispose() => Loc.Reset();

      [Fact]
      public void EveryFieldTheServerWritesIsRead()
      {
         DkimRotationStatus status = DkimRotationStatus.Parse(Document);

         Assert.NotNull(status);
         Assert.Equal("example.com", status.Domain);
         Assert.Equal("propagating", status.Phase);
         Assert.True(status.PolicyEnabled);
         Assert.Equal(180, status.IntervalDays);
         Assert.Equal(72, status.PropagationHours);
         Assert.Equal(7, status.RetentionDays);
         Assert.Equal(2048, status.KeyBits);

         Assert.Equal("s20260101", status.Current.Selector);
         Assert.True(status.Current.Generated);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1767225600), status.Current.Since);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1782777600), status.Current.NextRotationDue);
         Assert.Equal("matches", status.Current.Check.Status);
         Assert.Equal("keep", Assert.Single(status.Current.Records).Action);

         Assert.Equal("s20260915", status.Next.Selector);
         Assert.Equal(@"C:\Data\DKIM\s20260915._domainkey.example.com.pem", status.Next.KeyFile);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1788990000), status.Next.Created);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1788995000), status.Next.FirstSeen);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1789254200), status.Next.SwitchDue);
         Assert.Equal(2, status.Next.Records.Count);
         Assert.Equal("s20260915._domainkey.alias.example", status.Next.Records[1].Host);
         Assert.Equal("v=DKIM1; k=rsa; p=BBB", status.Next.Records[1].Value);
         Assert.Equal("not_found", status.Next.Check.Status);
         Assert.Equal("alias.example: no TXT record", status.Next.Check.Detail);

         Assert.Null(status.Retiring);

         Assert.Equal("s20250601", status.Retired.Selector);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1760000000), status.Retired.At);
         Assert.Equal(@"D:\keys\old.pem", status.Retired.KeyFileKept);
         DkimRotationStatus.Record removed = Assert.Single(status.Retired.Records);
         Assert.Equal("remove", removed.Action);
         Assert.Equal("TXT", removed.Type);
         Assert.Equal("", removed.Value);

         DkimRotationStatus.Wait wait = Assert.Single(status.Waiting);
         Assert.Equal("switch", wait.Step);
         Assert.Equal("propagation", wait.Reason);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1789254200), wait.Until);

         Assert.Equal("The key file could not be written.", status.LastError);
         Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1788000000), status.LastErrorTime);
      }

      [Fact]
      public void AZeroTimeIsNoTimeAndANullSlotIsNoSlot()
      {
         DkimRotationStatus status = DkimRotationStatus.Parse(
            @"{""domain"":""a.test"",""phase"":""idle"",""current"":{""selector"":""s1"",""since"":0,""records"":[]},""next"":null,""retiring"":null,""retired"":null,""waiting"":[{""step"":""start"",""reason"":""not_scheduled"",""until"":0}],""last_error"":null}");

         Assert.Null(status.Current.Since);
         Assert.Null(status.Current.Check);
         Assert.Null(status.Next);
         Assert.Null(status.Retired);
         Assert.Null(Assert.Single(status.Waiting).Until);
         Assert.Equal("", status.LastError);
         Assert.Null(status.LastErrorTime);
      }

      [Theory]
      [InlineData(null)]
      [InlineData("")]
      [InlineData("not json")]
      [InlineData("[1,2,3]")]
      [InlineData("{\"domain\":")]
      public void WhatIsNotADocumentIsNullAndNeverThrows(string json)
      {
         Assert.Null(DkimRotationStatus.Parse(json));
      }

      /// <summary>
      /// The words a newer server may add. The dialog draws them as they are rather
      /// than failing, so an older Control Panel still shows the rest of the status.
      /// </summary>
      [Fact]
      public void AnUnknownWordReadsAsItselfAtTheInformationLevel()
      {
         Assert.Equal("some_new_phase", DkimRotationStatus.PhaseText("some_new_phase"));
         Assert.Equal("some_new_reason", DkimRotationStatus.ReasonText("some_new_reason"));
         Assert.Equal("archive", DkimRotationStatus.ActionText("archive"));
         Assert.Equal((StatusLevel.Information, "stale"), DkimRotationStatus.CheckPresentation("stale"));
         Assert.Equal("", DkimRotationStatus.PhaseText(null));
         Assert.Equal("", DkimRotationStatus.ReasonText(null));
      }

      /// <summary>
      /// Every word the server writes has a sentence, and none of those sentences is
      /// the word itself - which is what a missing entry would show.
      /// </summary>
      [Fact]
      public void EveryWordTheServerWritesHasItsOwnText()
      {
         string[] phases = { "off", "idle", "awaiting_dns", "propagating", "ready", "waiting_for_retiring_slot" };
         string[] checks = { "unchecked", "matches", "not_found", "different", "lookup_failed", "key_unreadable" };
         string[] reasons = { "not_scheduled", "domain_inactive", "interval", "unchecked", "record_not_found", "record_differs",
                              "lookup_failed", "key_unreadable", "propagation", "retiring_slot", "retention" };
         string[] actions = { "publish", "keep", "remove" };

         Assert.Equal(phases.OrderBy(w => w), DkimRotationStatus.KnownPhases.OrderBy(w => w));
         Assert.Equal(checks.OrderBy(w => w), DkimRotationStatus.KnownChecks.OrderBy(w => w));
         Assert.Equal(reasons.OrderBy(w => w), DkimRotationStatus.KnownReasons.OrderBy(w => w));
         Assert.Equal(actions.OrderBy(w => w), DkimRotationStatus.KnownActions.OrderBy(w => w));

         foreach (string word in phases)
            Assert.NotEqual(word, DkimRotationStatus.PhaseText(word));
         foreach (string word in reasons)
            Assert.NotEqual(word, DkimRotationStatus.ReasonText(word));
         foreach (string word in actions)
            Assert.NotEqual(word, DkimRotationStatus.ActionText(word));
         foreach (string word in checks)
            Assert.NotEqual(word, DkimRotationStatus.CheckPresentation(word).Word);
      }

      /// <summary>
      /// A match is good and a record that carries another key is critical, because
      /// waiting does not fix that one; a missing record or a failed lookup is a
      /// warning, because the next pass may. The level is what colours the notice.
      /// </summary>
      [Fact]
      public void EachCheckCarriesTheLevelItDeserves()
      {
         Assert.Equal(StatusLevel.Good, DkimRotationStatus.CheckPresentation("matches").Level);
         Assert.Equal(StatusLevel.Information, DkimRotationStatus.CheckPresentation("unchecked").Level);
         Assert.Equal(StatusLevel.Warning, DkimRotationStatus.CheckPresentation("not_found").Level);
         Assert.Equal(StatusLevel.Warning, DkimRotationStatus.CheckPresentation("lookup_failed").Level);
         Assert.Equal(StatusLevel.Critical, DkimRotationStatus.CheckPresentation("different").Level);
         Assert.Equal(StatusLevel.Critical, DkimRotationStatus.CheckPresentation("key_unreadable").Level);
      }

      [Fact]
      public void TheWordsAreTranslatedThroughTheCatalogue()
      {
         var catalogue = new Dictionary<string, string>
         {
            ["Propagating"] = "Sprids",
            ["The record is propagating."] = "Posten sprids.",
            ["Matches"] = "Stämmer",
            ["Publish"] = "Publicera",
         };
         Loc.Use((text, culture) => catalogue.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("sv"));

         Assert.Equal("Sprids", DkimRotationStatus.PhaseText("propagating"));
         Assert.Equal("Posten sprids.", DkimRotationStatus.ReasonText("propagation"));
         Assert.Equal("Stämmer", DkimRotationStatus.CheckPresentation("matches").Word);
         Assert.Equal("Publicera", DkimRotationStatus.ActionText("publish"));
      }

      /// <summary>
      /// Every sentence the tables hold is in the English catalogue, so every language
      /// has an entry to translate - a text marked only with N() and never scanned
      /// would be English in all of them.
      /// </summary>
      [Fact]
      public void EveryTextIsInTheCatalogue()
      {
         string resources = ResourcesDirectory();
         if (resources == null)
            return;   // sources not available (a packaged drop)

         var english = new HashSet<string>(
            XDocument.Load(Path.Join(resources, "Strings.resx")).Root.Elements("data").Select(d => (string)d.Attribute("name")),
            StringComparer.Ordinal);

         // No catalogue is in use, so each text comes back as the English key itself.
         Loc.Reset();
         foreach (string word in DkimRotationStatus.KnownPhases)
            Assert.Contains(DkimRotationStatus.PhaseText(word), english);
         foreach (string word in DkimRotationStatus.KnownReasons)
            Assert.Contains(DkimRotationStatus.ReasonText(word), english);
         foreach (string word in DkimRotationStatus.KnownActions)
            Assert.Contains(DkimRotationStatus.ActionText(word), english);
         foreach (string word in DkimRotationStatus.KnownChecks)
            Assert.Contains(DkimRotationStatus.CheckPresentation(word).Word, english);
      }

      [Fact]
      public void ATimeIsShownInTheReadersZoneAndNoTimeAsADash()
      {
         DateTimeOffset time = DateTimeOffset.FromUnixTimeSeconds(1789000000);
         Assert.Equal(time.ToLocalTime().ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture), DkimRotationStatus.When(time));
         Assert.Equal("—", DkimRotationStatus.When(null));
      }

      private static string ResourcesDirectory()
      {
         string dir = AppContext.BaseDirectory;
         for (int i = 0; i < 8 && dir != null; i++)
         {
            string candidate = Path.Join(dir, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            dir = Path.GetDirectoryName(dir);
         }
         return null;
      }
   }
}
