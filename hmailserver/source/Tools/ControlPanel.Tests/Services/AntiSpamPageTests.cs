// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F263: the Anti-spam settings page called the threshold that
   /// refuses or quarantines a message the delete threshold; a refuse threshold at or
   /// below the mark threshold, or both at 0, saved with "Saved 51 settings - applied
   /// immediately"; the Quarantine card, which changes what that threshold does, was
   /// fourth on General and below the fold; the rspamd hook sat on a tab called
   /// SpamAssassin; the Spam traps tab said traps are declared in the Control Deck
   /// with no count of the domains that have one; and the sentence after a save
   /// counted every setting, not the one changed. The page's wiring is read from its
   /// source; the sentence after a save is ServerSettingsPageStatus's.
   /// </summary>
   public class AntiSpamPageTests
   {
      [Fact]
      public void TheSentenceAfterASave_NamesWhatChanged()
      {
         Assert.Equal("Saved Spam refuse threshold (score) at 14:20:00 - applied immediately.",
            ServerSettingsPageStatus.SavedChangesSentence(new[] { "Spam refuse threshold (score)" }, 0, "14:20:00", 0));
         Assert.Equal("Saved 3 changes at 14:20:00 - applied immediately.",
            ServerSettingsPageStatus.SavedChangesSentence(new[] { "a", "b", "c" }, 0, "14:20:00", 0));
         Assert.Equal("Saved at 14:20:00: no setting had changed.",
            ServerSettingsPageStatus.SavedChangesSentence(Array.Empty<string>(), 0, "14:20:00", 0));

         // What waits is said of the page's settings, which may be more than this save changed.
         string waiting = ServerSettingsPageStatus.SavedChangesSentence(new[] { "SMTP port" }, 0, "14:20:00", 2);
         Assert.Equal("Saved SMTP port at 14:20:00. 2 settings on this page wait for a reinitialise or a restart.", waiting);

         string older = ServerSettingsPageStatus.SavedChangesSentence(new[] { "a", "b" }, 0, "14:20:00", null);
         Assert.Contains("hMailServer.ini settings apply after a service restart", older, StringComparison.Ordinal);
         Assert.DoesNotContain(" - applied immediately.", older, StringComparison.Ordinal);

         string partial = ServerSettingsPageStatus.SavedChangesSentence(new[] { "a", "b" }, 1, "14:20:00", 0);
         Assert.Equal("Saved 2 changes. 1 setting could not be written.", partial);
      }

      /// <summary>
      /// Alt+S presses Save changes without moving the keyboard - a button's access
      /// key clicks it where it is - and a NumberBox takes its text as its value only
      /// as the keyboard leaves it or Enter is pressed, so a refuse threshold typed
      /// and saved with Alt+S wrote the number the box held before and the sentence
      /// after the save said "no setting had changed" (seen in the renders of the
      /// threshold check, review finding F263). Ctrl+S moves the keyboard to the
      /// button first (PageCommands.Press); the save itself now takes each row's
      /// typed number first, whatever pressed it.
      /// </summary>
      [Fact]
      public void ASave_TakesANumberTypedAndNotYetTakenIn_BeforeItReadsTheEditors()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string save = WithheldRowTests.MemberText(page, "private bool Save_()");
         Assert.True(save != null, "ServerSettingsView.Save_ is gone.");
         int commit = save.IndexOf("typedIn.CommitTyped();", StringComparison.Ordinal);
         int refused = save.IndexOf("if (thresholdProblem_ != null)", StringComparison.Ordinal);
         int changed = save.IndexOf("var changed = new List<string>();", StringComparison.Ordinal);
         Assert.True(commit >= 0 && refused > commit && changed > commit, "Save_ does not take the typed numbers before it checks and reads the editors.");

         // Both number rows - ComText, with RestNumber under it, and IniNumber, with
         // SectionIniNumber - take their box's typed number, within its bounds.
         Assert.Equal(2, page.Split("public override void CommitTyped() => CommitTypedNumber(number_);").Length - 1);
         string helper = WithheldRowTests.MemberText(page, "protected static void CommitTypedNumber(Wpf.Ui.Controls.NumberBox box)");
         Assert.True(helper != null, "ServerSettingsView.ComSetting.CommitTypedNumber is gone.");
         Assert.Contains("System.Globalization.CultureInfo.CurrentCulture, out double typed))", helper, StringComparison.Ordinal);
         Assert.Contains("box.Value = Math.Max(box.Minimum, Math.Min(box.Maximum, typed));", helper, StringComparison.Ordinal);

         // A box nobody typed in is left as it is, a value outside its bounds that
         // the server holds with it: clamped, every save changed it unasked.
         int same = helper.IndexOf("if (box.Value == typed)", StringComparison.Ordinal);
         int clamp = helper.IndexOf("box.Value = Math.Max(", StringComparison.Ordinal);
         Assert.True(same >= 0 && clamp > same, "CommitTypedNumber clamps a box nobody typed in.");
      }

      // A pair at or below the mark is what SMTPConnection::DoSpamProtection_ makes of it:
      // refused with a 550 before the body; after it refused with a 554 while quarantining
      // is off and held in the quarantine while it is on (where the sentence said 554 of
      // both); and the mail of a sender whose domain is allowed neither refused nor held, and marked at the
      // mark threshold (where it said that none was only marked, of any sender). With the
      // switch unread, both outcomes are named.
      [Theory]
      [InlineData(5, 20, false, false, null)]
      [InlineData(5, 20, false, true, null)]
      [InlineData(5, 5, true, false, "A message scoring 5 or more is refused (550 before the body, 554 after it), and none is only marked: the mark threshold, 5, is not below that. A sender whose domain is allowed on the list of allowed and blocked domains is the exception: its mail is neither refused nor held on its score, and is marked at 5 or more.")]
      [InlineData(20, 5, true, false, "A message scoring 5 or more is refused (550 before the body, 554 after it), and none is only marked: the mark threshold, 20, is not below that. A sender whose domain is allowed on the list of allowed and blocked domains is the exception: its mail is neither refused nor held on its score, and is marked at 20 or more.")]
      [InlineData(5, 5, true, true, "A message scoring 5 or more is held in the quarantine when the score is reached after the body, and refused with a 550 when it is reached before; none is only marked: the mark threshold, 5, is not below that. A sender whose domain is allowed on the list of allowed and blocked domains is the exception: its mail is neither refused nor held on its score, and is marked at 5 or more.")]
      [InlineData(20, 5, true, true, "A message scoring 5 or more is held in the quarantine when the score is reached after the body, and refused with a 550 when it is reached before; none is only marked: the mark threshold, 20, is not below that. A sender whose domain is allowed on the list of allowed and blocked domains is the exception: its mail is neither refused nor held on its score, and is marked at 20 or more.")]
      [InlineData(20, 5, true, null, "A message scoring 5 or more is refused (550 before the body, 554 after it), or held in the quarantine after the body while quarantining is on; none is only marked: the mark threshold, 20, is not below that. A sender whose domain is allowed on the list of allowed and blocked domains is the exception: its mail is neither refused nor held on its score, and is marked at 20 or more.")]
      [InlineData(0, 20, false, true, "The mark threshold is 0, so no message is marked as spam: 0 means never.")]
      [InlineData(5, 0, false, false, "The refuse threshold is 0, so no message is refused or held on its score: 0 means never.")]
      [InlineData(0, 0, false, null, "Both thresholds are 0, so no score marks, refuses or holds any message.")]
      public void ThePairOfThresholds_IsRefusedWhenItContradictsItself_AndAZeroIsWarnedOf(int mark, int refuse, bool refused, bool? quarantine, string warning)
      {
         string problem = SpamPipeline.ThresholdProblem(mark, refuse);
         Assert.Equal(refused, problem != null);
         if (refused)
            Assert.Contains(mark.ToString(System.Globalization.CultureInfo.InvariantCulture), problem, StringComparison.Ordinal);
         Assert.Equal(warning, SpamPipeline.ThresholdWarning(mark, refuse, quarantine));
      }

      [Fact]
      public void APairAtOrBelowTheMark_SaysNo554WhileQuarantiningIsOn_AndTheWhiteSendersMark()
      {
         string on = SpamPipeline.ThresholdWarning(20, 5, true);
         Assert.DoesNotContain("554", on, StringComparison.Ordinal);
         Assert.Contains("held in the quarantine", on, StringComparison.Ordinal);
         foreach (bool? quarantine in new bool?[] { true, false, null })
            Assert.Contains("allowed on the list of allowed and blocked domains", SpamPipeline.ThresholdWarning(20, 5, quarantine), StringComparison.Ordinal);
         Assert.DoesNotContain("quarantine", SpamPipeline.ThresholdWarning(20, 5, false), StringComparison.Ordinal);
      }

      /// <summary>
      /// The server takes any pair of thresholds - COM's put_SpamMarkThreshold and
      /// put_SpamDeleteThreshold and the REST API's settings rows check neither against
      /// the other - and the regression suite leaves both at 10000; the page checked the
      /// pair as it opened, so on such a server it opened with "The refuse threshold must
      /// be above the mark threshold" on the field and refused every save. The pair the
      /// server holds is not refused, whatever it is, and is said under the two; an edit
      /// that makes or keeps a pair at or below the mark is refused as before.
      /// </summary>
      [Theory]
      [InlineData(10000, 10000, 10000, 10000, false)]
      [InlineData(20, 5, 20, 5, false)]
      [InlineData(10000, 10000, 10000, 9000, true)]
      [InlineData(10000, 10000, 10000, 10001, false)]
      [InlineData(5, 20, 5, 5, true)]
      [InlineData(5, 20, 20, 20, true)]
      [InlineData(5, 20, 5, 30, false)]
      [InlineData(5, 20, 0, 20, false)]
      public void ThePairTheServerHolds_IsNotRefused_AndAnEditThatContradictsItselfIs(int heldMark, int heldRefuse, int mark, int refuse, bool refused)
      {
         Assert.Equal(refused, SpamPipeline.ThresholdProblem(mark, refuse, heldMark, heldRefuse) != null);

         // As read, the bench's pair opens the page with no error and says what it does.
         Assert.Null(SpamPipeline.ThresholdProblem(heldMark, heldRefuse, heldMark, heldRefuse));
         if (heldMark > 0 && heldRefuse > 0 && heldRefuse <= heldMark)
            Assert.Contains("none is only marked", SpamPipeline.ThresholdWarning(heldMark, heldRefuse, false), StringComparison.Ordinal);
      }

      [Fact]
      public void TheRefuseThreshold_IsNamedForWhatItDoes_AndHeldAboveTheMarkThreshold()
      {
         string code = AntiSpam_();

         Assert.Contains("Label = L(\"Spam refuse threshold (score)\"),", code, StringComparison.Ordinal);
         Assert.Contains("Older versions called it the delete threshold.", code, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Spam delete threshold (score)\")", code, StringComparison.Ordinal);
         Assert.Contains("afterBuildUi_ = () => WireThresholdCheck_(markThreshold, refuseThreshold, thresholdNotice, quarantineSwitch);", code, StringComparison.Ordinal);
         Assert.Contains("quarantine.Settings.Add(quarantineSwitch);", code, StringComparison.Ordinal);

         string page = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string check = WithheldRowTests.MemberText(page, "private void WireThresholdCheck_(ComText mark, ComText refuse, FollowingNoticeRow notice, IniBool quarantine)");
         Assert.True(check != null, "ServerSettingsView.WireThresholdCheck_ is gone.");
         Assert.Contains("thresholdProblem_ = SpamPipeline.ThresholdProblem(markAt, refuseAt, thresholdsHeld_.Mark, thresholdsHeld_.Refuse);", check, StringComparison.Ordinal);
         Assert.Contains("row.Error = thresholdProblem_;", check, StringComparison.Ordinal);
         Assert.Contains("string warning = thresholdProblem_ == null ? SpamPipeline.ThresholdWarning(markAt, refuseAt, holds) : null;", check, StringComparison.Ordinal);

         // The warning reads the quarantine switch as it is ticked, and again when it changes;
         // unread, it is neither on nor off.
         Assert.Contains(": quarantine.Box.IsChecked is true;", check, StringComparison.Ordinal);
         Assert.Contains("!quarantine.IniStore.SettingsReachable", check, StringComparison.Ordinal);
         Assert.Contains("quarantine.Box.Checked += (_, _) => Later();", check, StringComparison.Ordinal);
         Assert.Contains("quarantine.Box.Unchecked += (_, _) => Later();", check, StringComparison.Ordinal);

         // The pair held is the one the boxes were filled with, taken before the first
         // check, which runs as the page opens.
         int held = check.IndexOf("thresholdsHeld_ = (ThresholdTyped_(markBox), ThresholdTyped_(refuseBox));", StringComparison.Ordinal);
         int first = check.LastIndexOf("Check();", StringComparison.Ordinal);
         Assert.True(held >= 0 && first > held, "WireThresholdCheck_ does not take the pair the server holds before its first check.");

         // A save writes nothing while the pair is refused.
         string save = WithheldRowTests.MemberText(page, "private bool Save_()");
         Assert.True(save != null, "ServerSettingsView.Save_ is gone.");
         int refused = save.IndexOf("if (thresholdProblem_ != null)", StringComparison.Ordinal);
         int write = save.IndexOf("setting.Write(owner, property);", StringComparison.Ordinal);
         Assert.True(refused >= 0 && write > refused, "Save_ writes before it looks at the thresholds' check.");

         // The refusal's notice asks for what is marked below, and goes once the pair
         // is put right, where it stayed over a pair with nothing marked.
         Assert.Contains("ShowNotice_(thresholdRefusal_, StatusLevel.Critical);", save, StringComparison.Ordinal);
         Assert.Contains("if (thresholdProblem_ == null && thresholdRefusal_ != null)", check, StringComparison.Ordinal);
         Assert.Contains("string.Equals(StatusNotice.Text, thresholdRefusal_, StringComparison.Ordinal))", check, StringComparison.Ordinal);
         Assert.Contains("ShowNotice_(null, StatusLevel.Normal);", check, StringComparison.Ordinal);

         // The refusal is the page's, whichever tab the save was made from: the page shows
         // the field it marks, as a setting the palette finds is shown, where a save from
         // Content filters said "correct what is marked below" with nothing marked there.
         Assert.Contains("thresholdField_ = refuse;", check, StringComparison.Ordinal);
         int notice = save.IndexOf("ShowNotice_(thresholdRefusal_, StatusLevel.Critical);", StringComparison.Ordinal);
         int reveal = save.IndexOf("RevealSetting(thresholdField_.Path, null);", StringComparison.Ordinal);
         Assert.True(notice >= 0 && reveal > notice && reveal < write, "A refused save does not show the refuse threshold.");

         // A save that wrote every row makes what it wrote the pair held.
         int baseline = save.IndexOf("RecordBaseline_();", StringComparison.Ordinal);
         int heldAfter = save.IndexOf("thresholdsHeld_ = (ThresholdTyped_(thresholdMarkField_.Number), ThresholdTyped_(thresholdField_.Number));", StringComparison.Ordinal);
         Assert.True(baseline >= 0 && heldAfter > baseline, "A save does not take the pair it wrote as the pair held.");

         // The sentence after a save names what it changed.
         Assert.Contains("ServerSettingsPageStatus.SavedChangesSentence(changed, failed, DateTime.Now.ToLongTimeString(), waiting)", save, StringComparison.Ordinal);
         Assert.Contains("Changed_(setting)", save, StringComparison.Ordinal);
      }

      [Fact]
      public void TheQuarantineIsSecondOnGeneral_AndTheContentFiltersShareATab()
      {
         string code = AntiSpam_();

         int general = code.IndexOf("Tab(L(\"General\")).Cards.Add(general);", StringComparison.Ordinal);
         int quarantine = code.IndexOf("Tab(L(\"General\")).Cards.Add(quarantine);", StringComparison.Ordinal);
         int links = code.IndexOf("Tab(L(\"General\")).Cards.Add(linkDomains);", StringComparison.Ordinal);
         Assert.True(general >= 0 && quarantine > general && links > quarantine, "The Quarantine card is not second on General.");

         Assert.Contains("Tab(L(\"Content filters\")).Cards.Add(sa);", code, StringComparison.Ordinal);
         Assert.Contains("Tab(L(\"Content filters\")).Cards.Add(hook);", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Tab(L(\"SpamAssassin\"))", code, StringComparison.Ordinal);

         // The documentation sends the reader to the tab by its name (SmtpPolicy.md, The
         // external filter engine), where it sent them to a SpamAssassin tab no longer there.
         string docs = Docs_();
         foreach (string file in Directory.EnumerateFiles(docs, "*.md"))
            Assert.False(File.ReadAllText(file).Contains("SpamAssassin tab", StringComparison.Ordinal), Path.GetFileName(file) + " names the SpamAssassin tab, which is Content filters.");
         Assert.Contains("on the Content filters tab", File.ReadAllText(Path.Join(docs, "SmtpPolicy.md")), StringComparison.Ordinal);

         // Found by its old names and by rspamd, on its tab.
         foreach (string query in new[] { "rspamd", "Content filters" })
         {
            PaletteRow row = PaletteSearch.Query(query, null, null).FirstOrDefault(r => r.Kind == PaletteRowKind.Page && r.Page == "antispam");
            Assert.True(row != null, "'" + query + "' does not find Anti-spam settings.");
            Assert.Equal("Content filters", row.Tab);
         }

         Assert.NotNull(PaletteSearch.Query("Delete threshold", null, null).FirstOrDefault(r => r.Kind == PaletteRowKind.Page && r.Page == "antispam"));
      }

      [Fact]
      public void TheSpamTraps_SayWhetherAnyDomainHasOne()
      {
         string code = AntiSpam_();
         Assert.Contains("traps.Settings.Add(new SpamTrapCountRow());", code, StringComparison.Ordinal);

         string page = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("L(\"No domain has a trap address yet, so these settings do nothing.\")", page, StringComparison.Ordinal);
         Assert.Contains("if (!string.IsNullOrWhiteSpace((string)domain.SpamTraps))", page, StringComparison.Ordinal);

         // One call per domain, so off the UI thread once the card is built, the
         // notice saying so meanwhile: on it, every opening of the page waited for
         // the walk (review finding F265's shape).
         string build = WithheldRowTests.MemberText(page, "private sealed class SpamTrapCountRow : ComSetting, INotPersisted");
         Assert.True(build != null, "ServerSettingsView.SpamTrapCountRow is gone.");
         Assert.Contains("Text = L(\"Counting the domains that have trap addresses…\")", build, StringComparison.Ordinal);
         Assert.Contains("CountThenSay_(notice);", build, StringComparison.Ordinal);
         Assert.Contains("withTraps = await System.Threading.Tasks.Task.Run(DomainsWithTraps_);", build, StringComparison.Ordinal);
         Assert.DoesNotContain("int? withTraps = DomainsWithTraps_();", build, StringComparison.Ordinal);
      }

      private static string Docs_()
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Join(at.FullName, "hmailserver", "docs");
            if (Directory.Exists(candidate))
               return candidate;
         }

         Assert.Fail("hmailserver/docs could not be found from the test binaries.");
         return null;
      }

      private static string AntiSpam_()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string body = WithheldRowTests.MemberText(page, "private void BuildAntiSpam()");
         Assert.True(body != null, "ServerSettingsView.BuildAntiSpam is gone.");
         return body;
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
