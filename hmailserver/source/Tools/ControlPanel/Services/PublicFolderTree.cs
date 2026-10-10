// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Public folders page's tree, and the words it says about it (review finding
   /// F206). The page said "Use the hierarchy delimiter to create sub-folders" without
   /// naming the delimiter or showing a hierarchy: Sales and Sales.Leads were two flat
   /// rows, and a delete of Sales did not say that the folders under it went with it.
   ///
   /// What that advice made was worse than a flat list. Settings.PublicFolders.Add
   /// takes the name it is given as one folder's name (COM InterfaceIMAPFolders::Add),
   /// so "Sales.Leads" became a folder at the top whose own name holds the delimiter,
   /// and a mail client cannot open it: the server finds a folder by splitting its path
   /// at the delimiter and walking down from the top (IMAPFolders::GetFolderByFullPath),
   /// and Sales has no folder Leads under it. A name with the delimiter in it now makes
   /// the folder under the folders it names, as a client's CREATE does, and a folder
   /// named so already is pointed out.
   ///
   /// No WPF and no COM here, so the tests hold the tree, the checks and the words.
   /// </summary>
   public static class PublicFolderTree
   {
      /// <summary>The delimiter a server that says none uses, as the server's own default.</summary>
      public const string DefaultDelimiter = ".";

      /// <summary>How deep a folder may be (IMAPFolder::MaxFolderDepth).</summary>
      public const int MaxDepth = 25;

      /// <summary>The longest name a folder may have (IMAPFolder::IsValidFolderName).</summary>
      public const int MaxNameLength = 255;

      /// <summary>One folder of the tree, in the order the page lists it.</summary>
      public sealed class Folder
      {
         public Folder(IReadOnlyList<string> path, int subFolderCount, string delimiter)
         {
            Path = path ?? Array.Empty<string>();
            SubFolderCount = subFolderCount;
            FullName = string.Join(delimiter ?? DefaultDelimiter, Path);
            NameHoldsDelimiter = Path.Count > 0 && !string.IsNullOrEmpty(delimiter) && Path[Path.Count - 1].Contains(delimiter, StringComparison.Ordinal);
         }

         /// <summary>The names from the top folder down to this one.</summary>
         public IReadOnlyList<string> Path { get; }

         /// <summary>This folder's own name.</summary>
         public string Name => Path.Count == 0 ? "" : Path[Path.Count - 1];

         /// <summary>How far down the tree it is: 0 at the top.</summary>
         public int Depth => Math.Max(0, Path.Count - 1);

         /// <summary>How many folders are under it, at every depth: the ones a delete takes with it.</summary>
         public int SubFolderCount { get; }

         /// <summary>The names joined with the delimiter, as a mail client sees the folder: Sales.Leads.</summary>
         public string FullName { get; }

         /// <summary>Whether its own name holds the delimiter, so that a mail client cannot open it.</summary>
         public bool NameHoldsDelimiter { get; }

         // What the list's search reads and what a screen reader says for the row.
         public override string ToString() => FullName;
      }

      /// <summary>The server's delimiter, or the default where it says none.</summary>
      public static string DelimiterOf(string configured) => string.IsNullOrEmpty(configured) ? DefaultDelimiter : configured;

      /// <summary>
      /// The folders read, each given as the names from the top down, as the page lists
      /// them: every folder under its parent, the folders of one parent by name without
      /// regard to case, and each with the number of folders under it.
      /// </summary>
      public static IReadOnlyList<Folder> Build(IEnumerable<IReadOnlyList<string>> paths, string delimiter)
      {
         var root = new Node(null);
         foreach (IReadOnlyList<string> path in paths ?? Enumerable.Empty<IReadOnlyList<string>>())
         {
            if (path == null || path.Count == 0)
               continue;

            Node at = root;
            foreach (string name in path)
               at = at.Child(name ?? "");
         }

         var folders = new List<Folder>();
         foreach (Node top in root.Ordered())
            Walk_(top, new List<string>(), folders, delimiter);
         return folders;
      }

      private static int Walk_(Node node, List<string> above, List<Folder> into, string delimiter)
      {
         var path = new List<string>(above) { node.Name };
         int at = into.Count;
         into.Add(null);
         int under = 0;
         foreach (Node child in node.Ordered())
            under += 1 + Walk_(child, path, into, delimiter);
         into[at] = new Folder(path, under, delimiter);
         return under;
      }

      private sealed class Node
      {
         private readonly List<Node> children_ = new();

         public Node(string name) => Name = name;

         public string Name { get; }

         public Node Child(string name)
         {
            Node found = children_.FirstOrDefault(c => string.Equals(c.Name, name, StringComparison.OrdinalIgnoreCase));
            if (found == null)
            {
               found = new Node(name);
               children_.Add(found);
            }
            return found;
         }

         public IEnumerable<Node> Ordered() => children_.OrderBy(c => c.Name, StringComparer.OrdinalIgnoreCase);
      }

      /// <summary>
      /// The names a typed name makes, from the top down, split at the delimiter: Sales,
      /// Leads for Sales.Leads. Null when a part is empty - a name that begins or ends
      /// with the delimiter, or has two together - or when nothing is typed.
      /// </summary>
      public static IReadOnlyList<string> Split(string typed, string delimiter)
      {
         string name = (typed ?? "").Trim();
         if (name.Length == 0)
            return null;

         string[] parts = name.Split(new[] { DelimiterOf(delimiter) }, StringSplitOptions.None)
            .Select(part => part.Trim())
            .ToArray();
         return parts.Any(part => part.Length == 0) ? null : parts;
      }

      /// <summary>
      /// Why a typed name cannot be made into a folder, said on the name's box, or null
      /// when it can: nothing typed, an empty part, a part too long, a path too deep, or
      /// a folder of that name already there.
      /// </summary>
      public static string NameProblem(string typed, string delimiter, IEnumerable<Folder> existing)
      {
         string d = DelimiterOf(delimiter);
         if ((typed ?? "").Trim().Length == 0)
            return L("Enter a name for the folder.");

         IReadOnlyList<string> path = Split(typed, d);
         if (path == null)
            return F("A name cannot begin or end with \"{0}\" or have two together: \"{0}\" separates a folder from its sub-folder.", d);

         if (path.Any(part => part.Length > MaxNameLength))
            return F("A folder's name can be at most {0} characters long.", MaxNameLength);

         if (path.Count > MaxDepth)
            return F("A folder can be at most {0} levels down.", MaxDepth);

         if (Find(existing, path) != null)
            return F("{0} is a public folder already.", string.Join(d, path));

         return null;
      }

      /// <summary>
      /// Whether two paths are one folder's: as many names, each the same without regard
      /// to case, as the server finds a folder. Not their names joined: a folder at the top
      /// named Projects.2025, which the page's old advice made, and the sub-folder 2025 of
      /// Projects join to one name and are two folders, so that comparing joined names
      /// refused to make the sub-folder the warning above the list says to make, and
      /// selected the wrong one of the two after an add.
      /// </summary>
      public static bool SamePath(IReadOnlyList<string> a, IReadOnlyList<string> b)
      {
         if (a == null || b == null || a.Count != b.Count)
            return false;

         for (int i = 0; i < a.Count; i++)
         {
            if (!string.Equals(a[i], b[i], StringComparison.OrdinalIgnoreCase))
               return false;
         }

         return true;
      }

      /// <summary>The folder of this path among <paramref name="folders"/>, or null.</summary>
      public static Folder Find(IEnumerable<Folder> folders, IReadOnlyList<string> path)
         => path == null ? null : (folders ?? Enumerable.Empty<Folder>()).FirstOrDefault(f => SamePath(f.Path, path));

      /// <summary>What the name's box opens with: the selected folder's name and the delimiter, so that what is typed goes under it; empty with none selected.</summary>
      public static string Prefill(Folder selected, string delimiter)
         => selected == null ? "" : selected.FullName + DelimiterOf(delimiter);

      /// <summary>The page's subtitle, naming the delimiter in an example.</summary>
      public static string Subtitle(string delimiter)
      {
         string d = DelimiterOf(delimiter);
         return F("Shared IMAP folders that several accounts can open, each with the permissions it is given. A name with \"{0}\" in it makes a sub-folder: {1} puts {2} under {3}.",
            d, "Sales" + d + "Leads", "Leads", "Sales"); // no-loc: an example folder's names
      }

      /// <summary>The hint under the name's box.</summary>
      public static string Hint(string delimiter)
      {
         string d = DelimiterOf(delimiter);
         return F("{0} makes {1} under {2}, and {2} as well if it is not there. With a folder selected, the name starts under it.",
            "Sales" + d + "Leads", "Leads", "Sales"); // no-loc: an example folder's names
      }

      /// <summary>
      /// Delete's question: the folder, how many folders under it go with it, and their
      /// messages. Delete takes the folders under a folder with it
      /// (PersistentIMAPFolder::DeleteObject), which the question did not say.
      /// </summary>
      public static string DeleteQuestion(Folder folder)
      {
         if (folder == null)
            return null;
         if (folder.SubFolderCount <= 0)
            return Confirmations.DeletePublicFolder(folder.FullName);
         return PluralF(folder.SubFolderCount,
            "Delete the public folder {1}, its {0} sub-folder and every message in them?",
            "Delete the public folder {1}, its {0} sub-folders and every message in them?",
            folder.SubFolderCount, folder.FullName);
      }

      /// <summary>
      /// What the page says of the folders whose own name holds the delimiter, which the
      /// page's old advice made and a mail client cannot open; null when there are none.
      /// </summary>
      public static string MisnamedNotice(IReadOnlyList<Folder> folders, string delimiter)
      {
         string d = DelimiterOf(delimiter);
         List<Folder> misnamed = (folders ?? Array.Empty<Folder>()).Where(f => f.NameHoldsDelimiter).ToList();
         if (misnamed.Count == 0)
            return null;
         if (misnamed.Count == 1)
            return F("{0} has \"{1}\" in its own name rather than being a sub-folder, so a mail client cannot open it. Add folder now makes such a name a sub-folder.", misnamed[0].FullName, d);
         return PluralF(misnamed.Count,
            "{0} folder has \"{2}\" in its own name rather than being a sub-folder, so a mail client cannot open it: {1}. Add folder now makes such a name a sub-folder.",
            "{0} folders have \"{2}\" in their own names rather than being sub-folders, so a mail client cannot open them: {1}. Add folder now makes such a name a sub-folder.",
            misnamed.Count, string.Join(", ", misnamed.Select(f => f.FullName)), d);
      }
   }
}
