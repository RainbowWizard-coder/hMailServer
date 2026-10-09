// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A rule's criterion saved with an empty header name or an empty value, and the
   /// message size and the delivery attempt opened on "contains" with no unit (review
   /// finding F227). These hold what the criterion dialog asks of its fields, the
   /// comparisons a number offers, and that the dialog asks it on the field before
   /// it writes anything.
   /// </summary>
   public class RuleCriterionInputTests
   {
      private const int From = 1;

      [Fact]
      public void ACustomHeader_NeedsAName_AndAPredefinedFieldDoesNot()
      {
         Assert.NotNull(RuleCriterionInput.HeaderError(RuleCriterionInput.CustomHeader, ""));
         Assert.NotNull(RuleCriterionInput.HeaderError(RuleCriterionInput.CustomHeader, "   "));
         Assert.Null(RuleCriterionInput.HeaderError(RuleCriterionInput.CustomHeader, "X-Spam-Status"));
         Assert.Null(RuleCriterionInput.HeaderError(From, ""));
      }

      [Theory]
      [InlineData(RuleCriterionInput.MatchContains, true)]
      [InlineData(RuleCriterionInput.MatchNotContains, true)]
      [InlineData(RuleCriterionInput.MatchRegex, true)]
      [InlineData(RuleCriterionInput.MatchWildcard, true)]
      [InlineData(RuleCriterionInput.MatchEquals, false)]    // the field is empty
      [InlineData(RuleCriterionInput.MatchNotEquals, false)] // the field is not empty
      public void AnEmptyValue_IsRefused_UnlessTheComparisonAsksWhetherTheFieldIsEmpty(int match, bool refused)
      {
         Assert.Equal(refused, RuleCriterionInput.ValueError(From, match, "") != null);
         Assert.Null(RuleCriterionInput.ValueError(From, match, "invoice"));
      }

      [Theory]
      [InlineData(RuleCriterionInput.MessageSize)]
      [InlineData(RuleCriterionInput.DeliveryAttempts)]
      public void ANumbersField_OpensOnGreaterThan_AndOffersANumbersFourComparisons(int field)
      {
         Assert.True(RuleCriterionInput.IsNumeric(field));
         Assert.Equal(RuleCriterionInput.MatchGreaterThan, RuleCriterionInput.DefaultMatch(field));
         Assert.Equal(new[] { RuleCriterionInput.MatchGreaterThan, RuleCriterionInput.MatchLessThan, RuleCriterionInput.MatchEquals, RuleCriterionInput.MatchNotEquals },
            RuleCriterionInput.MatchesFor(field).ToArray());
      }

      [Fact]
      public void AFieldOfText_OpensOnContains_AndOffersAllEight_InTheirOrder()
      {
         Assert.False(RuleCriterionInput.IsNumeric(From));
         Assert.Equal(RuleCriterionInput.MatchContains, RuleCriterionInput.DefaultMatch(From));
         Assert.Equal(new[] { 1, 2, 3, 4, 5, 6, 7, 8 }, RuleCriterionInput.MatchesFor(From).ToArray());
      }

      [Fact]
      public void ACriterionSavedWithAComparisonOfText_KeepsItInTheList_SoThatOpeningItChangesNothing()
      {
         var offered = RuleCriterionInput.MatchesFor(RuleCriterionInput.MessageSize, RuleCriterionInput.MatchContains);
         Assert.Contains(RuleCriterionInput.MatchContains, offered);
         Assert.Equal(5, offered.Count);

         // A number's own comparison is not listed twice, and an unknown one is not listed at all.
         Assert.Equal(4, RuleCriterionInput.MatchesFor(RuleCriterionInput.MessageSize, RuleCriterionInput.MatchLessThan).Count);
         Assert.Equal(4, RuleCriterionInput.MatchesFor(RuleCriterionInput.MessageSize, 99).Count);
      }

      [Theory]
      [InlineData("1048576", true)]
      [InlineData(" 3 ", true)]
      [InlineData("0", true)]
      [InlineData("", false)]
      [InlineData("1 MB", false)]
      [InlineData("-5", false)]
      [InlineData("1.5", false)]
      [InlineData("1,000", false)]
      public void ANumberComparedAsOne_TakesAWholeNumber(string value, bool accepted)
      {
         foreach (int field in new[] { RuleCriterionInput.MessageSize, RuleCriterionInput.DeliveryAttempts })
            foreach (int match in RuleCriterionInput.MatchesFor(field))
               Assert.Equal(accepted, RuleCriterionInput.ValueError(field, match, value) == null);
      }

      [Fact]
      public void ANumber_IsStoredWithoutTheSpacesAroundIt_AndTextAsTyped()
      {
         Assert.Equal("3", RuleCriterionInput.StoredValue(RuleCriterionInput.DeliveryAttempts, RuleCriterionInput.MatchEquals, " 3 "));
         Assert.Equal(" invoice ", RuleCriterionInput.StoredValue(From, RuleCriterionInput.MatchContains, " invoice "));
      }

      [Fact]
      public void ANumbersCaption_CarriesItsUnit_AndKeepsTheValuesAccessKey()
      {
         Assert.Equal("_Value (bytes)", RuleCriterionInput.ValueCaption(RuleCriterionInput.MessageSize));
         Assert.Equal("_Value (attempts)", RuleCriterionInput.ValueCaption(RuleCriterionInput.DeliveryAttempts));
         Assert.Equal("_Value", RuleCriterionInput.ValueCaption(From));
         Assert.NotNull(RuleCriterionInput.ValueHint(RuleCriterionInput.MessageSize));
         Assert.NotNull(RuleCriterionInput.ValueHint(RuleCriterionInput.DeliveryAttempts));
         Assert.Null(RuleCriterionInput.ValueHint(From));
      }

      [Fact]
      public void TheDescription_NamesTheRule_AndIsNoneWithoutAName()
      {
         Assert.Contains("Invoices", RuleCriterionInput.Description("Invoices", adding: true), StringComparison.Ordinal);
         Assert.Contains("Invoices", RuleCriterionInput.Description("Invoices", adding: false), StringComparison.Ordinal);
         Assert.Contains("Invoices", RuleCriterionInput.ActionDescription("Invoices", adding: true), StringComparison.Ordinal);
         Assert.Contains("Invoices", RuleCriterionInput.ActionDescription("Invoices", adding: false), StringComparison.Ordinal);
         Assert.Null(RuleCriterionInput.Description(null, adding: true));
         Assert.Null(RuleCriterionInput.ActionDescription(" ", adding: false));
      }

      [Fact]
      public void TheDialog_AsksOnTheField_BeforeItWritesAnything()
      {
         string source = File.ReadAllText(Path.Join(ControlPanelDirectory_(), "Views", "RuleCriteriaDialog.cs"));
         string save = source[source.IndexOf("private void Save()", StringComparison.Ordinal)..];

         int header = save.IndexOf("DialogFields.ShowError(headerRow_", StringComparison.Ordinal);
         int value = save.IndexOf("DialogFields.ShowError(valueRow_", StringComparison.Ordinal);
         int write = save.IndexOf("c.MatchValue =", StringComparison.Ordinal);
         Assert.True(header > 0 && value > header && write > value, "the header name and the value are checked on their fields before the criterion is written");
         Assert.Contains("RuleCriterionInput.StoredValue(", save, StringComparison.Ordinal);
         Assert.Contains("RuleCriterionInput.MatchesFor(", source, StringComparison.Ordinal);
         Assert.Contains("frame_.Description = RuleCriterionInput.Description(", source, StringComparison.Ordinal);

         string action = File.ReadAllText(Path.Join(ControlPanelDirectory_(), "Views", "RuleActionDialog.cs"));
         Assert.Contains("frame_.Description = RuleCriterionInput.ActionDescription(", action, StringComparison.Ordinal);
      }

      private static string ControlPanelDirectory_()
      {
         for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
         {
            string candidate = Path.Join(dir.FullName, "ControlPanel", "Views");
            if (Directory.Exists(candidate))
               return Path.Join(dir.FullName, "ControlPanel");
         }
         throw new DirectoryNotFoundException("ControlPanel source directory not found");
      }
   }
}
