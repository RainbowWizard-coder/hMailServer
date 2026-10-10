// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// One filled accent button on a screen (review finding F140): with a certificate
   /// selected, the SSL certificates page drew Save passphrase and the add form's Add
   /// both as Primary, beside a filled red Delete selected, and Directory
   /// synchronisation drew Preview as Primary under the header's Save changes - two
   /// buttons each looking like the page's one way on. The passphrase box was as wide
   /// as its placeholder, 151 pixels, and grew while a passphrase was typed.
   /// </summary>
   public class OnePrimaryPerScreenTests
   {
      [Fact]
      public void TheSslCertificatesPage_HasTheAddFormsAddAsItsOnlyPrimary()
      {
         string xaml = SaveStateModelTests.ReadControlPanelFile("Views/SslCertificatesView.xaml");

         MatchCollection primaries = Regex.Matches(xaml, "<ui:Button[^>]*Appearance=\"Primary\"[^>]*>", RegexOptions.Singleline);
         Assert.Single(primaries);
         Assert.Contains("x:Name=\"AddCertButton\"", primaries[0].Value, StringComparison.Ordinal);

         // Save passphrase is the details card's own act, a secondary button, after Remove.
         Match save = Regex.Match(xaml, "<ui:Button x:Name=\"SavePassphraseButton\"[^>]*>", RegexOptions.Singleline);
         Assert.True(save.Success, "SslCertificatesView.xaml no longer has SavePassphraseButton.");
         Assert.DoesNotContain("Appearance=", save.Value, StringComparison.Ordinal);
         Assert.True(xaml.IndexOf("Re_move stored passphrase", StringComparison.Ordinal) < save.Index,
            "Remove stored passphrase no longer comes before Save passphrase.");
      }

      [Fact]
      public void TheSslCertificatesPagesPassphraseBox_HasAWidthOfItsOwn()
      {
         string xaml = SaveStateModelTests.ReadControlPanelFile("Views/SslCertificatesView.xaml");

         Match box = Regex.Match(xaml, "<local:PasswordField x:Name=\"PassphraseBox\"[^>]*>");
         Assert.True(box.Success, "SslCertificatesView.xaml no longer has PassphraseBox.");
         // MinWidth, which WPF-UI's template gives its box, and MaxWidth, so that a
         // passphrase typed does not widen it.
         Assert.Contains(" MinWidth=\"360\"", box.Value, StringComparison.Ordinal);
         Assert.Contains(" MaxWidth=\"360\"", box.Value, StringComparison.Ordinal);
      }

      [Fact]
      public void DirectorySynchronisation_HasTheHeadersSaveChangesAsItsOnlyPrimary()
      {
         string code = SaveStateModelTests.ReadControlPanelFile("Views/DirectorySyncView.cs").Replace("\r\n", "\n");

         string[] primaries = code.Split('\n').Where(line => line.Contains("ControlAppearance.Primary", StringComparison.Ordinal)).ToArray();
         Assert.True(primaries.Length == 1, "DirectorySyncView.cs draws more than one Primary button:\n" + string.Join("\n", primaries.Select(l => l.Trim())));

         int save = code.IndexOf("saveButton_ = new Wpf.Ui.Controls.Button", StringComparison.Ordinal);
         int primary = code.IndexOf("ControlAppearance.Primary", StringComparison.Ordinal);
         Assert.True(save >= 0 && primary > save && primary - save < 200, "The one Primary is not the header's Save changes.");

         // And nothing turns Preview back into one while the selection is saved.
         Assert.DoesNotContain("previewButton_.Appearance", code, StringComparison.Ordinal);
      }
   }
}
