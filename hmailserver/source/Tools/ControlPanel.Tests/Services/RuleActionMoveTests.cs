// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Move up and Move down of a rule's action asked RuleAction.Save of the action after
   /// the move had stored its place, and then Rule.Save. RuleAction.Save holds the action
   /// it saves to the save's checks whether it changes it or not, so an action they refuse,
   /// stored before them, restored from a backup or written to the table - a Run function
   /// name that is not plain, a domain's or an account's Reply from an address it may not
   /// send as or Set header value of From - was moved and told of as not moved ("Could not
   /// move the action"), moved again at each click, and the rule's save, which asks for
   /// the owner's rules to be read again, was never reached. The move saves the rule
   /// alone, which leaves out of its checks each action it writes as stored; the server's
   /// side of it is Rules/Rules.cs's AStoredActionTheSaveRefusesIsMovedAsTheControlPanelMovesIt.
   /// The test fails against that page.
   /// </summary>
   public class RuleActionMoveTests
   {
      [Fact]
      public void MovingAnAction_SavesTheRule_AndNotTheActionTheMoveStored()
      {
         string move = WithheldRowTests.MemberText(ReadRulesView(), "private void MoveAction(bool up)");
         Assert.True(move != null, "RulesView.MoveAction is gone.");

         // The calls alone, the comments taken out.
         string code = string.Join("\n", move.Split('\n').Select(line =>
         {
            int comment = line.IndexOf("//", StringComparison.Ordinal);
            return comment < 0 ? line : line.Substring(0, comment);
         }));

         int moves = code.IndexOf("if (up) a.MoveUp(); else a.MoveDown();", StringComparison.Ordinal);
         Assert.True(moves >= 0, "MoveAction no longer moves the action with RuleAction.MoveUp and MoveDown, which store its place.");

         int saves = code.IndexOf("rule.Save();", moves, StringComparison.Ordinal);
         Assert.True(saves > moves, "MoveAction does not save the rule after the move, so the owner's rules are not read again and delivery keeps the order they had.");

         // The rule's save is the one save: RuleAction.Save after the move refused an
         // action stored before the save's checks, whose place the move had stored.
         int first = code.IndexOf(".Save()", StringComparison.Ordinal);
         int last = code.LastIndexOf(".Save()", StringComparison.Ordinal);
         Assert.True(first == last && first == saves + "rule".Length,
            "MoveAction saves something besides the rule after the move: " + move);
      }

      private static string ReadRulesView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "RulesView.xaml.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/RulesView.xaml.cs could not be found from the test binaries.");
         return null;
      }
   }
}
