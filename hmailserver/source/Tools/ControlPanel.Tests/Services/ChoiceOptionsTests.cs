// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A settings list shows the value the server holds, even one that is none of
   /// its choices, so that a save of the page - which writes every row - writes
   /// that value back instead of the first choice.
   /// </summary>
   public class ChoiceOptionsTests
   {
      private static readonly (int Value, string Label)[] Security =
      {
         (0, "None"), (1, "SSL/TLS"), (2, "STARTTLS (optional)"), (3, "STARTTLS (required)")
      };

      [Fact]
      public void AKnownValue_OffersTheChoicesAsTheyAre()
      {
         var offered = ChoiceOptions.WithUnknown(Security, 2);

         Assert.Equal(Security, offered.ToArray());
      }

      [Fact]
      public void AnUnknownValue_IsOfferedAfterTheChoices_UnderItsOwnNumber()
      {
         var offered = ChoiceOptions.WithUnknown(Security, 7);

         Assert.Equal(Security.Length + 1, offered.Count);
         Assert.Equal(Security, offered.Take(Security.Length).ToArray());
         Assert.Equal(7, offered[^1].Value);
         Assert.Equal(ChoiceOptions.UnknownLabel("7"), offered[^1].Label);
         Assert.StartsWith("7 ", offered[^1].Label);
      }

      [Fact]
      public void AnUnknownWord_IsNamedByTheWordTheServerAnswered()
      {
         var offered = ChoiceOptions.WithUnknown(Security, int.MinValue, "quarantine");

         Assert.Equal(int.MinValue, offered[^1].Value);
         Assert.StartsWith("quarantine ", offered[^1].Label);
      }

      [Fact]
      public void TheLabel_SaysTheValueIsNotOneThisProgramKnows()
      {
         Assert.Equal("5 (a value this Control Panel does not know)", ChoiceOptions.UnknownLabel("5"));
      }

      [Fact]
      public void NoChoicesAtAll_StillOffersTheValue()
      {
         var offered = ChoiceOptions.WithUnknown(null, 1);

         Assert.Single(offered);
         Assert.Equal(1, offered[0].Value);
      }
   }
}
