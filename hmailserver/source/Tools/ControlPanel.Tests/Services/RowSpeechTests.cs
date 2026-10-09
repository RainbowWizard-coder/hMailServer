// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What a screen reader says for a list's row (review findings F070, F285): a
   /// row class without a ToString of its own is announced by its class name, and
   /// a row of bare values says nothing without its columns. Services/RowSpeech is
   /// the sentence; the pages' row classes call it, which their source is read for.
   /// </summary>
   public class RowSpeechTests
   {
      [Fact]
      public void ARow_IsItsCellsThatHaveSomething_JoinedWithCommas()
      {
         Assert.Equal("serveradmin, Everything, on every domain", RowSpeech.Of("serveradmin", "Everything", null, "  ", "on every domain"));
         Assert.Equal("", RowSpeech.Of());
         Assert.Equal("", RowSpeech.Of(null, ""));
      }

      [Fact]
      public void ALabelledCell_SaysItsColumn_AndAnEmptyOneSaysNothing()
      {
         Assert.Equal("Active: Yes", RowSpeech.Labelled("Active", "Yes"));
         Assert.Equal("Yes", RowSpeech.Labelled("", "Yes"));
         Assert.Equal("", RowSpeech.Labelled("Score", ""));
         Assert.Equal("zen.spamhaus.org, Active: Yes, Score: 3", RowSpeech.Of("zen.spamhaus.org", RowSpeech.Labelled("Active", "Yes"), RowSpeech.Labelled("Score", "3")));
      }

      /// <summary>
      /// The administrator roles page: each card is titled and each grid named for
      /// what it holds, and each row says its role, or its account, role and
      /// domains (review finding F070).
      /// </summary>
      [Fact]
      public void TheRolesPage_NamesItsCardsGridsAndRows()
      {
         string page = Read(Path.Join("Views", "RolesView.cs"));

         Assert.Contains("new Card { Title = L(\"Roles\"), Content = roleGrid_ }", page, StringComparison.Ordinal);
         Assert.Contains("new Card { Title = L(\"Who holds a role\"), Content = held", page, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(roleGrid_, L(\"Roles\"));", page, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(assignmentGrid_, L(\"Who holds a role\"));", page, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"class RoleRow[\s\S]{0,900}?public override string ToString\(\) => RowSpeech\.Of\(Name, Purpose\);"), page);
         Assert.Matches(new Regex(@"class AssignmentRow[\s\S]{0,500}?public override string ToString\(\) => RowSpeech\.Of\(Address, Role, Domains\);"), page);
      }

      internal static string Read(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
