// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The quarantine digest settings dialog writes with itself open, and a refusal
   /// leaves the server holding what it held (review finding F209). It wrote the
   /// address and then the hours, unchecked, so that hours of 0 or 1000 - the
   /// number box has no range - were refused on their field after the new address
   /// had been stored, and Cancel closed the dialog over an address that had
   /// changed with nothing said.
   /// </summary>
   public class QuarantineDigestSettingsWriteTests
   {
      [Theory]
      [InlineData(0)]
      [InlineData(-5)]
      [InlineData(721)]
      [InlineData(1000)]
      public void HoursOutsideTheServersBounds_AreRefused_BeforeAnythingIsWritten(int hours)
      {
         var server = new Server_("https://mail.example.com", 24);
         QuarantineDigestSettingsWrite.Outcome outcome = server.Write("https://quarantine.example.org", hours, out Exception error);

         Assert.Equal(QuarantineDigestSettingsWrite.Outcome.LinkHoursOutOfRange, outcome);
         Assert.Null(error);
         Assert.Empty(server.Writes);
         Assert.Equal("https://mail.example.com", server.Address);
      }

      [Theory]
      [InlineData(1)]
      [InlineData(24)]
      [InlineData(720)]
      public void HoursWithinTheBounds_AreWritten_AfterTheAddress(int hours)
      {
         var server = new Server_("https://mail.example.com", 24);
         QuarantineDigestSettingsWrite.Outcome outcome = server.Write("https://quarantine.example.org", hours, out Exception error);

         Assert.Equal(QuarantineDigestSettingsWrite.Outcome.Saved, outcome);
         Assert.Null(error);
         Assert.Equal(new[] { "address=https://quarantine.example.org", "hours=" + hours }, server.Writes);
         Assert.Equal(hours, server.Hours);
      }

      [Fact]
      public void TheBounds_AreTheServers()
      {
         // InterfaceSettings::put_QuarantineDigestLinkHours: "A release link lives
         // between 1 and 720 hours." for newVal < 1 || newVal > 720.
         Assert.Equal(1, QuarantineDigestSettingsWrite.MinLinkHours);
         Assert.Equal(720, QuarantineDigestSettingsWrite.MaxLinkHours);
         string server = File.ReadAllText(RepoFile("hmailserver", "source", "Server", "COM", "InterfaceSettings.cpp"));
         Assert.Contains("if (newVal < 1 || newVal > 720)\n         return COMError::GenerateError(\"A release link lives between 1 and 720 hours.\");",
            server.Replace("\r\n", "\n"), StringComparison.Ordinal);
      }

      [Fact]
      public void AnAddressTheServerRefuses_WritesNothing()
      {
         var server = new Server_("https://mail.example.com", 24) { RefuseAddress = "quarantine" };
         QuarantineDigestSettingsWrite.Outcome outcome = server.Write("quarantine", 48, out Exception error);

         Assert.Equal(QuarantineDigestSettingsWrite.Outcome.AddressRefused, outcome);
         Assert.IsType<COMException>(error);
         Assert.Equal("https://mail.example.com", server.Address);
         Assert.Equal(24, server.Hours);
      }

      [Fact]
      public void HoursTheServerRefusesAfterTakingTheAddress_PutTheAddressBack()
      {
         var server = new Server_("https://mail.example.com", 24) { RefuseHours = true };
         QuarantineDigestSettingsWrite.Outcome outcome = server.Write("https://quarantine.example.org", 48, out Exception error);

         Assert.Equal(QuarantineDigestSettingsWrite.Outcome.LinkHoursRefused, outcome);
         Assert.IsType<COMException>(error);
         Assert.Equal(new[] { "address=https://quarantine.example.org", "address=https://mail.example.com" }, server.Writes);
         Assert.Equal("https://mail.example.com", server.Address);
         Assert.Equal(24, server.Hours);
      }

      [Fact]
      public void HoursRefusedWithTheAddressUnchanged_WriteTheAddressOnce()
      {
         var server = new Server_("https://mail.example.com", 24) { RefuseHours = true };
         QuarantineDigestSettingsWrite.Outcome outcome = server.Write("https://mail.example.com", 48, out _);

         Assert.Equal(QuarantineDigestSettingsWrite.Outcome.LinkHoursRefused, outcome);
         Assert.Equal(new[] { "address=https://mail.example.com" }, server.Writes);
      }

      [Fact]
      public void TheDigestDialog_SavesThroughTheWrite_AndSaysTheRangeOnTheHours()
      {
         string quarantine = FormLineAssert.ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));
         string digest = WithheldRowTests.MemberText(quarantine, "private void EditDigestSettings()");
         Assert.True(digest != null, "QuarantineView.EditDigestSettings is gone.");
         Assert.Contains("string addressBefore = (string)current.Values[\"QuarantineDigestBaseUrl\"];", digest, StringComparison.Ordinal);
         Assert.Contains("QuarantineDigestSettingsWrite.Write(", digest, StringComparison.Ordinal);
         Assert.Contains("new FieldDialog.Refusal(L(\"Enter a whole number of hours from 1 to 720.\"), \"QuarantineDigestLinkHours\")", digest, StringComparison.Ordinal);

         // Nothing written past the write: the callback sets no setting of its own.
         Assert.Single(System.Text.RegularExpressions.Regex.Matches(digest, @"settings\.QuarantineDigestBaseUrl = "));
         Assert.Single(System.Text.RegularExpressions.Regex.Matches(digest, @"settings\.QuarantineDigestLinkHours = "));
      }

      /// <summary>The server's two settings, as the dialog's writes reach them.</summary>
      private sealed class Server_
      {
         public Server_(string address, int hours)
         {
            Address = address;
            Hours = hours;
         }

         public string Address { get; private set; }
         public int Hours { get; private set; }
         public List<string> Writes { get; } = new();
         public string RefuseAddress { get; init; }
         public bool RefuseHours { get; init; }

         public QuarantineDigestSettingsWrite.Outcome Write(string address, int hours, out Exception error)
         {
            string before = Address;
            return QuarantineDigestSettingsWrite.Write(address, hours, before,
               value =>
               {
                  if (value == RefuseAddress)
                     throw new COMException("The address the release links point at must be an absolute http or https URL.");
                  Writes.Add("address=" + value);
                  Address = value;
               },
               value =>
               {
                  if (RefuseHours)
                     throw new COMException("The RPC server is unavailable.");
                  Writes.Add("hours=" + value);
                  Hours = value;
               },
               out error);
         }
      }

      private static string RepoFile(params string[] parts)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, Path.Join(parts));
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail(string.Join("/", parts) + " could not be found from the test binaries.");
         return null;
      }
   }
}
