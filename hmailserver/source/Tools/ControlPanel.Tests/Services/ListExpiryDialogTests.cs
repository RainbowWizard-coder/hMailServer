// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The allow and block lists' expiry in the generated Add and Edit dialog
   /// (Views/FieldDialog). The date's box opened on the reader's general form of the
   /// date the server holds - "12/31/2026 12:00:00 AM", or "12/30/1899 12:00:00 AM" for
   /// an entry that never expires - under a caption that asks for yyyy-mm-dd hh:mm:ss,
   /// and the dialog sent the box whether or not "Stops mattering at a date" was ticked;
   /// the server reads only its own form and refuses an empty one, so that no entry that
   /// never expires could be added and none could be saved without its date typed again.
   /// </summary>
   public class ListExpiryDialogTests : IDisposable
   {
      private readonly CultureInfo culture_ = CultureInfo.CurrentCulture;

      public void Dispose() => CultureInfo.CurrentCulture = culture_;

      [Theory]
      [InlineData("en-US")]
      [InlineData("en-GB")]
      [InlineData("de-DE")]
      [InlineData("ja-JP")]
      public void ADate_OpensInTheServersOwnForm_WhateverTheLanguage(string language)
      {
         CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo(language);
         Assert.Equal("2026-12-31 00:00:00", FieldDialogRules.BoxText(new DateTime(2026, 12, 31, 0, 0, 0)));
         Assert.Equal("2027-01-07 10:00:05", FieldDialogRules.BoxText(new DateTime(2027, 1, 7, 10, 0, 5)));
      }

      [Fact]
      public void TheServersNoDate_OpensEmpty_AndAnythingElseAsItWas()
      {
         CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("en-GB");
         Assert.Equal("", FieldDialogRules.BoxText(new DateTime(1899, 12, 30)));
         Assert.Equal("", FieldDialogRules.BoxText(null));
         Assert.Equal("Reported phishing", FieldDialogRules.BoxText("Reported phishing"));
         Assert.Equal("100", FieldDialogRules.BoxText(100));
      }

      [Fact]
      public void TheDate_IsNotSent_WhileItsSwitchIsOff()
      {
         var values = new Dictionary<string, object> { ["Value"] = "partner.example.net", ["Expires"] = false, ["ExpiresTime"] = "" };
         FieldDialogRules.DropWhereSwitchedOff(values, new[] { ("ExpiresTime", "Expires") });
         Assert.False(values.ContainsKey("ExpiresTime"));
         Assert.Equal(false, values["Expires"]);
         Assert.Equal("partner.example.net", values["Value"]);
      }

      [Fact]
      public void TheDate_IsSent_WhileItsSwitchIsOn_EvenEmpty_ForTheServerToRefuseOnItsField()
      {
         var on = new Dictionary<string, object> { ["Expires"] = true, ["ExpiresTime"] = "2026-12-31 00:00:00" };
         FieldDialogRules.DropWhereSwitchedOff(on, new[] { ("ExpiresTime", "Expires") });
         Assert.Equal("2026-12-31 00:00:00", on["ExpiresTime"]);

         var empty = new Dictionary<string, object> { ["Expires"] = true, ["ExpiresTime"] = "" };
         FieldDialogRules.DropWhereSwitchedOff(empty, new[] { ("ExpiresTime", "Expires") });
         Assert.Equal("", empty["ExpiresTime"]);

         // A dialog without the switch sends the value as it is.
         var none = new Dictionary<string, object> { ["ExpiresTime"] = "2026-12-31 00:00:00" };
         FieldDialogRules.DropWhereSwitchedOff(none, new[] { ("ExpiresTime", "Expires") });
         Assert.True(none.ContainsKey("ExpiresTime"));
      }

      [Fact]
      public void TheDialog_OpensItsBoxesThroughBoxText_AndLeavesTheSwitchedOffOut_BeforeTheWrite()
      {
         string dialog = HostedAddressTests.ReadControlPanelFile(Path.Join("Views", "FieldDialog.cs"));
         Assert.Contains("var box = new TextBox { Text = FieldDialogRules.BoxText(current) };", dialog, StringComparison.Ordinal);

         int dropped = dialog.IndexOf("FieldDialogRules.DropWhereSwitchedOff(Result, fields.Where(f => f.GridOnlyWhen != null).Select(f => (f.Prop, f.GridOnlyWhen)));", StringComparison.Ordinal);
         int written = dialog.IndexOf("Refusal refusal = save_?.Invoke(Result);", StringComparison.Ordinal);
         Assert.True(dropped >= 0, "FieldDialog sends a value whose switch is off.");
         Assert.True(written > dropped, "FieldDialog writes the values before it leaves out those whose switch is off.");
      }
   }
}
