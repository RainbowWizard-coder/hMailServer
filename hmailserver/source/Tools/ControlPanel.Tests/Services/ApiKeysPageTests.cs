// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The REST API keys page says what each key is. Each row's pill read a
   /// severity word - "Warning" for a key with full authority, "OK" for a
   /// read-only one, "Information" for an expired one - which said how much
   /// attention the key wanted and not what it was; the new key's line put the
   /// English "read-only" into every language; the source field's example carried
   /// the English "or" into German and Japanese under a plural caption for a
   /// field that takes one value; and an empty store was said twice.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class ApiKeysPageTests
   {
      [Theory]
      [InlineData("Ignored", "StatusLevel.Warning")]
      [InlineData("Full authority", "StatusLevel.Warning")]
      [InlineData("Read-only", "StatusLevel.Good")]
      public void EachKeysPill_NamesItsState_AtItsLevel(string word, string level)
      {
         string row = KeyRow();
         Match pill = Regex.Match(row, @"level = (StatusLevel\.\w+);\s*pill = L\(""" + Regex.Escape(word) + @"""\);");
         Assert.True(pill.Success, "No key's pill reads \"" + word + "\".");
         Assert.Equal(level, pill.Groups[1].Value);
      }

      [Fact]
      public void AnExpiredKey_IsNormal_AndOneWhoseExpiryCannotBeRead_IsAWarning()
      {
         string row = KeyRow();
         Assert.Contains("level = key.ExpiresAt == null ? StatusLevel.Warning : StatusLevel.Normal;", row, StringComparison.Ordinal);
         Assert.Contains("pill = key.ExpiresAt == null ? L(\"Expiry unreadable\") : L(\"Expired\");", row, StringComparison.Ordinal);
         Assert.Contains("Text = pill,", row, StringComparison.Ordinal);
         Assert.DoesNotContain("SeverityWord", row, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDialogsWords_AreTranslated_AndTheSourceIsOneValue()
      {
         string code = Source();
         Assert.Contains("L(\"Full authority\") : L(\"Read-only\")", code, StringComparison.Ordinal);
         Assert.DoesNotContain(": \"read-only\"", code, StringComparison.Ordinal);
         Assert.Contains("PlaceholderText = L(\"For example 10.0.0.5 or 10.0.0.0/24\")", code, StringComparison.Ordinal);
         Assert.Contains("L(\"Source address or range it may be used from (optional)\")", code, StringComparison.Ordinal);
         Assert.DoesNotContain("10.0.0.5, or 10.0.0.0/24", code, StringComparison.Ordinal);
      }

      [Fact]
      public void AnEmptyStore_IsSaidOnce_ByTheListsEmptyState_WithItsAction()
      {
         string code = Source();
         Assert.DoesNotContain("No API keys exist.", code, StringComparison.Ordinal);
         Assert.Contains("empty_.Action = createFirst;", code, StringComparison.Ordinal);
         Assert.Contains("summary_.Visibility = Visibility.Collapsed;", code, StringComparison.Ordinal);
      }

      private static string KeyRow()
      {
         string row = WithheldRowTests.MemberText(Source(), "private FrameworkElement BuildKeyRow_(ApiKeyRecord key)");
         Assert.True(row != null, "ApiKeysView.BuildKeyRow_ is gone.");
         return row;
      }

      private static string Source()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ApiKeysView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/ApiKeysView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
