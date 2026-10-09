// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Performance page's Database tab does not offer a setting the server
   /// ignores, and its live figures say when they could not be read. On the
   /// built-in database the pool size was a live box under the caption
   /// "Connections in the pool (ignored: this server uses the built-in database)"
   /// with a note that shouted "THIS SERVER USES THE BUILT-IN DATABASE"; and a
   /// cache or message-store figure that failed was drawn as the value, "This could
   /// not be read from the server: ...", at the value's size and with a JSON
   /// parser's message in it.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class PerformancePageTests
   {
      [Fact]
      public void ThePoolSize_IsNotOffered_OnTheBuiltInDatabase_AndANoticeSaysWhy()
      {
         string source = Source();
         Assert.Contains("Ignored = builtInDatabase,", source, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Connections in the pool\"),", source, StringComparison.Ordinal);
         Assert.DoesNotContain("(ignored: this server uses the built-in database)", source, StringComparison.Ordinal);
         Assert.DoesNotContain("THIS SERVER USES THE BUILT-IN DATABASE", source, StringComparison.Ordinal);
         Assert.DoesNotContain("Where that database IS", source, StringComparison.Ordinal);
         Assert.Contains("L(\"This server uses the built-in database (SQL Server Compact), which always uses one connection; the pool size applies after a move to MySQL, SQL Server or PostgreSQL.\")", source, StringComparison.Ordinal);
         Assert.Contains("IgnoredBecause = builtInDatabase ? poolIgnored : null,", source, StringComparison.Ordinal);

         int number = source.IndexOf("private class IniNumber : ComSetting, IIniSetting", StringComparison.Ordinal);
         int section = source.IndexOf("private class SectionIniNumber : IniNumber", StringComparison.Ordinal);
         Assert.True(number >= 0 && section > number, "IniNumber is gone.");
         string build = WithheldRowTests.MemberText(source.Substring(number, section - number), "protected override FrameworkElement BuildEditor(object value)");
         Assert.True(build != null, "IniNumber.BuildEditor is gone.");
         Assert.Contains("if (Ignored)", build, StringComparison.Ordinal);

         // Read-only, not disabled, as ComInert's box: a disabled box is skipped
         // by the keyboard and, in most screen readers, not reachable at all.
         Assert.Contains("number_.IsReadOnly = true;", build, StringComparison.Ordinal);
         Assert.DoesNotContain("IsEnabled = false", build, StringComparison.Ordinal);
         Assert.Contains("number_.SpinButtonPlacementMode = Wpf.Ui.Controls.NumberBoxSpinButtonPlacementMode.Hidden;", build, StringComparison.Ordinal);
         Assert.Contains("number_.ClearButtonEnabled = false;", build, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetHelpText(number_, IgnoredBecause);", build, StringComparison.Ordinal);

         // And a save writes nothing of it.
         string save = WithheldRowTests.MemberText(source.Substring(number, section - number), "public bool SaveToIni()");
         Assert.True(save != null, "IniNumber.SaveToIni is gone.");
         Assert.Contains("if (Ignored ||", save, StringComparison.Ordinal);
      }

      [Fact]
      public void AFigureThatCouldNotBeRead_IsAWarningNotice_NotAValue()
      {
         string source = Source();
         int stat = source.IndexOf("private class ComStat", StringComparison.Ordinal);
         Assert.True(stat >= 0, "ComStat is gone.");
         string body = source.Substring(stat, source.IndexOf("private class ComAction", stat, StringComparison.Ordinal) - stat);

         Assert.Contains("new Scaffold.InlineNotice", body, StringComparison.Ordinal);
         Assert.Contains("Level = StatusLevel.Warning,", body, StringComparison.Ordinal);
         Assert.Contains("L(\"The server did not report this figure.\")", body, StringComparison.Ordinal);
         Assert.Contains("ex is System.Text.Json.JsonException || ex is KeyNotFoundException", body, StringComparison.Ordinal);
      }

      private static string Source()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ServerSettingsView.xaml.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/ServerSettingsView.xaml.cs could not be found from the test binaries.");
         return null;
      }
   }
}
