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
   /// Add SURBL server and Add blacklist opened on an unticked Active box, listed
   /// first, and on an empty Expected result with nothing to say what goes in it, so
   /// a list added and saved was never asked, or never a match (review finding F174).
   /// These hold the order the generated dialog lays its fields out in, the words for
   /// a required field left empty, that the two specs start a new list active and
   /// say what each field wants, that a DNS blacklist's host and expected result are
   /// required, and that the dialog's button says Add when it adds.
   /// </summary>
   public class BlockListDialogTests
   {
      [Theory]
      [InlineData("b:Active,t:DNSHost,t:ExpectedResult,t:RejectMessage,n:Score", "DNSHost,ExpectedResult,RejectMessage,Score,Active")]
      [InlineData("b:Enabled,t:Name,t:Server,b:UseAntiSpam,t:Headers,b:Route", "Name,Server,UseAntiSpam,Headers,Route,Enabled")] // a box after a text field keeps its place
      [InlineData("b:Active,b:Blocked,t:Value", "Value,Active,Blocked")]                                                         // the leading run keeps its order
      [InlineData("t:Name,b:Active", "Name,Active")]
      [InlineData("t:Name,t:Text", "Name,Text")]
      [InlineData("b:Only", "Only")]
      [InlineData("", "")]
      public void TheTickBoxesThatLeadASpec_GoAfterTheRest(string fields, string expected)
      {
         string[] given = fields.Length == 0 ? Array.Empty<string>() : fields.Split(',');
         string ordered = string.Join(",", FieldDialogRules.TickBoxesAfterTheRest(given, f => f.StartsWith("b:", StringComparison.Ordinal))
            .Select(f => f.Substring(2)));
         Assert.Equal(expected, ordered);
      }

      [Fact]
      public void TheRequiredMessage_NamesTheField_WithoutItsAltKey()
      {
         Assert.Contains("Expected result", FieldDialogRules.RequiredMessage("Expected result"), StringComparison.Ordinal);
         Assert.Contains("DNS host", FieldDialogRules.RequiredMessage("_DNS host"), StringComparison.Ordinal);
         Assert.DoesNotContain("_", FieldDialogRules.RequiredMessage("_DNS host"), StringComparison.Ordinal);
         Assert.DoesNotContain("{0}", FieldDialogRules.RequiredMessage(null), StringComparison.Ordinal);
      }

      [Fact]
      public void ANewBlockListStartsActive_AndItsFieldsSayWhatTheyWant()
      {
         string specs = ReadSource("CollectionSpecs.cs");

         foreach (string signature in new[] { "public static CollectionEditorView SurblServers()", "public static CollectionEditorView DnsBlackLists()" })
         {
            string spec = WithheldRowTests.MemberText(specs, signature);
            Assert.True(spec != null, signature + " is gone from CollectionSpecs.");

            string active = Field(spec, "Active");
            Assert.Contains("Default = true", active, StringComparison.Ordinal);

            string host = Field(spec, "DNSHost");
            Assert.Contains("Required = true", host, StringComparison.Ordinal);
            Assert.Contains("Hint = L(\"Such as ", host, StringComparison.Ordinal);

            Assert.Contains("Hint = L(\"The answer that means listed: 127.0.0.2, ", Field(spec, "ExpectedResult"), StringComparison.Ordinal);
            Assert.Contains("Hint = L(", Field(spec, "Score"), StringComparison.Ordinal);
         }

         // A DNS blacklist with no expected result matches no answer at all; a SURBL
         // server with none takes any answer but the list's own error codes.
         Assert.Contains("Required = true", Field(WithheldRowTests.MemberText(specs, "public static CollectionEditorView DnsBlackLists()"), "ExpectedResult"), StringComparison.Ordinal);
         Assert.DoesNotContain("Required = true", Field(WithheldRowTests.MemberText(specs, "public static CollectionEditorView SurblServers()"), "ExpectedResult"), StringComparison.Ordinal);
      }

      [Fact]
      public void TheDialog_SaysAddWhenItAdds_LaysTickBoxesLast_AndRefusesARequiredFieldEmpty()
      {
         string dialog = ReadSource("FieldDialog.cs");

         Assert.Contains("existing == null ? L(\"_Add\") : L(\"_Save\")", dialog, StringComparison.Ordinal);
         Assert.Contains("FieldDialogRules.TickBoxesAfterTheRest(", dialog, StringComparison.Ordinal);

         int required = dialog.IndexOf("if (f.Required && string.IsNullOrWhiteSpace(box.Text))", StringComparison.Ordinal);
         int refused = dialog.IndexOf("DialogFields.ShowError(row, FieldDialogRules.RequiredMessage(f.Label));", StringComparison.Ordinal);
         int written = dialog.IndexOf("Result[prop] = box.Text;", required < 0 ? 0 : required, StringComparison.Ordinal);
         Assert.True(required >= 0 && refused > required && written > refused, "The dialog writes a required text field before it refuses it empty.");
         Assert.Contains("DialogFields.ClearErrors(rows_.ToArray());", dialog, StringComparison.Ordinal);
      }

      /// <summary>The one FieldSpec initialiser of a spec member that names the property.</summary>
      private static string Field(string spec, string prop)
      {
         int at = spec.IndexOf("Prop = \"" + prop + "\"", StringComparison.Ordinal);
         Assert.True(at >= 0, "No field " + prop + ".");
         int end = spec.IndexOf("}", at, StringComparison.Ordinal);
         return spec.Substring(at, end - at);
      }

      private static string ReadSource(string view)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", view);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + view + " could not be found from the test binaries.");
         return null;
      }
   }
}
