// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F040: with the link lost, the setup checklist drew "4 need
   /// action, 7 cannot be told from here, 0 done, 4 not needed. Next step: MTA-STS
   /// policy hosting..." first, a tally counted from values that could not be read,
   /// and the notice that said so came after it and spoke of "the items above" while
   /// it sat above them. The notice is first, says the cards below may be incomplete
   /// with its reason last, and the tally says how it counts an item that depends on
   /// a value that could not be read; and while reads fail neither the tally nor a
   /// card names a next step, and an item that depends on an unread value cannot be
   /// told.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class SetupChecklistReadFailureTests
   {
      [Fact]
      public void TheFailure_IsAboveTheTally()
      {
         string code = Source();
         int failure = code.IndexOf("page.Children.Add(readFailure_);", StringComparison.Ordinal);
         int tally = code.IndexOf("page.Children.Add(summary_);", StringComparison.Ordinal);
         int items = code.IndexOf("page.Children.Add(body_);", StringComparison.Ordinal);
         Assert.True(failure >= 0 && tally >= 0 && items >= 0, "The checklist no longer adds its notices and items to the page.");
         Assert.True(failure < tally && tally < items, "The read failure is not above the tally, or the tally not above the items.");
      }

      [Fact]
      public void TheFailure_SaysTheCardsBelow_WithItsReasonLast()
      {
         string code = Source();
         Assert.DoesNotContain("above may be incomplete", code, StringComparison.Ordinal);
         // DNS records' and the overviews' sentence (t31c, F325), one for one thing.
         Assert.Contains("F(\"Some values could not be read, so the sections below may be incomplete. Why: {0}\", checks_.FirstError)", code, StringComparison.Ordinal);
         Assert.Contains("F(\"Some values could not be read, so the sections below may be incomplete. The first reason: {0}\", checks_.FirstError)", code, StringComparison.Ordinal);
      }

      [Fact]
      public void WhileReadsFail_TheTallySaysHowItCountsAnUnreadValue_AndNamesNoNextStep()
      {
         string render = WithheldRowTests.MemberText(Source(), "private void RenderItems(string dnsNote)");
         Assert.True(render != null, "ExternalSetupView.RenderItems is gone.");

         // A sentence of its own before the tally while values could not be read, and
         // the next step only while every value was. It said "Only what could be read is
         // counted.", which the numbers were not: every item is counted, and an unread
         // value is a Cannot tell finding of its item.
         Assert.DoesNotContain("Only what could be read", Source(), StringComparison.Ordinal);
         Assert.Contains("summary_.Text = Sentences(partial ? L(\"An item that depends on a value that could not be read is counted as one that cannot be told, unless what was read already shows that it needs action.\") : null,", render, StringComparison.Ordinal);
         Assert.Contains("SetupTally.Tally(action, unknown, done, unused, next?.Title));", render, StringComparison.Ordinal);
      }

      [Fact]
      public void WhileReadsFail_NoCardIsTheNextStep()
      {
         // The tally named no next step while values could not be read, and the first
         // card that needed action kept its Next step pill, the page's primary button
         // and "Next step." in its name: the step is withheld where it is chosen, before
         // the cards are drawn, so that neither can name it.
         string render = WithheldRowTests.MemberText(Source(), "private void RenderItems(string dnsNote)");
         Assert.True(render != null, "ExternalSetupView.RenderItems is gone.");

         int partial = render.IndexOf("bool partial = checks_.FailedReads > 0;", StringComparison.Ordinal);
         int next = render.IndexOf("SetupItem next = partial ? null : checks_.Items.FirstOrDefault(i => i.State == SetupItemState.ActionNeeded);", StringComparison.Ordinal);
         int cards = render.IndexOf("Card card = ItemCard(item, ++number, isNext, out FrameworkElement link);", StringComparison.Ordinal);
         Assert.Contains("bool isNext = ReferenceEquals(item, next);", render, StringComparison.Ordinal);
         Assert.True(partial >= 0, "RenderItems no longer asks whether values could not be read.");
         Assert.True(next > partial, "The next step is chosen without asking whether values could not be read, or before it is asked.");
         Assert.True(cards > next, "The cards are drawn before the next step is chosen, or no longer told which card it is.");
         Assert.Equal(1, CountOf(render, "SetupItem next ="));
         Assert.DoesNotContain("!partial", render, StringComparison.Ordinal);
      }

      [Fact]
      public void AnUnreadDomainList_CannotBeTold_WhereDkimSaidNoDomainsAreConfigured()
      {
         // With the link lost, DKIM signing said "No domains are configured, so there is
         // nothing to sign" - Not needed, from the empty list a read that failed returns.
         // The list's failure is kept, and each item that judges the domains says it.
         string checks = ChecksSource();
         string read = WithheldRowTests.MemberText(checks, "private List<DomainDkim> ReadDomains()");
         Assert.True(read != null, "ExternalSetupChecks.ReadDomains is gone.");
         Assert.Contains("domainListError_ = ServerSession.DescribeComError(ex);", read, StringComparison.Ordinal);

         string dkim = WithheldRowTests.MemberText(checks, "private SetupItem DkimItem(List<DomainDkim> domains, bool local)");
         Assert.True(dkim != null, "ExternalSetupChecks.DkimItem is gone.");
         Assert.Contains("bool listed = domainListError_ == null;", dkim, StringComparison.Ordinal);
         Assert.Contains("item.Add(SetupItemState.CannotTell, F(\"The domain list could not be read: {0}\", domainListError_));", dkim, StringComparison.Ordinal);
         // Not needed only from a list that was read.
         Assert.Contains("if (listed && domains.Count == 0)", dkim, StringComparison.Ordinal);
         Assert.Contains("else if (listed && signing.Count == 0)", dkim, StringComparison.Ordinal);

         string sealing = WithheldRowTests.MemberText(checks, "private SetupItem ArcSealingItem(List<DomainDkim> domains, IniFeatureStore ini, bool iniReadable, string iniExcuse)");
         Assert.True(sealing != null, "ExternalSetupChecks.ArcSealingItem is gone.");
         int none = sealing.IndexOf("else if (domainListError_ != null)", StringComparison.Ordinal);
         int action = sealing.IndexOf("item.Add(SetupItemState.ActionNeeded,", StringComparison.Ordinal);
         Assert.True(none >= 0 && none < action, "ARC sealing says that no domain can seal before asking whether the domain list was read.");

         string mtaSts = WithheldRowTests.MemberText(checks, "private SetupItem MtaStsItem(List<DomainDkim> domains, IniFeatureStore ini, bool iniReadable, string iniExcuse)");
         Assert.True(mtaSts != null, "ExternalSetupChecks.MtaStsItem is gone.");
         Assert.Contains("if (domainListError_ != null)\n               item.Add(SetupItemState.CannotTell, F(\"The domain list could not be read, so not every domain's _mta-sts record was looked up: {0}\", domainListError_));", mtaSts, StringComparison.Ordinal);
      }

      [Fact]
      public void AnUnreadDmarcScore_OrChecklist_CannotBeTold()
      {
         string checks = ChecksSource();

         // ARC filtering on, with sealers, and a DMARC failure score that could not be
         // read: the item said Done, though at zero ARC does nothing.
         string filtering = WithheldRowTests.MemberText(checks, "private SetupItem ArcFilteringItem()");
         Assert.True(filtering != null, "ExternalSetupChecks.ArcFilteringItem is gone.");
         Assert.Contains("else if (failedReads_ != failuresBefore && failuresBefore == arcFailuresBefore && arcEnabled)", filtering, StringComparison.Ordinal);

         // A checklist the server gave that could not be parsed in full kept the items
         // before the fault and left the rest out, counted nowhere: it is one item that
         // cannot be told, as a checklist that could not be read at all is.
         string firstHour = WithheldRowTests.MemberText(checks, "private List<SetupItem> FirstHourItems()");
         Assert.True(firstHour != null, "ExternalSetupChecks.FirstHourItems is gone.");
         int parse = firstHour.IndexOf("catch (Exception ex) when (ex is JsonException", StringComparison.Ordinal);
         Assert.True(parse >= 0, "FirstHourItems no longer catches a checklist it cannot parse.");
         string caught = firstHour.Substring(parse);
         Assert.Contains("items.Clear();", caught, StringComparison.Ordinal);
         Assert.Contains("unread.Add(SetupItemState.CannotTell, F(\"The server's checklist could not be read: {0}\", ServerSession.DescribeComError(ex)));", caught, StringComparison.Ordinal);
         Assert.Contains("items.Add(unread);", caught, StringComparison.Ordinal);
      }

      private static string ChecksSource()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Services", "ExternalSetupChecks.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
         }

         Assert.Fail("ControlPanel/Services/ExternalSetupChecks.cs could not be found from the test binaries.");
         return null;
      }

      private static int CountOf(string text, string part)
      {
         int count = 0;
         for (int at = text.IndexOf(part, StringComparison.Ordinal); at >= 0; at = text.IndexOf(part, at + part.Length, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static string Source()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ExternalSetupView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
         }

         Assert.Fail("ControlPanel/Views/ExternalSetupView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
