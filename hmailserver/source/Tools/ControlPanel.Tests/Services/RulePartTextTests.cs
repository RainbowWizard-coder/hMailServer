// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Rules page writes each action as its verb and what it acts on, each
   /// criterion as its field, comparison and quoted value, and each rule's summary
   /// as its first criterion and first action (review finding F309), where the
   /// actions read "Forward e-mail -> someone@example.com", "Move to IMAP folder
   /// 'Invoices'" and "Set header value X-Rule=" - three ways in one list - and the
   /// grid of rules said only their number, name and whether they were on.
   /// </summary>
   public class RulePartTextTests
   {
      [Theory]
      [InlineData(RulePartText.Forward, "Forward to someone@example.com")]
      [InlineData(RulePartText.Reply, "Reply with the subject 'Out of office'")]
      [InlineData(RulePartText.MoveToFolder, "Move to folder 'Invoices'")]
      [InlineData(RulePartText.RunFunction, "Run the script function OnRule")]
      [InlineData(RulePartText.SendUsingRoute, "Send using route #3")]
      [InlineData(RulePartText.BindToAddress, "Bind to address 192.0.2.10")]
      public void AnActionIsItsVerbAndWhatItActsOn(int type, string expected)
      {
         string text = RulePartText.Action(type, "name", to: "someone@example.com", subject: "Out of office", folder: "Invoices",
            function: "OnRule", value: "192.0.2.10", route: 3);
         Assert.Equal(expected, text);
         Assert.DoesNotContain("->", text, StringComparison.Ordinal);
      }

      [Fact]
      public void AHeaderIsSetToAValueOrToEmpty()
      {
         Assert.Equal("Set header X-Rule to (empty)", RulePartText.Action(RulePartText.SetHeader, "Set header value", header: "X-Rule", value: ""));
         Assert.Equal("Set header X-Rule to 'yes'", RulePartText.Action(RulePartText.SetHeader, "Set header value", header: "X-Rule", value: "yes"));
      }

      [Fact]
      public void AnActionThatActsOnNothingNamedIsItsName()
      {
         Assert.Equal("Delete e-mail", RulePartText.Action(1, "Delete e-mail"));
         Assert.Equal("Stop rule processing", RulePartText.Action(6, "Stop rule processing"));
      }

      [Fact]
      public void ACriterionAndASummary()
      {
         string criterion = RulePartText.Criterion("From", "contains", "invoice");
         Assert.Equal("From contains 'invoice'", criterion);
         Assert.Equal("From contains 'invoice' → Forward to someone@example.com", RulePartText.Summary(criterion, "Forward to someone@example.com"));
         Assert.Equal("Delete e-mail", RulePartText.Summary(null, "Delete e-mail"));
         Assert.Equal(criterion, RulePartText.Summary(criterion, ""));
         Assert.Equal("", RulePartText.Summary(null, null));
      }

      [Fact]
      public void ThePageWritesItsPartsThroughIt_AndShowsTheSummary()
      {
         string code = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "RulesView.xaml.cs"));
         Assert.Contains("RulePartText.Criterion(field, Pick(MatchNames, (int)c.MatchType), (string)c.MatchValue)", code, StringComparison.Ordinal);
         Assert.Contains("Summary = Summarise_(rule)", code, StringComparison.Ordinal);
         Assert.DoesNotContain("\" -> \"", code, StringComparison.Ordinal);
         Assert.DoesNotContain("+ \"=\" +", code, StringComparison.Ordinal);

         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "RulesView.xaml"));
         Assert.Contains("Header=\"{loc:L 'Summary'}\" Binding=\"{Binding Summary}\"", page, StringComparison.Ordinal);
         // The criteria's card is titled IF, as the actions' is THEN, and the match
         // mode is its first row under a caption that is the list's label.
         Assert.Contains("<scaffold:Card x:Name=\"CriteriaCard\" Title=\"{loc:L 'IF'}\"", page, StringComparison.Ordinal);
         Assert.Contains("<scaffold:Card x:Name=\"ActionsCard\" Title=\"{loc:L 'THEN'}\"", page, StringComparison.Ordinal);
         Assert.Contains("Content=\"{loc:L 'Matc_h'}\" Target=\"{Binding ElementName=MatchMode}\"", page, StringComparison.Ordinal);
      }
   }
}
