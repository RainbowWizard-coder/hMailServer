// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text.Json;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The settings the server's environment sets (docs/EnvironmentConfiguration.md),
   /// as the Control Panel reads them from Settings.GetChangedFromDefault and shows
   /// them: the parse of the document's environment array, the marks, the page's
   /// sentence, and which field a managed setting is - by the key for a field kept in
   /// the settings store, by the property's own name for a COM field, and never a
   /// field that merely resembles one.
   /// </summary>
   [Collection("IniFeatureStore")]
   public class EnvironmentSettingsTests : IDisposable
   {
      private const string Document =
         "{\"changed\":1,\"considered\":400,\"settings\":[{\"store\":\"database\",\"name\":\"maxmessagesize\",\"value\":\"1234\",\"default\":\"20480\"," +
         "\"secret\":false,\"known\":true,\"overridden\":false,\"environment\":\"variable\"}]," +
         "\"environment\":[{\"variable\":\"HM_PROPERTY_maxmessagesize\",\"store\":\"database\",\"name\":\"maxmessagesize\",\"source\":\"variable\"}," +
         "{\"variable\":\"HM_SETTING_RestApiPort_FILE\",\"store\":\"ini\",\"name\":\"RestApiPort\",\"source\":\"file\"}]}";

      public EnvironmentSettingsTests()
      {
         IniFeatureStore.ComChangedDocument = null;
         IniFeatureStore.ForgetApplies();
      }

      public void Dispose()
      {
         IniFeatureStore.ComChangedDocument = null;
         IniFeatureStore.ForgetApplies();
      }

      [Fact]
      public void TheDocumentsEnvironmentArrayIsReadWithEverySource()
      {
         IReadOnlyList<EnvironmentSettings.Entry> entries = EnvironmentSettings.Parse(Document);

         Assert.Equal(2, entries.Count);
         Assert.Equal("HM_PROPERTY_maxmessagesize", entries[0].VariableName);
         Assert.Equal(EnvironmentSettings.StoreDatabase, entries[0].Store);
         Assert.Equal(EnvironmentSettings.Variable, entries[0].Source);
         Assert.Equal("RestApiPort", entries[1].Name);
         Assert.Equal(EnvironmentSettings.File, entries[1].Source);
      }

      [Fact]
      public void AServerOlderThanTheArrayManagesNothing()
      {
         Assert.Empty(EnvironmentSettings.Parse("{\"changed\":0,\"considered\":391,\"settings\":[]}"));
         Assert.Empty(EnvironmentSettings.Parse(""));
         Assert.Empty(EnvironmentSettings.Parse(null));
         Assert.ThrowsAny<JsonException>(() => EnvironmentSettings.Parse("[1,2]"));
      }

      [Fact]
      public void AnEntryWithoutAStoreOrANameIsNotAField()
      {
         IReadOnlyList<EnvironmentSettings.Entry> entries = EnvironmentSettings.Parse(
            "{\"environment\":[{\"variable\":\"HM_X\",\"store\":\"elsewhere\",\"name\":\"x\",\"source\":\"variable\"},{\"variable\":\"HM_Y\",\"store\":\"ini\",\"source\":\"variable\"}]}");

         Assert.Empty(entries);
      }

      [Fact]
      public void TheMarksSayWhereTheValueCameFromAndNothingForAnythingElse()
      {
         Assert.Equal("Set by the environment", EnvironmentSettings.Caption(EnvironmentSettings.Variable));
         Assert.Equal("Set by the environment, from a file", EnvironmentSettings.Caption(EnvironmentSettings.File));
         Assert.Null(EnvironmentSettings.Caption(null));
         Assert.Null(EnvironmentSettings.Caption("somewhere"));
      }

      [Fact]
      public void TheSentenceNamesEveryManagedSettingByItsVariable()
      {
         string sentence = EnvironmentSettings.Notice(EnvironmentSettings.Parse(Document));

         Assert.Contains("HM_PROPERTY_maxmessagesize", sentence);
         Assert.Contains("HM_SETTING_RestApiPort_FILE", sentence);
         Assert.DoesNotContain("1234", sentence);
         Assert.Null(EnvironmentSettings.Notice(Array.Empty<EnvironmentSettings.Entry>()));
      }

      [Fact]
      public void AFieldIsTheSettingByItsKeyOrItsPropertysOwnName()
      {
         IniFeatureStore.ComChangedDocument = () => Document;
         IniFeatureStore.LoadEnvironment();

         Assert.Equal(EnvironmentSettings.File, IniFeatureStore.EnvironmentForKey("RestApiPort"));
         Assert.Equal(EnvironmentSettings.File, IniFeatureStore.EnvironmentForKey("restapiport"));
         Assert.Equal(EnvironmentSettings.Variable, IniFeatureStore.EnvironmentForComPath("MaxMessageSize"));
         Assert.Null(IniFeatureStore.EnvironmentForComPath("AntiSpam.MaximumMessageSize"));
         Assert.Null(IniFeatureStore.EnvironmentForKey("maxmessagesize"));
         Assert.Null(IniFeatureStore.EnvironmentForComPath("RestApiPort"));
      }

      [Fact]
      public void AReadThatFailsMarksNothingAndIsAskedAgain()
      {
         IniFeatureStore.ComChangedDocument = () => Document;
         IniFeatureStore.LoadEnvironment();
         Assert.NotEmpty(IniFeatureStore.EnvironmentManaged());

         IniFeatureStore.ComChangedDocument = () => throw new COMException("The document is the server administrator's.");
         IniFeatureStore.LoadEnvironment();
         Assert.Empty(IniFeatureStore.EnvironmentManaged());
         Assert.Null(IniFeatureStore.EnvironmentForKey("RestApiPort"));

         IniFeatureStore.ComChangedDocument = () => Document;
         IniFeatureStore.LoadEnvironment();
         Assert.Equal(2, IniFeatureStore.EnvironmentManaged().Count);
      }

      [Fact]
      public void AnotherServerForgetsWhatTheLastOneManaged()
      {
         IniFeatureStore.ComChangedDocument = () => Document;
         IniFeatureStore.LoadEnvironment();
         IniFeatureStore.ForgetApplies();

         Assert.Empty(IniFeatureStore.EnvironmentManaged());
      }
   }
}
