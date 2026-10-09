// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F267: the Blocked attachments page showed "5 rules" and the
   /// list, and never whether attachment blocking was on - the virus overview knew
   /// ("Switched off ... none is applied") - and the greylisting lists said nothing
   /// while greylisting was off. A list now carries CollectionSpec.StateNotice, read
   /// with the list on every load: a Warning above the blocked attachments while
   /// blocking is off, Information above the two greylisting lists while greylisting
   /// is, each with the page that holds the switch. The entries are patterns
   /// throughout, where the page called them wildcards, its pill rules and the
   /// overview patterns, and the subtitle says what happens to a match.
   ///
   /// Scanned rather than rendered, as QuarantinePageTests is: this assembly has no
   /// WPF. Each test fails against the tree before the fix.
   /// </summary>
   public class ListStateNoticeTests
   {
      [Fact]
      public void BlockedAttachments_SayWhenBlockingIsOff_AtWarning_WithTheAntiVirusPage()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         string spec = SpecText(specs, "BlockedAttachments()");
         Assert.True(spec != null, "CollectionSpecs.BlockedAttachments is gone.");
         Assert.Contains("StateNotice = BlockingStateNotice_", spec, StringComparison.Ordinal);

         string notice = WithheldRowTests.MemberText(specs, "private static ListStateNotice BlockingStateNotice_(int patterns)");
         Assert.True(notice != null, "CollectionSpecs.BlockingStateNotice_ is gone.");
         Assert.Contains("(bool)Settings.AntiVirus.EnableAttachmentBlocking", notice, StringComparison.Ordinal);
         Assert.Contains("return null;", notice, StringComparison.Ordinal);
         Assert.Contains("StatusLevel.Warning", notice, StringComparison.Ordinal);
         Assert.Contains("Page = \"antivirus\"", notice, StringComparison.Ordinal);

         // One whole sentence for none, one and several: the count is never glued.
         Assert.Contains("so a pattern added here is not applied until it is turned on.", notice, StringComparison.Ordinal);
         Assert.Contains("so this pattern is not applied.", notice, StringComparison.Ordinal);
         Assert.Contains("so none of these {0} patterns is applied.", notice, StringComparison.Ordinal);
      }

      [Fact]
      public void TheGreylistingLists_SayWhenGreylistingIsOff_AtInformation_WithTheAntiSpamPage()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         foreach (string list in new[] { "GreyListWhiteList()", "GreyListBypass()" })
         {
            string spec = SpecText(specs, list);
            Assert.True(spec != null, "CollectionSpecs." + list + " is gone.");
            Assert.True(spec.Contains("StateNotice = GreylistingStateNotice_", StringComparison.Ordinal),
               "CollectionSpecs." + list + " does not say whether greylisting is on.");
         }

         string notice = WithheldRowTests.MemberText(specs, "private static ListStateNotice GreylistingStateNotice_(int _)");
         Assert.True(notice != null, "CollectionSpecs.GreylistingStateNotice_ is gone.");
         Assert.Contains("(bool)AntiSpam.GreyListingEnabled", notice, StringComparison.Ordinal);
         Assert.Contains("StatusLevel.Information", notice, StringComparison.Ordinal);
         Assert.Contains("Page = \"antispam\"", notice, StringComparison.Ordinal);
      }

      [Fact]
      public void TheEditor_ReadsTheSwitchWithTheList_AndHidesTheNoticeWhenTheReadFails()
      {
         string editor = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         // The read is Load_, which Reload and the list's own acts call (review finding F208).
         string reload = WithheldRowTests.MemberText(editor, "private void Load_(int selectId, int selectIndex, bool focus)");
         Assert.True(reload != null, "CollectionEditorView.Load_ is gone.");

         // Inside the read's own try, so a link lost between the list and the
         // switch is one failure, said once by the read's Critical notice.
         int read = reload.IndexOf("spec_.StateNotice?.Invoke(rows_.Count)", StringComparison.Ordinal);
         int caught = reload.IndexOf("catch (Exception ex)", StringComparison.Ordinal);
         Assert.True(read > 0 && caught > read, "The switch is not read inside the list's read.");

         int failed = reload.IndexOf("ReadFailure.Show(", StringComparison.Ordinal);
         Assert.True(reload.IndexOf("ShowState_(null);", failed, StringComparison.Ordinal) > failed,
            "A failed read leaves the last read's notice about the switch on the page.");
         Assert.Contains("ShowState_(state);", reload, StringComparison.Ordinal);

         // Above the last action's line, as a standing state is.
         Assert.True(editor.IndexOf("parts.Children.Add(stateNotice_);", StringComparison.Ordinal)
            < editor.IndexOf("parts.Children.Add(status_);", StringComparison.Ordinal),
            "The switch's notice is not above the last action's line.");
      }

      [Fact]
      public void BlockedAttachments_ArePatternsThroughout_AndTheSubtitleSaysWhatHappensToAMatch()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         string spec = SpecText(specs, "BlockedAttachments()");
         Assert.True(spec != null, "CollectionSpecs.BlockedAttachments is gone.");

         Assert.Contains("ItemNoun = L(\"pattern\")", spec, StringComparison.Ordinal);
         Assert.Contains("CountOne = N(\"1 pattern\")", spec, StringComparison.Ordinal);
         Assert.Contains("CountMany = N(\"{0} patterns\")", spec, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"File-name pattern\")", spec, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Wildcard\")", spec, StringComparison.Ordinal);
         Assert.DoesNotContain("\"1 rule\"", spec, StringComparison.Ordinal);

         // It read as if the wildcards were stripped, and of incoming mail alone;
         // the server replaces a matching part with a note, on every delivery.
         Assert.DoesNotContain("stripped", spec, StringComparison.Ordinal);
         Assert.Contains("are replaced by a short note", spec, StringComparison.Ordinal);
      }

      /// <summary>
      /// One spec of CollectionSpecs, from its factory's name to the end of its
      /// initialiser: WithheldRowTests.MemberText stops an arrow-bodied member at
      /// its first semicolon, and a subtitle or a comment may hold one.
      /// </summary>
      private static string SpecText(string specs, string factory)
      {
         int at = specs.IndexOf("public static CollectionEditorView " + factory, StringComparison.Ordinal);
         if (at < 0)
            return null;
         int end = specs.IndexOf("      });", at, StringComparison.Ordinal);
         return end < 0 ? null : specs.Substring(at, end - at);
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
