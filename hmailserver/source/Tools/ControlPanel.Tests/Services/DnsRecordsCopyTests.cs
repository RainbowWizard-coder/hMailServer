// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The DNS records page's captions, its state with no domain and its MX box
   /// (review finding F306): one caption said "Record type: MX.  Host/Name - the
   /// domain itself ...:", two statements and a double space; Copy host stood a
   /// card from the PTR card's Copy host name, which copies the HELO name; with
   /// no domains the page drew an empty, enabled domain list above a bare
   /// sentence and a twelve-pixel link; and the one-line MX value had a box 46
   /// pixels tall below a 28-pixel host box.
   /// </summary>
   public class DnsRecordsCopyTests
   {
      private static readonly string Page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DnsRecordsView.cs"));

      [Fact]
      public void TheTypeAndTheNameAreTwoCaptions_AndTheCopyButtonsSayNameAndValue()
      {
         Assert.Contains("Label(mx ? L(\"Record type: MX\") : L(\"Record type: TXT\"))", Page, StringComparison.Ordinal);
         Assert.Contains("Label(hostNote != null ? F(\"Name - {0}\", hostNote) : L(\"Name\"))", Page, StringComparison.Ordinal);
         Assert.Contains("Content = L(\"Copy name\")", Page, StringComparison.Ordinal);
         Assert.Contains("Content = L(\"Copy value\")", Page, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Copy host\")", Page, StringComparison.Ordinal);
      }

      [Fact]
      public void WithNoDomain_TheListGoes_AndTheEmptyStateLeadsToDomains()
      {
         Assert.Contains("domainRow_.Visibility = domains_.Count == 0 ? Visibility.Collapsed : Visibility.Visible;", Page, StringComparison.Ordinal);
         // A list that could not be read is no claim that the server hosts no domain.
         Assert.Contains("noDomains_.Visibility = domains_.Count == 0 && failedReads_ == 0 ? Visibility.Visible : Visibility.Collapsed;", Page, StringComparison.Ordinal);
         Assert.Contains("Icon = Wpf.Ui.Controls.SymbolRegular.Globe24", Page, StringComparison.Ordinal);
         Assert.Contains("noDomains_.Action = openDomains;", Page, StringComparison.Ordinal);
         Assert.Contains("NavigateTo(\"domains\")", Page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheMxValueBoxIsOneLine()
      {
         Assert.Contains("block.ValueBox = ReadOnlyBox(mx ? 0 : 44);", Page, StringComparison.Ordinal);
      }
   }
}
