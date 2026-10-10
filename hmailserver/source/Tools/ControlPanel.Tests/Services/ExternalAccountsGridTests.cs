// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The account dialog's External tab gives its grid's server column - the one
   /// that says which mailbox a row collects from, and the grid's one star column -
   /// room for a server's name (review finding F199). Beside fixed columns for
   /// Enabled, the name, the server type, the port and the connection security its
   /// header read "Ser…"; with the type off the grid it had about 110 pixels and a
   /// row read "pop.oldprovide…".
   /// </summary>
   public class ExternalAccountsGridTests
   {
      // The External tab's grid, in the account dialog at its width: 578 pixels, of
      // which the card's padding, the grid's vertical bar and the cells' edges take
      // some 30.
      private const double GridWidth = 578;
      private const double GridChrome = 30;
      private const double ServerFloor = 200;

      [Fact]
      public void TheServerColumn_IsTheOneStarColumn_AndHasTwoHundredPixelsOrMore()
      {
         var fields = Fields().ToList();
         var shown = fields.Where(f => !f.Body.Contains("ShowInGrid = false", StringComparison.Ordinal)).ToList();

         Assert.Equal(new[] { "Enabled", "Name", "ServerAddress", "Port" }, shown.Select(f => f.Prop).ToArray());
         Assert.Equal(new[] { "ServerAddress" }, shown.Where(f => f.Width == null).Select(f => f.Prop).ToArray());

         double fixedWidth = shown.Where(f => f.Width != null).Sum(f => f.Width.Value);
         Assert.True(GridWidth - GridChrome - fixedWidth >= ServerFloor,
            "The External tab's fixed columns take " + fixedWidth + " pixels, which leaves the server " + (GridWidth - GridChrome - fixedWidth) + ".");
      }

      [Theory]
      // Both say in sentences what they choose, and the dialog shows them.
      [InlineData("ServerType")]
      [InlineData("ConnectionSecurity")]
      public void TheChoicesWhoseWordsAreSentences_AreInTheDialogOnly(string prop)
      {
         var field = Fields().Single(f => f.Prop == prop);
         Assert.Contains("ShowInGrid = false", field.Body, StringComparison.Ordinal);
      }

      private static System.Collections.Generic.IEnumerable<(string Prop, double? Width, string Body)> Fields()
      {
         string specs = Read(Path.Join("Views", "CollectionSpecs.cs"));
         int start = specs.IndexOf("public static CollectionEditorView FetchAccounts(", StringComparison.Ordinal);
         int end = start < 0 ? -1 : specs.IndexOf(SavedAsMadeNoteTests.DialogTabEnd, start, StringComparison.Ordinal);
         Assert.True(start >= 0 && end > start, "CollectionSpecs no longer has FetchAccounts as a dialog tab's editor.");
         string block = specs.Substring(start, end - start);

         foreach (Match m in Regex.Matches(block, @"new FieldSpec\s*\{(.*?)\}", RegexOptions.Singleline))
         {
            string body = m.Groups[1].Value;
            string prop = Regex.Match(body, @"Prop = ""(\w+)""").Groups[1].Value;
            Match width = Regex.Match(body, @"GridWidth = (\d+)");
            yield return (prop, width.Success ? double.Parse(width.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture) : null, body);
         }
      }

      private static string Read(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
