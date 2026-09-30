// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The one caption convention behind every Alt-key mnemonic in the Control
   /// Panel. Buttons and check boxes read it through their templates; the
   /// TextBlock captions above editors, and every caption that comes from data,
   /// read it through MnemonicText - so the rule pinned here is the rule the whole
   /// product follows, including the escape that keeps a folder named "in_tray"
   /// from acquiring an access key.
   /// </summary>
   public class MnemonicTextTests
   {
      [Theory]
      [InlineData("_Save changes", "Save changes", 0, "S")]
      [InlineData("Save s_ettings", "Save settings", 6, "E")]
      [InlineData("Back up _domains (accounts, aliases, lists)", "Back up domains (accounts, aliases, lists)", 8, "D")]
      [InlineData("Start _restore", "Start restore", 6, "R")]
      public void TheMarkedLetterIsTheKeyAndTheMarkerIsNotDisplayed(string caption, string text, int index, string key)
      {
         var (shown, keyIndex) = MnemonicText.Parse(caption);

         Assert.Equal(text, shown);
         Assert.Equal(index, keyIndex);
         Assert.Equal(key, MnemonicText.Key(caption));
         Assert.Equal(text, MnemonicText.Strip(caption));
      }

      [Fact]
      public void ADoubledUnderscoreIsALiteralOneAndMarksNothing()
      {
         var (shown, keyIndex) = MnemonicText.Parse("in__tray");

         Assert.Equal("in_tray", shown);
         Assert.Equal(-1, keyIndex);
         Assert.Null(MnemonicText.Key("in__tray"));
      }

      [Fact]
      public void OnlyTheFirstMarkerCountsAndLaterOnesAreText()
      {
         var (shown, keyIndex) = MnemonicText.Parse("_Copy host_name");

         Assert.Equal("Copy host_name", shown);
         Assert.Equal(0, keyIndex);
      }

      [Fact]
      public void AnUnderscoreThatCannotMarkALetterIsText()
      {
         Assert.Equal(("odd_ one", -1), MnemonicText.Parse("odd_ one"));
         Assert.Equal(("trailing_", -1), MnemonicText.Parse("trailing_"));
      }

      [Fact]
      public void TheKeyIsUpperCaseWhateverTheCaptionSays()
      {
         Assert.Equal("E", MnemonicText.Key("Save s_ettings"));
         Assert.Equal("S", MnemonicText.Key("_save"));
      }

      [Fact]
      public void ACaptionWithoutAMarkerHasNoKey()
      {
         Assert.Null(MnemonicText.Key("Cancel"));
         Assert.Equal(("Cancel", -1), MnemonicText.Parse("Cancel"));
      }

      [Fact]
      public void EscapedDataRoundTripsAndNeverGrowsAKey()
      {
         const string folder = "in_tray_2";

         string escaped = MnemonicText.Escape(folder);

         Assert.Equal("in__tray__2", escaped);
         Assert.Equal(folder, MnemonicText.Strip(escaped));
         Assert.Null(MnemonicText.Key(escaped));
      }

      /// <summary>
      /// What WPF's AccessText draws for a caption, and its automation peer names: each case
      /// measured on .NET 10's WPF (a button with the caption as its content, its peer's name
      /// and AccessText's own RemoveAccessKeyMarker). The caption copied from a control
      /// (SelectableCaption) is this, so it is what the control shows.
      /// </summary>
      [Theory]
      [InlineData("_Save", "Save")]
      [InlineData("a__b", "a_b")]
      [InlineData("__init__", "_init_")]
      [InlineData("odd_ one", "odd one")]
      [InlineData("_ _a", " _a")]
      [InlineData("trailing_", "trailing_")]
      [InlineData("_Copy host_name", "Copy host_name")]
      [InlineData("x_y_z", "xy_z")]
      [InlineData("a___b", "a_b")]
      [InlineData("x____y", "x__y")]
      [InlineData("コピー(_C)", "コピー(C)")]
      [InlineData("Spara _ändringar", "Spara ändringar")]
      [InlineData("no marker", "no marker")]
      public void AccessTextShown_IsWhatWpfDraws(string caption, string shown)
      {
         Assert.Equal(shown, MnemonicText.AccessTextShown(caption));
      }

      [Fact]
      public void AccessTextShown_DiffersFromStrip_OnlyBeforeWhiteSpace()
      {
         foreach (string caption in new[] { "_Save changes", "in__tray", "_Copy host_name", "trailing_", "Save s_ettings" })
            Assert.Equal(MnemonicText.Strip(caption), MnemonicText.AccessTextShown(caption));

         Assert.Equal("odd_ one", MnemonicText.Strip("odd_ one"));
         Assert.Equal("odd one", MnemonicText.AccessTextShown("odd_ one"));
         Assert.Equal(string.Empty, MnemonicText.AccessTextShown(null));
         Assert.Equal(string.Empty, MnemonicText.AccessTextShown(string.Empty));
      }

      [Fact]
      public void EmptyAndNullAreHarmless()
      {
         Assert.Equal((string.Empty, -1), MnemonicText.Parse(null));
         Assert.Equal((string.Empty, -1), MnemonicText.Parse(string.Empty));
         Assert.Null(MnemonicText.Key(null));
         Assert.Null(MnemonicText.Escape(null));
      }
   }
}
