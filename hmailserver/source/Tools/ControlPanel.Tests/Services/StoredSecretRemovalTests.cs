// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A secret stored on the feature settings pages - the shared HMAC secret, the
   /// introspection client secret, the SRS and BATV signing secrets, the metrics
   /// token, the password pepper - could not be removed: a blank box kept it, and
   /// nothing cleared it (review finding F239). While one is stored a box under the
   /// field removes it with the page's save; a new value typed with the box ticked is
   /// refused before anything is written, as the Directory authentication page refuses
   /// its service account password; and a pepper removed is asked about as a pepper
   /// changed. Scanned rather than rendered, as UnsavedChangesTests is: this assembly
   /// has no WPF.
   /// </summary>
   public class StoredSecretRemovalTests
   {
      [Theory]
      [InlineData("old pepper", 12, true)]    // stored, on a server with accounts
      [InlineData("old pepper", null, true)]  // accounts not counted: asked as if there were some
      [InlineData("old pepper", 0, false)]    // no accounts yet: nothing to break
      [InlineData("", 12, false)]             // nothing stored: nothing to remove
      [InlineData(null, 12, false)]
      public void RemovingThePepper_AsksFirst_WhenAStoredPepperMeetsAccounts(string stored, int? accounts, bool asks)
         => Assert.Equal(asks, PepperChange.RemovalAsksFirst(stored, accounts));

      [Fact]
      public void AStoredSecret_HasABoxThatRemovesIt_ShownOnlyWhileOneIsStored()
      {
         string page = SaveStateModelTests.ReadControlPanelFile("Views/FeatureSettingsView.xaml.cs");
         string secret = SaveStateModelTests.ClassBody_(page, "private class SecretSetting : Setting");

         Assert.Contains("Content = L(\"Remove the stored secret when saving\")", secret, StringComparison.Ordinal);
         Assert.Contains("remove_.Visibility = stored ? Visibility.Visible : Visibility.Collapsed;", secret, StringComparison.Ordinal);

         // The save writes the empty value that is no secret, and the field then says
         // that none is stored.
         string save = WithheldRowTests.MemberText(secret, "public override void Save(IniFeatureStore store)");
         Assert.True(save != null, "SecretSetting.Save is gone.");
         int removes = save.IndexOf("if (RemovesStored)", StringComparison.Ordinal);
         Assert.True(removes >= 0 && save.IndexOf("store.Write(Key, \"\");", removes, StringComparison.Ordinal) > removes,
            "Ticking the box does not remove the stored secret.");
         Assert.Contains("ShowStored_(false)", save, StringComparison.Ordinal);

         Assert.True(save.IndexOf("box_.Password = \"\";", removes, StringComparison.Ordinal) > removes,
            "A removed secret's box keeps the value it held.");

         // The value a save just wrote stays in the box to be copied - a generated
         // metrics token, say - and is neither written again nor taken, beside a
         // ticked removal box, for a new value typed: only a value typed since the
         // last read or save is.
         Assert.Contains("public bool Conflicts => RemovesStored && base.IsChanged && !string.IsNullOrEmpty(box_?.Password);", secret, StringComparison.Ordinal);
         Assert.Contains("if (!string.IsNullOrEmpty(entered) && base.IsChanged)", save, StringComparison.Ordinal);

         // Ticking the box is an edit: the pill says Unsaved changes, and leaving the
         // page asks; the computed warnings that ask whether a secret is set follow it.
         Assert.Contains("public override bool IsChanged => base.IsChanged || RemovesStored;", secret, StringComparison.Ordinal);
         Assert.Contains("remove_.Checked += (s, e) => handler();", secret, StringComparison.Ordinal);
         Assert.Contains("if (box_ != null && RemovesStored && !Conflicts)", secret, StringComparison.Ordinal);
      }

      [Fact]
      public void ANewValueAndTheRemovalBoth_AreRefusedBeforeAnythingIsWritten()
      {
         string page = SaveStateModelTests.ReadControlPanelFile("Views/FeatureSettingsView.xaml.cs");
         string save = WithheldRowTests.MemberText(page, "private bool Save_()");
         Assert.True(save != null, "FeatureSettingsView.Save_ is gone.");

         int refuses = save.IndexOf("secret.Conflicts", StringComparison.Ordinal);
         int asks = save.IndexOf("ConfirmPepperChange_()", StringComparison.Ordinal);
         int writes = save.IndexOf("setting.Save(store_)", StringComparison.Ordinal);
         Assert.True(refuses >= 0 && asks > refuses && writes > refuses,
            "A secret given a new value and ticked for removal is written, or asked about, before the save refuses it.");
         // Withdrawn once the secret it names no longer conflicts - the value cleared or the
         // box unticked - though another edit is still unsaved.
         Assert.Contains("saveState_.Refuse(SecretSetting.ConflictSentence(conflict.Label), () => conflict.Conflicts)", save, StringComparison.Ordinal);

         string confirm = WithheldRowTests.MemberText(page, "private bool ConfirmPepperChange_()");
         Assert.True(confirm != null && confirm.Contains("PepperChange.RemovalAsksFirst(", StringComparison.Ordinal),
            "Removing the password pepper is not asked about.");
      }

      private const string LdapRefusal = "A new service account password has been typed, and \"Remove the stored service account password when saving\" is ticked under it. Clear the password or untick the box - guessing which was meant is not this page's decision to make.";
      private const string LdapBox = "Remove the stored service account pass_word when saving";
      private const string SecretRefusal = "A new value has been typed for \"{0}\", and \"Remove the stored secret when saving\" is ticked under it. Clear the value or untick the box - guessing which was meant is not this page's decision to make.";
      private const string SecretBox = "Remove the stored secret when saving";

      /// <summary>
      /// The two refusals of the one conflict - a new value typed with the removal of
      /// the stored one ticked - name the box by its caption, in English and in every
      /// catalogue, so that the reader can find what to untick. Directory
      /// authentication's quoted "remove the stored password", which is on no box, said
      /// AND in capitals and asked for one of a password and a box to be unticked,
      /// where the Authentication page refused the same conflict in whole words.
      /// </summary>
      [Theory]
      [InlineData("Views/LdapSettingsView.cs", LdapRefusal, LdapBox)]
      [InlineData("Views/FeatureSettingsView.xaml.cs", SecretRefusal, SecretBox)]
      public void TheConflictsRefusal_NamesTheBoxByItsCaption_InEveryLanguage(string file, string refusal, string box)
      {
         string code = SaveStateModelTests.ReadControlPanelFile(file);
         Assert.Contains("L(\"" + box.Replace("\"", "\\\"", StringComparison.Ordinal) + "\")", code, StringComparison.Ordinal);
         Assert.Contains(refusal.Replace("\"", "\\\"", StringComparison.Ordinal), code, StringComparison.Ordinal);
         Assert.Contains("\"" + MnemonicText.Strip(box) + "\"", refusal, StringComparison.Ordinal);
         Assert.DoesNotContain(" AND ", refusal, StringComparison.Ordinal);

         foreach (string path in Directory.GetFiles(SaveStateModelTests.ControlPanelDirectory() + Path.DirectorySeparatorChar + "Resources", "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx_(path);
            Assert.True(catalogue.TryGetValue(refusal, out string said) && said.Length > 0, Path.GetFileName(path) + " does not translate the refusal.");
            Assert.True(catalogue.TryGetValue(box, out string caption) && caption.Length > 0, Path.GetFileName(path) + " does not translate the box.");

            // The caption as the box draws it: its access key out, and a Chinese or
            // Japanese caption's "(_W)" with it.
            string drawn = Regex.Replace(MnemonicText.Strip(caption), @"\s*\([A-Z]\)$", "");
            Assert.True(said.Contains(drawn, StringComparison.Ordinal),
               Path.GetFileName(path) + ": the refusal does not name the box \"" + drawn + "\": " + said);
         }
      }

      private static Dictionary<string, string> ReadResx_(string path)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }
   }
}
