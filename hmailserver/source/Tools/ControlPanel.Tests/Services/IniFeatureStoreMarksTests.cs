// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What the Control Panel asks the server about when a save takes effect
   /// (docs/SettingsApply.md), and how often. The review of 22 September 2026 found
   /// a page's first open making a COM call per field for the marks, a failed call
   /// remembered as "no mark" for the whole session, the pending list read once per
   /// field of a save, and an older server - which applies every key at its next
   /// start - told that its settings were in effect. These hold each of those.
   ///
   /// IniFeatureStore keeps its injected calls and what they found in static state,
   /// so every test here sets them and puts them back, and the class runs in a
   /// collection of its own rather than beside another test that touches them.
   /// </summary>
   [Collection("IniFeatureStore")]
   public class IniFeatureStoreMarksTests : IDisposable
   {
      private const int UnknownName = unchecked((int)0x80020006);   // DISP_E_UNKNOWNNAME
      private const int RpcUnavailable = unchecked((int)0x800706BA); // RPC_S_SERVER_UNAVAILABLE

      public IniFeatureStoreMarksTests()
      {
         IniFeatureStore.ComAllApplies = null;
         IniFeatureStore.ComPendingSettings = null;
         IniFeatureStore.ComWriteSetting = null;
         IniFeatureStore.ForgetApplies();
      }

      public void Dispose()
      {
         IniFeatureStore.ComAllApplies = null;
         IniFeatureStore.ComPendingSettings = null;
         IniFeatureStore.ComWriteSetting = null;
         IniFeatureStore.ForgetApplies();
      }

      [Fact]
      public void TheMarksComeInOneCallAndAreThenReadWithoutAsking()
      {
         int calls = 0;
         IniFeatureStore.ComAllApplies = () =>
         {
            calls++;
            return "{\"RestApiPort\":\"reinitialize\",\"LogLevel\":\"now\",\"ServiceAccountName\":\"restart\"}";
         };

         IniFeatureStore.LoadMarks();

         Assert.Equal(SettingApplies.Reinitialize, IniFeatureStore.AppliesFor("RestApiPort"));
         Assert.Equal(SettingApplies.Now, IniFeatureStore.AppliesFor("loglevel"));
         Assert.Equal(SettingApplies.Restart, IniFeatureStore.AppliesFor("ServiceAccountName"));
         Assert.Null(IniFeatureStore.AppliesFor("SomethingTheLoaderDoesNotRead"));
         Assert.True(IniFeatureStore.ServerMarksSettings);

         // Four answers, one call: AppliesFor never goes to the server. Before, each
         // field asked for its own mark.
         Assert.Equal(1, calls);
      }

      [Fact]
      public void AFailedReadIsNotRememberedAndTheNextPageAsksAgain()
      {
         bool fail = true;
         IniFeatureStore.ComAllApplies = () =>
         {
            if (fail)
               throw new COMException("The RPC server is unavailable.", RpcUnavailable);
            return "{\"RestApiPort\":\"reinitialize\"}";
         };

         IniFeatureStore.LoadMarks();

         // A moment's failure: nothing known, and not "this server has no marks".
         Assert.Null(IniFeatureStore.AppliesFor("RestApiPort"));
         Assert.Null(IniFeatureStore.ServerMarksSettings);

         // The next page open asks again, and is answered. Before, the failure was
         // cached as "no mark" for the rest of the session.
         fail = false;
         IniFeatureStore.LoadMarks();
         Assert.Equal(SettingApplies.Reinitialize, IniFeatureStore.AppliesFor("RestApiPort"));

         // And a later failure does not take away what a read found: a mark is the
         // build's, not the moment's.
         fail = true;
         IniFeatureStore.LoadMarks();
         Assert.Equal(SettingApplies.Reinitialize, IniFeatureStore.AppliesFor("RestApiPort"));
      }

      [Fact]
      public void AnOlderServerIsToldApartAndEveryKeyAppliesAtItsRestart()
      {
         IniFeatureStore.ComAllApplies = () => throw new COMException("Unknown name.", UnknownName);
         IniFeatureStore.ComPendingSettings = () => throw new COMException("Unknown name.", UnknownName);

         IniFeatureStore.LoadMarks();

         // A server from before a save could publish: every [Settings] key applies
         // at its next start, so the page marks each one so - never "Applies on save".
         Assert.False(IniFeatureStore.ServerMarksSettings);
         Assert.Equal(SettingApplies.Restart, IniFeatureStore.AppliesFor("LogLevel"));

         // And "cannot say" is not "nothing waits": null, which a page answers with
         // the sentences it used before, not with "every setting is in effect".
         Assert.Null(IniFeatureStore.PendingNames());
      }

      [Fact]
      public void AServerThatSaysNothingWaitsIsNotAnOlderOne()
      {
         // The negative control for the test above: an empty list is an answer.
         IniFeatureStore.ComPendingSettings = () => "";

         IReadOnlyList<string> pending = IniFeatureStore.PendingNames();

         Assert.NotNull(pending);
         Assert.Empty(pending);
         Assert.NotEqual(false, IniFeatureStore.ServerMarksSettings);
      }

      [Fact]
      public void ASaveReadsThePendingListOnceHoweverManyFieldsItWrote()
      {
         int reads = 0;
         IniFeatureStore.ComPendingSettings = () =>
         {
            reads++;
            return "RestApiPort";
         };
         var written = new List<string>();
         IniFeatureStore.ComWriteSetting = (key, value) => written.Add(key);

         var store = new IniFeatureStore();
         store.Write("LogLevel", "3");
         store.Write("RestApiPort", "8080");
         store.Write("MaxLogLineLen", "600");

         // The page's own read after its save, and the window's refresh straight
         // after it: one call between them.
         IReadOnlyList<string> pageRead = IniFeatureStore.PendingNames();
         IReadOnlyList<string> barRead = IniFeatureStore.PendingNamesSinceLastWrite();

         Assert.Equal(new[] { "RestApiPort" }, pageRead);
         Assert.Same(pageRead, barRead);
         Assert.Equal(1, reads);
         Assert.Equal(3, written.Count);

         // A write after the read makes the next refresh ask again - the negative
         // control: the reuse is of a read that is still current, not of any read.
         store.Write("LogLevel", "4");
         IniFeatureStore.PendingNamesSinceLastWrite();
         Assert.Equal(2, reads);
      }
   }
}
