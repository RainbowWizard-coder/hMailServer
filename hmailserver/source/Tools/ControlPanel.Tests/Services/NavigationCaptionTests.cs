// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A button that opens another page names the page and ends in no ellipsis, which
   /// Windows keeps for a command that asks for more before it acts; a button that
   /// opens a dialog for more input ends in one (review findings F305, F042). The
   /// Dashboard's "Setup checklist…", DNS records' "Open Domains…", "Host name
   /// setting…" and "PTR / HELO checks…", the overviews' "Settings…", "Ports…" and
   /// "Patterns…", the signposts' "Open…" and the directory page's "Open Domains..."
   /// only navigated, while the list editor's Add and Edit, which open its dialog,
   /// had none. The links in a card are one style, CompactPageLink: the Caption size
   /// and 28 pixels tall at the least, where seven pages set 12 pixels and a 3-pixel
   /// padding of their own, a 24-pixel button beside the card's 32-pixel ones. The
   /// Dashboard's KPI values are all KpiValue's size, shrunk only to fit, and their
   /// labels the ramp's Caption size.
   /// </summary>
   public class NavigationCaptionTests
   {
      private static readonly Regex PageLinkCall = new Regex(
         @"\b\w*PageLink\w*\s*\(\s*[^,()]+,\s*(?:L|F)\(""((?:[^""\\]|\\.)*)""", RegexOptions.Compiled);

      [Fact]
      public void NoPageLinkCaptionEndsInAnEllipsis()
      {
         var offending = new List<string>();
         int calls = 0;
         foreach (string file in ViewFiles("*.cs"))
         {
            string source = File.ReadAllText(file);
            foreach (Match call in PageLinkCall.Matches(source))
            {
               calls++;
               string caption = call.Groups[1].Value;
               if (EndsInEllipsis(caption))
                  offending.Add(Path.GetFileName(file) + ": " + caption);
            }
         }

         Assert.True(calls >= 15, "Fewer page links than the Control Panel draws: " + calls);
         Assert.True(offending.Count == 0, "A page link's caption ends in an ellipsis: " + string.Join("; ", offending));
      }

      /// <summary>
      /// A XAML button whose Click handler does nothing but open a page has no
      /// ellipsis in its caption: the Dashboard's Open Setup checklist and the SSL
      /// certificates page's Set up ACME, which opens the ACME page.
      /// </summary>
      [Fact]
      public void NoXamlButtonThatOnlyNavigatesEndsInAnEllipsis()
      {
         var button = new Regex(@"<ui:(?:Button|HyperlinkButton)\b[^>]*?Content=""\{loc:L '((?:[^'\\]|\\.)*)'\}""[^>]*?Click=""(\w+)""", RegexOptions.Singleline);
         var buttonClickFirst = new Regex(@"<ui:(?:Button|HyperlinkButton)\b[^>]*?Click=""(\w+)""[^>]*?Content=""\{loc:L '((?:[^'\\]|\\.)*)'\}""", RegexOptions.Singleline);
         var offending = new List<string>();
         int navigating = 0;
         foreach (string xaml in ViewFiles("*.xaml"))
         {
            string codeBehind = xaml + ".cs";
            if (!File.Exists(codeBehind))
               continue;
            string markup = File.ReadAllText(xaml);
            string code = File.ReadAllText(codeBehind);
            var pairs = button.Matches(markup).Select(m => (Caption: m.Groups[1].Value, Handler: m.Groups[2].Value))
               .Concat(buttonClickFirst.Matches(markup).Select(m => (Caption: m.Groups[2].Value, Handler: m.Groups[1].Value)));
            foreach (var (caption, handler) in pairs.Distinct())
            {
               Match body = Regex.Match(code, @"void " + handler + @"\(object sender, RoutedEventArgs e\)\s*=>\s*([^;]+);");
               if (!body.Success || !Regex.IsMatch(body.Groups[1].Value, @"\.NavigateTo(?:Tab)?\("))
                  continue;
               navigating++;
               if (EndsInEllipsis(caption))
                  offending.Add(Path.GetFileName(xaml) + ": " + caption);
            }
         }

         Assert.True(navigating >= 2, "The XAML buttons that only open a page were not found: " + navigating);
         Assert.True(offending.Count == 0, "A button that only opens a page ends in an ellipsis: " + string.Join("; ", offending));
      }

      /// <summary>
      /// A XAML button whose Click handler opens a dialog that asks for more - an
      /// entry's own dialog, a rule's criterion or action - ends in the ellipsis, so
      /// that pages built alike say alike: IP ranges' Properties read "Prop_erties…"
      /// while Routes' and TCP/IP ports', which open their entry's dialog the same
      /// way, read "_Properties" (review finding F305). The handler is read with the
      /// methods of its file that it calls, three calls deep. Not held to it: a dialog
      /// that asks nothing (the message viewer of View source) or that is a step of
      /// another act (the code the sign-in asks for when the server wants a second
      /// factor). The domains page's row buttons are 24-pixel icon buttons named for
      /// their row (the owner's decision 15 of 9 October 2026), with no caption.
      /// </summary>
      [Fact]
      public void EveryXamlButtonThatOpensADialogForInputEndsInAnEllipsis()
      {
         var asksNothing = new HashSet<string>(StringComparer.Ordinal) { "MessageViewerDialog", "TotpPromptDialog" };
         var button = new Regex(@"<ui:(?:Button|HyperlinkButton)\b[^>]*>", RegexOptions.Singleline);
         var offending = new List<string>();
         int opening = 0;
         foreach (string xaml in ViewFiles("*.xaml"))
         {
            string codeBehind = xaml + ".cs";
            if (!File.Exists(codeBehind))
               continue;
            string code = File.ReadAllText(codeBehind);
            foreach (Match element in button.Matches(File.ReadAllText(xaml)))
            {
               Match caption = Regex.Match(element.Value, @"Content=""\{loc:L '((?:[^'\\]|\\.)*)'\}""");
               Match click = Regex.Match(element.Value, @"Click=""(\w+)""");
               if (!caption.Success || !click.Success)
                  continue;
               var dialogs = DialogsOpenedBy(code, MethodBody(code, click.Groups[1].Value), 0, new HashSet<string>(StringComparer.Ordinal));
               dialogs.ExceptWith(asksNothing);
               if (dialogs.Count == 0)
                  continue;
               opening++;
               if (!EndsInEllipsis(caption.Groups[1].Value))
                  offending.Add(Path.GetFileName(xaml) + ": \"" + caption.Groups[1].Value + "\" opens " + string.Join(", ", dialogs));
            }
         }

         // Seven since the sign-in card lost its two-factor link (review finding F241).
         Assert.True(opening >= 7, "The XAML buttons that open a dialog were not found: " + opening);
         Assert.True(offending.Count == 0, "A button that opens a dialog for input must end in an ellipsis: " + string.Join("; ", offending));
      }

      /// <summary>The body of the method <paramref name="name"/> in <paramref name="code"/>, or null.</summary>
      private static string MethodBody(string code, string name)
      {
         Match head = Regex.Match(code, @"\b(?:void|bool|Task|async\s+void|async\s+Task)\s+" + Regex.Escape(name) + @"\s*\([^)]*\)\s*(=>[^;]+;|\{)");
         if (!head.Success)
            return null;
         if (head.Groups[1].Value.StartsWith("=>", StringComparison.Ordinal))
            return head.Groups[1].Value;
         int i = head.Index + head.Length, depth = 1;
         for (; i < code.Length && depth > 0; i++)
            depth += code[i] == '{' ? 1 : code[i] == '}' ? -1 : 0;
         return code.Substring(head.Index, i - head.Index);
      }

      /// <summary>The dialogs <paramref name="body"/> constructs, itself or through the methods of the file it calls.</summary>
      private static HashSet<string> DialogsOpenedBy(string code, string body, int depth, HashSet<string> seen)
      {
         var found = new HashSet<string>(StringComparer.Ordinal);
         if (body == null)
            return found;
         foreach (Match dialog in Regex.Matches(body, @"new\s+(\w+Dialog)\s*\("))
            found.Add(dialog.Groups[1].Value);
         if (depth >= 3)
            return found;
         foreach (Match call in Regex.Matches(body, @"(?<![\w.])(\w+)\s*\("))
         {
            if (seen.Add(call.Groups[1].Value))
               found.UnionWith(DialogsOpenedBy(code, MethodBody(code, call.Groups[1].Value), depth + 1, seen));
         }
         return found;
      }

      [Fact]
      public void TheListEditorsAddAndEditSayTheyOpenADialog()
      {
         string editor = Read("Views", "CollectionEditorView.cs");

         Assert.Contains("MakeButton(L(\"_Add…\")", editor, StringComparison.Ordinal);
         Assert.Contains("MakeButton(L(\"_Edit…\")", editor, StringComparison.Ordinal);
         Assert.DoesNotContain("MakeButton(L(\"_Add\")", editor, StringComparison.Ordinal);
         Assert.DoesNotContain("MakeButton(L(\"_Edit\")", editor, StringComparison.Ordinal);
      }

      /// <summary>
      /// The public folder permissions dialog's Add and Edit open an entry's own
      /// dialog, which asks for the account or group and its rights before anything
      /// is saved, as the list editor's do, and say so with the ellipsis; they were
      /// the dialog's "_Add" and "_Edit".
      /// </summary>
      [Fact]
      public void TheFolderPermissionsAddAndEditSayTheyOpenADialog()
      {
         string dialog = Read("Views", "FolderPermissionsDialog.cs");

         Assert.Contains("new PermissionEditDialog(", dialog, StringComparison.Ordinal);
         Assert.Contains("var add = new Wpf.Ui.Controls.Button { Content = L(\"_Add…\")", dialog, StringComparison.Ordinal);
         Assert.Contains("var edit = new Wpf.Ui.Controls.Button { Content = L(\"_Edit…\")", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"_Add\")", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"_Edit\")", dialog, StringComparison.Ordinal);
      }

      [Fact]
      public void ASignpostNamesThePageItOpens()
      {
         string feature = Read("Views", "FeatureSettingsView.xaml.cs");

         Assert.DoesNotContain("L(\"Open…\")", feature, StringComparison.Ordinal);
         Assert.Contains("Content = F(\"Open {0}\", pageTitle)", feature, StringComparison.Ordinal);
         // The name starts with the words on the button (Label in Name).
         Assert.Contains("F(\"Open {0}, which now has {1}\", pageTitle, Label)", feature, StringComparison.Ordinal);
      }

      [Fact]
      public void EveryCardLinkTakesTheCompactPageLinkStyle()
      {
         string app = Read("App.xaml");
         Match style = Regex.Match(app, @"<Style x:Key=""CompactPageLink""[^>]*>(.*?)</Style>", RegexOptions.Singleline);
         Assert.True(style.Success, "App.xaml has no CompactPageLink style.");
         Assert.Contains("TargetType=\"{x:Type ui:Button}\"", style.Value, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"FontSize\" Value=\"{DynamicResource AppFontSizeCaption}\" />", style.Value, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"Padding\" Value=\"8,4,8,4\" />", style.Value, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"MinHeight\" Value=\"28\" />", style.Value, StringComparison.Ordinal);
         // A caption with an access key is an AccessText, whose implicit style (WPF-UI's)
         // sets the Body size over the button's: the Dashboard's "Open Setup _checklist"
         // was drawn at 14 pixels. The string's template in the style sets the Caption size.
         Assert.Matches(new Regex(@"<Style\.Resources>\s*<DataTemplate DataType=""\{x:Type sys:String\}"">\s*<AccessText Text=""\{Binding\}"" FontSize=""\{DynamicResource AppFontSizeCaption\}"" />\s*</DataTemplate>\s*</Style\.Resources>"), style.Value);

         string styles = Read("Services", "ButtonStyles.cs");
         Assert.Contains("TryFindResource(\"CompactPageLink\")", styles, StringComparison.Ordinal);

         foreach (string view in new[] { "ExternalSetupView.cs", "DnsRecordsView.cs", "SpamOverviewView.cs", "TlsOverviewView.cs", "VirusOverviewView.cs", "ApiKeysView.cs", "LdapSettingsView.cs" })
         {
            string source = Read("Views", view);
            Match helper = Regex.Match(source, @"private static FrameworkElement PageLink_?\((?:[^)]*)\)\s*\{(.*?)\r?\n      \}", RegexOptions.Singleline);
            Assert.True(helper.Success, view + " has no PageLink helper.");
            Assert.Contains("ButtonStyles.CompactPageLink(button);", helper.Value, StringComparison.Ordinal);
            Assert.DoesNotContain("FontSize =", helper.Value, StringComparison.Ordinal);
            Assert.DoesNotContain("Padding =", helper.Value, StringComparison.Ordinal);
         }

         string signpostClass = WithheldRowTests.MemberText(Read("Views", "FeatureSettingsView.xaml.cs"), "private class ElsewhereSetting : Setting");
         Assert.True(signpostClass != null, "FeatureSettingsView has no ElsewhereSetting.");
         string signpost = WithheldRowTests.MemberText(signpostClass, "public override FrameworkElement CreateEditor(IniFeatureStore store)");
         Assert.True(signpost != null && signpost.Contains("ButtonStyles.CompactPageLink(button);", StringComparison.Ordinal),
            "The signpost row's button does not take the CompactPageLink style.");
      }

      [Fact]
      public void TheDashboardsKpisAreOneSizeAndItsSetupLinkIsACompactPageLink()
      {
         string dashboard = Read("Views", "DashboardView.xaml");

         Match link = Regex.Match(dashboard, @"<ui:Button x:Name=""OpenExternalSetupButton""[^>]*>", RegexOptions.Singleline);
         Assert.True(link.Success, "The Dashboard has no OpenExternalSetupButton.");
         Assert.Contains("Style=\"{StaticResource CompactPageLink}\"", link.Value, StringComparison.Ordinal);
         Assert.Contains("Content=\"{loc:L 'Open Setup _checklist'}\"", link.Value, StringComparison.Ordinal);
         Assert.DoesNotContain("FontSize=", link.Value, StringComparison.Ordinal);

         MatchCollection values = Regex.Matches(dashboard, @"<TextBlock x:Name=""Kpi\w+""[^>]*Style=""\{StaticResource KpiValue\}""[^>]*/>");
         Assert.Equal(5, values.Count);
         foreach (Match value in values)
         {
            Assert.DoesNotContain("FontSize=", value.Value, StringComparison.Ordinal);
            string before = dashboard.Substring(0, value.Index).TrimEnd();
            Assert.True(before.EndsWith("<Viewbox StretchDirection=\"DownOnly\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\">", StringComparison.Ordinal),
               "A KPI value is not in a Viewbox that only shrinks it: " + value.Value);
         }

         string app = Read("App.xaml");
         Match label = Regex.Match(app, @"<Style x:Key=""KpiLabel""[^>]*>(.*?)</Style>", RegexOptions.Singleline);
         Assert.True(label.Success, "App.xaml has no KpiLabel style.");
         Assert.Contains("<Setter Property=\"FontSize\" Value=\"{DynamicResource AppFontSizeCaption}\" />", label.Value, StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"TextWrapping\" Value=\"Wrap\" />", label.Value, StringComparison.Ordinal);
      }

      private static bool EndsInEllipsis(string caption)
         => caption.EndsWith("…", StringComparison.Ordinal) || caption.EndsWith("...", StringComparison.Ordinal)
            || caption.EndsWith("\\u2026", StringComparison.Ordinal);

      private static IEnumerable<string> ViewFiles(string pattern)
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");
         return Directory.GetFiles(Path.Join(controlPanel, "Views"), pattern, SearchOption.AllDirectories);
      }

      private static string Read(params string[] parts)
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");
         return File.ReadAllText(Path.Join(controlPanel, Path.Join(parts)));
      }
   }
}
