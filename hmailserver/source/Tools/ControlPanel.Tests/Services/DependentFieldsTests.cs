// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Fields that count only while their switch is on stayed editable while it was off -
   /// a relay's credential, a domain's account limit, an account's forward address, a
   /// route's credential, an IP range's expiry, the ACME and OAuth2 settings, the
   /// anti-virus notices (review findings F197 and F106). These hold each pair to
   /// DialogFields.EnabledBy, the fields the server reads for something else to staying
   /// live, and the IP range's expiry to a date and a time of day.
   /// </summary>
   public class DependentFieldsTests
   {
      [Theory]
      // Domain properties
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(maxAccountsOn_, maxAccountsRow_);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(maxAliasesOn_, maxAliasesRow_);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(maxDistsOn_, maxDistsRow_);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(plusAddressingOn_, plusCharRow);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(signatureOn_, methodRow, plainRow, htmlRow);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(relayAuthOn_, userRow, passwordRow);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(oooOn_, internalSubjectRow, internalMessageRow, daysRow);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(tagSubject_, tagTextRow_);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(disclaimerOn_, disclaimerPlainRow, disclaimerHtmlRow);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(rotOn_, rotIntervalRow_);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(digestOn_, digestHourRow_);")]
      [InlineData("DomainDialog.cs", "DialogFields.EnabledBy(webhooksOn_, webhookSubjects_);")]
      // Account properties
      [InlineData("AccountDialog.cs", "DialogFields.EnabledBy(forwardOn_, toRow, forwardKeep_, forwardAbortSpam_);")]
      [InlineData("AccountDialog.cs", "DialogFields.EnabledBy(signatureOn_, plainRow, htmlRow);")]
      [InlineData("AccountDialog.cs", "DialogFields.EnabledBy(isAd_, domainRow, userRow, browse);")]
      [InlineData("AccountDialog.cs", "DialogFields.EnabledWhenAll(new System.Windows.Controls.Primitives.ToggleButton[] { vacationOn_, vacationExpires_ }, expiryRow);")]
      // Route and IP range
      [InlineData("RouteDialog.cs", "DialogFields.EnabledBy(requiresAuth_, userRow, passwordRow);")]
      [InlineData("IPRangeDialog.cs", "DialogFields.EnabledBy(expires_, expiresRow_);")]
      public void AFieldThatCountsOnlyWhileItsSwitchIsOn_FollowsTheSwitch(string view, string wiring)
         => Assert.Contains(wiring, View_(view), StringComparison.Ordinal);

      [Fact]
      public void TheAutoReply_FollowsItsSwitch_Whole()
      {
         string reply = WithheldRowTests.MemberText(View_("AccountDialog.cs"), "private ScrollViewer BuildAutoReply()");
         Assert.True(reply != null, "AccountDialog.BuildAutoReply is gone.");
         Assert.Contains("DialogFields.EnabledBy(vacationOn_, subjectRow, messageRow, beginRow, vacationExpires_, vacationAbortSpam_,", reply, StringComparison.Ordinal);
         Assert.Contains("daysRow, internalSubjectRow, internalMessageRow, vacationInternalOnly_);", reply, StringComparison.Ordinal);
      }

      /// <summary>
      /// What the server reads with the switch off stays live: a domain's reply subject
      /// and text, which answer outside senders in an account's place under the override
      /// (LocalDelivery); the two signature boxes that hold back every signature
      /// (SignatureAdder); a rotation's period, retention and key size, which Rotate now
      /// uses (DkimRotation); DKIM itself, whose key is made and published before it is
      /// switched on.
      /// </summary>
      [Fact]
      public void WhatTheServerReadsWithTheSwitchOff_StaysLive()
      {
         string domain = View_("DomainDialog.cs");
         foreach (Match wiring in Regex.Matches(domain, @"DialogFields\.EnabledBy\(([^;]*)\);"))
         {
            string args = wiring.Groups[1].Value;
            foreach (string kept in new[] { "oooSubject", "oooMessage", "oooExternalOverride_", "signReplies_", "signLocal_",
                                            "rotPropagationRow_", "rotRetentionRow_", "rotKeyBits_", "dkimOn_" })
               Assert.DoesNotContain(kept, args, StringComparison.Ordinal);
         }
      }

      [Fact]
      public void ANumberBehindASwitchThatIsOff_IsNotHeldAgainstTheSave()
      {
         string domain = View_("DomainDialog.cs");
         foreach (string guard in new[] { "&& maxAccountsOn_.IsChecked is true)", "&& maxAliasesOn_.IsChecked is true)", "&& maxDistsOn_.IsChecked is true)",
                                          "&& digestOn_.IsChecked is true)", "&& rotOn_.IsChecked is true)" })
            Assert.Contains(guard, domain, StringComparison.Ordinal);
      }

      /// <summary>
      /// The feature settings pages follow the same rule (found re-checking 827bf5403):
      /// Setting.Validate held every edited row to its range whatever its switch said, so
      /// that the ACME port edited to 0 with ACME on, and ACME then switched off, refused
      /// the save with "Nothing was saved: correct what is marked below" under a dimmed
      /// box the keyboard cannot enter. A row behind a switch that is off is let through,
      /// and the save writes nothing of a value the server would not take, putting the
      /// box back to what the file holds; a message left on it goes with the switch.
      /// </summary>
      [Fact]
      public void AValueBehindAFeaturePagesSwitchThatIsOff_IsNotHeldAgainstTheSave_AndIsKeptAsStored()
      {
         string feature = View_("FeatureSettingsView.xaml.cs");
         string validate = WithheldRowTests.MemberText(feature, "public bool Validate()");
         Assert.True(validate != null, "FeatureSettingsView's Setting.Validate is gone.");
         Assert.Contains("string error = Refusal;", validate, StringComparison.Ordinal);
         Assert.Contains("public string Refusal => IsChanged && !SwitchedOff ? Check() : null;", feature, StringComparison.Ordinal);
         Assert.Contains("public bool SwitchedOff => Master?.Box != null && Master.Box.IsChecked != true;", feature, StringComparison.Ordinal);
         Assert.Contains("protected bool KeptAsStored => SwitchedOff && IsChanged && Check() != null;", feature, StringComparison.Ordinal);

         // The switch is the row's master once the page has found it, and its going off takes a message away.
         Assert.Contains("setting.Master = master;", feature, StringComparison.Ordinal);
         Assert.Contains("master.Box.Unchecked += (s, e) => { if (setting.Field != null) setting.Field.Error = null; };", feature, StringComparison.Ordinal);

         // Every setting that can refuse a value keeps the stored one behind an off switch, before it writes.
         foreach (string kind in new[] { "NumberSetting", "TextSetting" })
         {
            string save = WithheldRowTests.MemberText(Class_(feature, kind), "public override void Save(IniFeatureStore store)");
            Assert.True(save != null, kind + ".Save is gone.");
            int kept = save.IndexOf("if (KeptAsStored)", StringComparison.Ordinal);
            int put = save.IndexOf("box_.Text = Loaded ?? \"\";", StringComparison.Ordinal);
            int writes = save.IndexOf("WriteIfChanged(store,", StringComparison.Ordinal);
            Assert.True(kept > 0 && put > kept && writes > put, kind + ".Save no longer keeps a refused value behind an off switch as stored before it writes.");
         }

         // And the save still asks every row before it writes any.
         string pageSave = WithheldRowTests.MemberText(feature, "private bool Save_()");
         int asks = pageSave.IndexOf(".Where(setting => !setting.Validate())", StringComparison.Ordinal);
         int writesAll = pageSave.IndexOf("setting.Save(store_);", StringComparison.Ordinal);
         Assert.True(asks > 0 && writesAll > asks, "FeatureSettingsView.Save_ no longer asks every row before it writes.");
      }

      /// <summary>
      /// A row dimmed by its switch reads as dimmed: the field row's template gives its
      /// caption, its Applies line and its hint the disabled text colour while the row is
      /// disabled, where they stayed at full contrast over a greyed box - the ACME
      /// settings under their switch, an account's expiry date under an auto-reply that
      /// does not stop (found re-checking 827bf5403). Read from the template, so that a
      /// rewrite of it keeps the trigger.
      /// </summary>
      [Fact]
      public void ADisabledRow_DimsItsCaptionAppliesAndHint()
      {
         XNamespace wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
         XDocument scaffold = XDocument.Load(ControlPanelFile_(Path.Join("Views", "Scaffold", "Scaffold.xaml")));
         XElement style = scaffold.Descendants(wpf + "Style").SingleOrDefault(s => (string)s.Attribute("TargetType") == "{x:Type scaffold:FieldRow}");
         Assert.True(style != null, "Scaffold.xaml has no FieldRow style.");
         XElement disabled = style.Descendants(wpf + "ControlTemplate.Triggers").Elements(wpf + "Trigger")
            .SingleOrDefault(trigger => (string)trigger.Attribute("Property") == "IsEnabled" && (string)trigger.Attribute("Value") == "False");
         Assert.True(disabled != null, "The FieldRow template no longer dims a disabled row.");
         foreach (string part in new[] { "PART_Label", "PART_Applies", "PART_Hint" })
         {
            Assert.True(style.Descendants().Any(e => (string)e.Attribute(XName.Get("Name", "http://schemas.microsoft.com/winfx/2006/xaml")) == part),
               "The FieldRow template has no " + part + ".");
            Assert.Contains(disabled.Elements(wpf + "Setter"), setter =>
               (string)setter.Attribute("TargetName") == part
               && (string)setter.Attribute("Property") == "Foreground"
               && (string)setter.Attribute("Value") == "{DynamicResource TextFillColorDisabledBrush}");
         }
      }

      [Fact]
      public void TheRowsFollow_NotTheirEditors_AndEveryTickAndFillIsFollowed()
      {
         string fields = View_("DialogFields.cs");
         string all = WithheldRowTests.MemberText(fields, "public static void EnabledWhenAll(System.Windows.Controls.Primitives.ToggleButton[] toggles, params UIElement[] dependents)");
         Assert.True(all != null, "DialogFields.EnabledWhenAll is gone.");
         foreach (string step in new[] { "toggle.IsChecked == true", "toggle.Checked +=", "toggle.Unchecked +=", "toggle.Indeterminate +=", "Apply();" })
            Assert.Contains(step, all, StringComparison.Ordinal);
      }

      [Fact]
      public void TheFeaturePages_FollowTheirMasterSwitches()
      {
         string feature = View_("FeatureSettingsView.xaml.cs");
         Assert.Contains("DialogFields.EnabledBy(master.Box, setting.Built);", feature, StringComparison.Ordinal);
         Assert.Contains("fields.Children.Add(setting.Built = setting.CreateEditor(store_));", feature, StringComparison.Ordinal);

         foreach (string key in new[] { "AcmeContactEmail", "AcmeDomains", "AcmeDirectoryUrl", "AcmeHttpPort", "AcmeReuseKey" })
            Assert.Contains("Key = \"" + key + "\", DependsOn = AcmeSwitch", feature, StringComparison.Ordinal);

         // The certificate folder is read by the REST API's and the web services' fallback
         // to the ACME certificate, the setup check and the service account's grants.
         Assert.DoesNotContain("Key = \"AcmeCertificateDirectory\", DependsOn", feature, StringComparison.Ordinal);

         int oauth = Regex.Matches(feature, "DependsOn = OAuth2Switch").Count;
         Assert.Equal(14, oauth);
      }

      [Fact]
      public void TheAntivirusNotices_FollowTheAction_TheyAreSentOnlyWhenTheMessageIsDeleted()
      {
         string settings = View_("ServerSettingsView.xaml.cs");
         Assert.Contains("afterBuildUi_ = () => WireAntivirusNoticeDependency(action, notifySender, notifyReceiver);", settings, StringComparison.Ordinal);
         string wire = WithheldRowTests.MemberText(settings, "private static void WireAntivirusNoticeDependency(ComCombo action, ComBool notifySender, ComBool notifyReceiver)");
         Assert.True(wire != null, "ServerSettingsView.WireAntivirusNoticeDependency is gone.");
         Assert.Contains("bool sent = action.SelectedValue == 0;", wire, StringComparison.Ordinal);
         Assert.Contains("row.IsEnabled = sent;", wire, StringComparison.Ordinal);
         Assert.Contains("row.Hint = why;", wire, StringComparison.Ordinal);

         // Removing the attachments tells the receiver whatever the boxes say: by the
         // note, or by the "message deleted" notice for a message the stripper cannot
         // read, which SMTPDeliverer deletes. The hint says both, and the note under
         // the action says what becomes of a message with nothing to strip.
         Assert.Contains("Sent only when the action is to delete the whole message.", wire, StringComparison.Ordinal);
         Assert.Contains("when the server cannot open or parse a message and deletes it", wire, StringComparison.Ordinal);
         Assert.Contains("a message that is one attachment becomes the note alone. A message that is only text, or encrypted, is delivered unchanged; one the server cannot open or parse is deleted, and its recipients are told.", settings, StringComparison.Ordinal);
      }

      [Fact]
      public void TheIPRangesExpiry_IsADateAndATimeOfDay_HeldOnTheirFields()
      {
         string range = View_("IPRangeDialog.cs");
         Assert.Contains("private readonly DatePicker expiresDate_", range, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Expiry _time (YYYY-MM-DD HH:MM:SS)\")", range, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Enter the expiry as YYYY-MM-DD HH:MM:SS.\")", range, StringComparison.Ordinal);
         string save = WithheldRowTests.MemberText(range, "private void Save()");
         Assert.Contains("DialogFields.ShowError(expiresRow_, L(\"Choose the day the range expires.\"));", save, StringComparison.Ordinal);
         Assert.Contains("TimeOfDayText.TryParse(expiresTime_.Text, out TimeSpan time)", save, StringComparison.Ordinal);
         Assert.Contains("r.ExpiresTime = expiry.Value;", save, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("", 0, 0, 0)]
      [InlineData("18:30", 18, 30, 0)]
      [InlineData("7:05", 7, 5, 0)]
      [InlineData("23:59:59", 23, 59, 59)]
      [InlineData(" 00:00 ", 0, 0, 0)]
      public void ATimeOfDay_IsReadAsThe24HourClockWritesIt(string text, int hours, int minutes, int seconds)
      {
         Assert.True(TimeOfDayText.TryParse(text, out TimeSpan time));
         Assert.Equal(new TimeSpan(hours, minutes, seconds), time);
      }

      [Theory]
      [InlineData("24:00")]
      [InlineData("18.30")]
      [InlineData("6 pm")]
      [InlineData("18:60")]
      [InlineData("1830")]
      public void AnythingElse_IsNotATimeOfDay(string text)
         => Assert.False(TimeOfDayText.TryParse(text, out _));

      [Fact]
      public void ATime_IsShownWithItsSecondsOnlyWhereThereAreSome()
      {
         Assert.Equal("18:30", TimeOfDayText.Format(new DateTime(2026, 10, 8, 18, 30, 0)));
         Assert.Equal("04:13:09", TimeOfDayText.Format(new DateTime(2026, 10, 8, 4, 13, 9)));
      }

      /// <summary>The text of one of FeatureSettingsView's setting classes, to the next class.</summary>
      private static string Class_(string source, string name)
      {
         int start = source.IndexOf("private class " + name + " : Setting", StringComparison.Ordinal);
         Assert.True(start >= 0, "FeatureSettingsView." + name + " is gone.");
         int end = source.IndexOf("private class ", start + 1, StringComparison.Ordinal);
         return end > start ? source[start..end] : source[start..];
      }

      private static string ControlPanelFile_(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
         }

         throw new FileNotFoundException("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
      }

      private static string View_(string name)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", name);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("ControlPanel/Views/" + name + " could not be found from the test binaries.");
      }
   }
}
