// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The hMailServer.ini pages' ports, timeouts, cache lifetimes and work factors were
   /// 520-pixel free-text boxes whose save wrote whatever was typed, the MTA-STS mode
   /// was free text beside a list type in the same file, and the DNS override took
   /// anything (review findings F220 and F139). These hold the ranges, the words a
   /// refusal says, the address rule, and the pages' definitions: every numeric key a
   /// NumberSetting with the range the server reads it in.
   /// </summary>
   public class SettingNumberRangeTests
   {
      [Theory]
      [InlineData("0", 0, 65535, false, true)]
      [InlineData("65535", 0, 65535, false, true)]
      [InlineData("65536", 0, 65535, false, false)]
      [InlineData("-1", 0, 65535, false, false)]
      [InlineData("8o", 0, 65535, false, false)]
      [InlineData("", 0, 65535, false, false)]
      [InlineData("1.5", 0, 65535, false, false)]
      [InlineData(" 80 ", 1, 65535, false, true)]
      [InlineData("0", 10000, 10000000, true, true)]     // the built-in work factor
      [InlineData("9999", 10000, 10000000, true, false)]
      [InlineData("10,000", 10000, 10000000, true, true)] // as an English reader writes it
      [InlineData("2147483647", 0, int.MaxValue, false, true)]
      public void ANumber_IsSaved_OnlyInsideTheRangeTheServerReadsItIn(string text, long minimum, long maximum, bool zero, bool accepted)
         => Assert.Equal(accepted, SettingNumberRange.Error(text, minimum, maximum, zero) == null);

      [Fact]
      public void ARefusal_SaysTheRange_InTheReadersNumbers()
      {
         CultureInfo saved = CultureInfo.CurrentCulture;
         try
         {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("en-GB");
            Assert.Equal("Enter a whole number from 0 to 65,535.", SettingNumberRange.Describe(0, 65535));
            Assert.Equal("Enter a whole number of 10 or more.", SettingNumberRange.Describe(10, int.MaxValue));
            Assert.Equal("Enter 0 for the built-in value, or a whole number from 10,000 to 10,000,000.", SettingNumberRange.Describe(10000, 10000000, true));

            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("de-DE");
            Assert.Contains("65.535", SettingNumberRange.Describe(0, 65535), StringComparison.Ordinal);
            Assert.True(SettingNumberRange.TryParse("10.000", out long german) && german == 10000);
            Assert.True(SettingNumberRange.TryParse("10000", out long plain) && plain == 10000);
         }
         finally
         {
            CultureInfo.CurrentCulture = saved;
         }
      }

      [Theory]
      [InlineData("", true)]
      [InlineData("1.1.1.1", true)]
      [InlineData(" 9.9.9.9 ", true)]
      [InlineData("255.255.255.255", true)]
      [InlineData("8.8.8", false)]          // an older form .NET reads and the server refuses
      [InlineData("010.0.0.1", false)]      // octal to inet_addr, refused by inet_pton
      [InlineData("256.1.1.1", false)]
      [InlineData("2606:4700:4700::1111", false)] // the server takes no IPv6 address there
      [InlineData("1.1.1.1, 8.8.8.8", false)]
      [InlineData("dns.example.com", false)]
      public void TheDnsOverride_IsOneIPv4Address_AsTheServerReadsIt(string value, bool accepted)
         => Assert.Equal(accepted, SettingAddressRule.DnsOverride(value) == null);

      /// <summary>The range each numeric key of the pages is held to, and where the bound comes from on the server.</summary>
      public static IEnumerable<object[]> Ranges() => new[]
      {
         new object[] { "AcmeHttpPort", 1L, 65535L },                     // EnvironmentSettings::CheckIniValue; 0 leaves no challenge listener
         new object[] { "RestApiPort", 0L, 65535L },
         new object[] { "MetricsServerPort", 0L, 65535L },
         new object[] { "ManageSieveServerPort", 0L, 65535L },
         new object[] { "WebServicesHttpPort", 0L, 65535L },
         new object[] { "WebServicesHttpsPort", 0L, 65535L },
         new object[] { "OtelMetricsInterval", 5L, 3600L },              // OtelMetricsExporter's clamps
         new object[] { "UpdateCheckHours", 1L, 168L },
         new object[] { "UpdateServiceWaitSeconds", 5L, 86400L },
         new object[] { "DNSQueryTimeout", 0L, 86400L },                 // DNSResolverWinApi's day
         new object[] { "PeriodHours", 1L, 168L },                       // RateLimiter's kMinPeriodHours, kMaxPeriodHours
         new object[] { "StateSaveIntervalSeconds", 1L, 3600L },
         new object[] { "LoadHeaderReadSize", 64L, 50000L },             // PersistentMessage's buffer and magic
         new object[] { "LoadBodyReadSize", 64L, 50000L },
         new object[] { "OAuth2JwksCacheSeconds", 10L, (long)int.MaxValue }, // JwksKeySet's floor
         new object[] { "PasswordHashIterations", 10000L, 10000000L },    // ReadPasswordHashWorkFactor_
         new object[] { "PasswordHashMemoryKB", 4096L, 1048576L },
         new object[] { "PasswordHashTimeCost", 1L, 20L },
         new object[] { "MtaStsPolicyMaxAge", 86400L, 31557600L },       // WebServicesServer's clamps, RFC 8461
         new object[] { "SMTPDMaxSizeDrop", 0L, (long)int.MaxValue }
      };

      [Theory]
      [MemberData(nameof(Ranges))]
      public void EveryNumericKey_IsANumberSetting_WithTheServersRange(string key, long minimum, long maximum)
      {
         string definition = Definition_(key);
         Assert.StartsWith("new NumberSetting", definition, StringComparison.Ordinal);
         Assert.Contains("Minimum = " + minimum.ToString(CultureInfo.InvariantCulture), definition, StringComparison.Ordinal);
         if (maximum < int.MaxValue)
            Assert.Contains("Maximum = " + maximum.ToString(CultureInfo.InvariantCulture), definition, StringComparison.Ordinal);
         else
            Assert.DoesNotContain("Maximum =", definition, StringComparison.Ordinal);
      }

      [Fact]
      public void NoKeyThePagesReadAsANumber_IsLeftAFreeTextBox()
      {
         string source = Source_();
         foreach (Match text in Regex.Matches(source, @"new TextSetting\s*\{[^}]*?Default = ""(\d+)"""))
            Assert.Fail("A text box still holds a number: " + text.Value);
      }

      [Fact]
      public void TheWorkFactors_TakeZero_ForTheBuiltInValue()
      {
         foreach (string key in new[] { "PasswordHashIterations", "PasswordHashMemoryKB", "PasswordHashTimeCost" })
            Assert.Contains("ZeroIsBuiltIn = true", Definition_(key), StringComparison.Ordinal);
      }

      [Fact]
      public void TheMtaStsMode_IsAListOfTheServersThreeWords()
      {
         string definition = Definition_("MtaStsPolicyMode");
         Assert.StartsWith("new WordSetting", definition, StringComparison.Ordinal);
         foreach (string word in new[] { "(\"enforce\",", "(\"testing\",", "(\"none\"," })
            Assert.Contains(word, definition, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDnsOverride_IsHeldToItsRule_AndTheAddressesAreAddressWide()
      {
         Assert.Contains("Rule = SettingAddressRule.DnsOverride", Definition_("DNSServer"), StringComparison.Ordinal);
         foreach (string key in new[] { "DNSServer", "RestApiBindAddress", "MetricsServerBindAddress", "ManageSieveServerBindAddress", "WebServicesBindAddress", "AuthUserReplacementIP" })
            Assert.Contains("Address = true", Definition_(key), StringComparison.Ordinal);
      }

      [Fact]
      public void TheDropSize_IsCaptionedInTheUnitTheServerReads()
      {
         // SMTPConnection compares it with the message's size in KB.
         Assert.Contains("above N KB mid-transfer", Definition_("SMTPDMaxSizeDrop"), StringComparison.Ordinal);
      }

      [Fact]
      public void ASave_AsksEveryRowBeforeItWritesAny_AndTheNumbersAreTheServerSettingsWidth()
      {
         string source = Source_();
         string save = WithheldRowTests.MemberText(source, "private bool Save_()");
         Assert.True(save != null, "FeatureSettingsView.Save_ is gone.");
         int asks = save.IndexOf(".Where(setting => !setting.Validate())", StringComparison.Ordinal);
         int writes = save.IndexOf("setting.Save(store_);", StringComparison.Ordinal);
         Assert.True(asks > 0 && writes > asks, "Save_ no longer holds every row to its range before it writes.");
         Assert.Contains("DialogFields.ShowError(refused[0].Field, refused[0].Field.Error);", save, StringComparison.Ordinal);

         Assert.Contains("protected const double NumberWidth = 180;", source, StringComparison.Ordinal);
         Assert.DoesNotContain("EditorWidth", source, StringComparison.Ordinal);

         // The box sets no bounds of its own, which would pull a typed value to the nearest one without a word.
         string number = source[source.IndexOf("private class NumberSetting", StringComparison.Ordinal)..source.IndexOf("private class WordSetting", StringComparison.Ordinal)];
         Assert.DoesNotContain("box_.Minimum", number, StringComparison.Ordinal);
         Assert.DoesNotContain("Minimum = Minimum", number, StringComparison.Ordinal);
      }

      private static string Definition_(string key)
      {
         string source = Source_();
         Match m = Regex.Match(source, @"new \w+Setting\s*\{\s*Key = """ + Regex.Escape(key) + @"""");
         Assert.True(m.Success, key + " is not defined on the feature settings pages.");
         int depth = 0;
         int end = source.IndexOf('{', m.Index);
         for (; end < source.Length; end++)
         {
            if (source[end] == '{') depth++;
            else if (source[end] == '}' && --depth == 0) break;
         }
         return source[m.Index..(end + 1)];
      }

      private static string Source_()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "FeatureSettingsView.xaml.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("FeatureSettingsView.xaml.cs could not be found from the test binaries.");
      }
   }
}
