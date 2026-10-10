// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;
using Folder = hMailServer.ControlPanel.Services.PublicFolderTree.Folder;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Public folders page shows the folders' tree and names the hierarchy
   /// delimiter, makes a name with the delimiter in it a sub-folder, and says what a
   /// delete takes with it (review finding F206). The page said "Use the hierarchy
   /// delimiter to create sub-folders" and listed Sales and Sales.Leads as two flat
   /// rows; the server's Add took "Sales.Leads" as one folder's name, which a mail
   /// client cannot open, since the server finds a folder by walking its path down
   /// from the top; and "Delete the public folder Sales and every message in it?" was
   /// asked while Sales.Leads, which goes with it, was listed below.
   /// </summary>
   [Collection("Loc")]
   public class PublicFolderTreeTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      private static IReadOnlyList<string> P(params string[] names) => names;

      [Fact]
      public void TheTree_ListsEachFolderUnderItsParent_ByName_WithItsDepthAndWhatIsUnderIt()
      {
         IReadOnlyList<Folder> folders = PublicFolderTree.Build(new[]
         {
            P("Support"), P("Sales"), P("Sales", "Leads"), P("Sales", "Archive"), P("Sales", "Leads", "Hot"), P("announcements")
         }, ".");

         Assert.Equal(new[] { "announcements", "Sales", "Sales.Archive", "Sales.Leads", "Sales.Leads.Hot", "Support" }, folders.Select(f => f.FullName));
         Assert.Equal(new[] { 0, 0, 1, 1, 2, 0 }, folders.Select(f => f.Depth));
         Assert.Equal(new[] { 0, 3, 0, 1, 0, 0 }, folders.Select(f => f.SubFolderCount));
         Assert.Equal("Hot", folders[4].Name);
         Assert.Equal("Sales.Leads.Hot", folders[4].ToString());
      }

      [Fact]
      public void TheTree_TakesTheServersDelimiter_AndItsDefaultWhereItSaysNone()
      {
         IReadOnlyList<Folder> slash = PublicFolderTree.Build(new[] { P("Sales"), P("Sales", "Leads") }, "/");
         Assert.Equal("Sales/Leads", slash[1].FullName);
         Assert.Equal(".", PublicFolderTree.DelimiterOf(""));
         Assert.Equal(".", PublicFolderTree.DelimiterOf(null));
         Assert.Equal("/", PublicFolderTree.DelimiterOf("/"));
      }

      /// <summary>A folder whose own name holds the delimiter - what the page's old advice made - is pointed out.</summary>
      [Fact]
      public void AFolderNamedWithTheDelimiter_IsPointedOut()
      {
         IReadOnlyList<Folder> folders = PublicFolderTree.Build(new[] { P("Sales"), P("Sales.Leads"), P("Support") }, ".");
         Assert.True(folders.Single(f => f.Name == "Sales.Leads").NameHoldsDelimiter);
         Assert.False(folders.Single(f => f.Name == "Sales").NameHoldsDelimiter);

         Assert.Equal("Sales.Leads has \".\" in its own name rather than being a sub-folder, so a mail client cannot open it. Add folder now makes such a name a sub-folder.",
            PublicFolderTree.MisnamedNotice(folders, "."));
         Assert.Null(PublicFolderTree.MisnamedNotice(PublicFolderTree.Build(new[] { P("Sales"), P("Sales", "Leads") }, "."), "."));

         IReadOnlyList<Folder> two = PublicFolderTree.Build(new[] { P("A.B"), P("C.D") }, ".");
         Assert.Equal("2 folders have \".\" in their own names rather than being sub-folders, so a mail client cannot open them: A.B, C.D. Add folder now makes such a name a sub-folder.",
            PublicFolderTree.MisnamedNotice(two, "."));
      }

      [Theory]
      [InlineData("Sales.Leads", new[] { "Sales", "Leads" })]
      [InlineData("  Sales . Leads ", new[] { "Sales", "Leads" })]
      [InlineData("Support", new[] { "Support" })]
      public void ATypedName_SplitsAtTheDelimiter(string typed, string[] path)
         => Assert.Equal(path, PublicFolderTree.Split(typed, "."));

      [Theory]
      [InlineData("")]
      [InlineData(".Sales")]
      [InlineData("Sales.")]
      [InlineData("Sales..Leads")]
      public void ANameWithAnEmptyPart_DoesNotSplit(string typed) => Assert.Null(PublicFolderTree.Split(typed, "."));

      [Fact]
      public void ANameThatCannotBeAFolder_IsSaidOnTheBox()
      {
         IReadOnlyList<Folder> existing = PublicFolderTree.Build(new[] { P("Sales"), P("Sales", "Leads") }, ".");

         Assert.Equal("Enter a name for the folder.", PublicFolderTree.NameProblem("  ", ".", existing));
         Assert.Equal("A name cannot begin or end with \".\" or have two together: \".\" separates a folder from its sub-folder.", PublicFolderTree.NameProblem("Sales.", ".", existing));
         Assert.Equal("sales.leads is a public folder already.", PublicFolderTree.NameProblem("sales.leads", ".", existing));
         Assert.Equal("A folder's name can be at most 255 characters long.", PublicFolderTree.NameProblem(new string('x', 256), ".", existing));
         Assert.Equal("A folder can be at most 25 levels down.", PublicFolderTree.NameProblem(string.Join(".", Enumerable.Range(1, 26)), ".", existing));

         // A parent that is there is used; a folder under it is new.
         Assert.Null(PublicFolderTree.NameProblem("Sales.Archive", ".", existing));
         Assert.Null(PublicFolderTree.NameProblem("Projects.2026", ".", existing));
      }

      [Fact]
      public void TheBox_OpensUnderTheSelectedFolder()
      {
         IReadOnlyList<Folder> folders = PublicFolderTree.Build(new[] { P("Sales"), P("Sales", "Leads") }, ".");
         Assert.Equal("Sales.Leads.", PublicFolderTree.Prefill(folders[1], "."));
         Assert.Equal("", PublicFolderTree.Prefill(null, "."));
      }

      [Fact]
      public void Delete_SaysWhatGoesWithTheFolder()
      {
         IReadOnlyList<Folder> folders = PublicFolderTree.Build(new[] { P("Sales"), P("Sales", "Leads"), P("Sales", "Leads", "Hot"), P("Support") }, ".");

         Assert.Equal("Delete the public folder Sales, its 2 sub-folders and every message in them?", PublicFolderTree.DeleteQuestion(folders[0]));
         Assert.Equal("Delete the public folder Sales.Leads, its 1 sub-folder and every message in them?", PublicFolderTree.DeleteQuestion(folders[1]));
         Assert.Equal(Confirmations.DeletePublicFolder("Support"), PublicFolderTree.DeleteQuestion(folders[3]));
      }

      [Fact]
      public void TheSubtitleAndTheHint_NameTheDelimiterInAnExample()
      {
         Assert.Equal("Shared IMAP folders that several accounts can open, each with the permissions it is given. A name with \"/\" in it makes a sub-folder: Sales/Leads puts Leads under Sales.",
            PublicFolderTree.Subtitle("/"));
         Assert.Equal("Sales.Leads makes Leads under Sales, and Sales as well if it is not there. With a folder selected, the name starts under it.",
            PublicFolderTree.Hint("."));
      }

      /// <summary>
      /// The page reads the delimiter and the tree through each folder's SubFolders,
      /// makes each part of a name under the one before it, and opens a folder's
      /// permissions by its path; the name's dialog waits for a name.
      /// </summary>
      [Fact]
      public void ThePage_ReadsAndMakesTheTreeThroughSubFolders()
      {
         string page = Read(Path.Join("Views", "PublicFoldersView.cs"));
         Assert.Contains("(string)settings.IMAPHierarchyDelimiter", page, StringComparison.Ordinal);
         Assert.Contains("ReadLevel_(subFolders, path, into);", page, StringComparison.Ordinal);
         Assert.Contains("header_.Subtitle = PublicFolderTree.Subtitle(delimiter_);", page, StringComparison.Ordinal);

         string create = WithheldRowTests.MemberText(page, "private string CreateFolder_(string typed)");
         Assert.True(create != null, "PublicFoldersView.CreateFolder_ is gone.");
         Assert.Contains("PublicFolderTree.NameProblem(typed, delimiter_, Folders_)", create, StringComparison.Ordinal);
         Assert.Contains("FindChild_(level, path[i]) ?? level.Add(path[i])", create, StringComparison.Ordinal);
         Assert.Contains("level = folder.SubFolders;", create, StringComparison.Ordinal);
         Assert.DoesNotContain("folders.Add(name", page, StringComparison.Ordinal);

         string open = WithheldRowTests.MemberText(page, "internal static dynamic OpenFolderAt(dynamic folders, IReadOnlyList<string> path)");
         Assert.True(open != null, "PublicFoldersView.OpenFolderAt is gone.");
         Assert.Contains("subFolders.ItemByName[path[i]]", open, StringComparison.Ordinal);

         string permissions = Read(Path.Join("Views", "FolderPermissionsDialog.cs"));
         string walk = WithheldRowTests.MemberText(permissions, "private dynamic OpenFolder(dynamic folders)");
         Assert.True(walk != null, "FolderPermissionsDialog.OpenFolder is gone.");
         Assert.Contains("subFolders.ItemByName[folderPath_[i]]", walk, StringComparison.Ordinal);
         Assert.DoesNotContain("folders.ItemByName[folderName_]", permissions, StringComparison.Ordinal);
         Assert.Contains("new FolderPermissionsDialog(Window.GetWindow(this), folder.Path, folder.FullName)", page, StringComparison.Ordinal);

         string add = WithheldRowTests.MemberText(page, "private void AddFolder()");
         Assert.True(add != null, "PublicFoldersView.AddFolder is gone.");
         Assert.Contains("PublicFolderTree.Prefill(list_.SelectedItem as PublicFolderTree.Folder, delimiter_)", add, StringComparison.Ordinal);
         Assert.Contains("Reload_(full, -1);", add, StringComparison.Ordinal);
         Assert.Contains("ListOutcome.Added(full, DateTime.Now)", add, StringComparison.Ordinal);

         // A search typed in before is cleared where it would leave the new folder out.
         string reload = WithheldRowTests.MemberText(page, "private void Reload_(string selectName, int selectIndex)");
         int reveals = reload.IndexOf("ListSearchState.Reveal(search_, list_, again);", StringComparison.Ordinal);
         Assert.True(reveals >= 0 && reveals < reload.IndexOf("list_.SelectedItem = again;", StringComparison.Ordinal),
            "A folder added under a search that hides it is not selected.");
         string reveal = WithheldRowTests.MemberText(Read(Path.Join("Services", "ListSearchState.cs")), "public static void Reveal(Toolbar search, ItemsControl list, object item)");
         Assert.True(reveal != null, "ListSearchState.Reveal is gone.");
         Assert.Contains("list.Items.Contains(item)", reveal, StringComparison.Ordinal);
         Assert.Contains("search.SearchText = \"\";", reveal, StringComparison.Ordinal);

         string dialog = Read(Path.Join("Views", "PublicFolderNameDialog.cs"));
         Assert.Contains("Content = L(\"Cr_eate\")", dialog, StringComparison.Ordinal);
         Assert.Contains("IsEnabled = name_.Text.Trim().Length > 0", dialog, StringComparison.Ordinal);
         Assert.Contains("createButton.IsEnabled = name_.Text.Trim().Length > 0;", dialog, StringComparison.Ordinal);
         Assert.Contains("KeepRoomForFieldErrors(2, nameRow_);", dialog, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowError(nameRow_, refusal);", dialog, StringComparison.Ordinal);
      }

      private static string Read(string relative)
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
