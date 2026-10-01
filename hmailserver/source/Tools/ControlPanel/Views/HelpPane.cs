// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using Wpf.Ui.Controls;
using hMailServer.ControlPanel.Services;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;
using Typography = hMailServer.ControlPanel.Services.Typography;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Help beside the page: what the documentation says about the page
   /// showing, in a pane to the right of it - never a window, never a dialog,
   /// so the page stays usable while the text is open. The text is this
   /// program's own embedded copy of hmailserver/docs (<see cref="HelpPages"/>),
   /// which is what the server serves at /api/v1/help and what the wiki's
   /// pages are written from; the page is mapped to its document and heading
   /// by <see cref="HelpMap"/>, read by <see cref="MarkdownBlocks"/> and drawn
   /// by <see cref="MarkdownFlow"/> into a FlowDocumentScrollViewer, scrolled
   /// to the heading, with the document's other headings as a list at the
   /// top, a search box over the open document, and two links at the foot:
   /// the wiki's copy of the page, and every document by its title. The shell
   /// (MainWindow) owns the pane's column, its width and whether it is open,
   /// and tells it the page as the navigation changes.
   /// </summary>
   public class HelpPane : UserControl
   {
      private readonly TextBlock title_ = new TextBlock { FontSize = Typography.Body, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
      private readonly TextBox search_ = new TextBox { FontSize = Typography.Control, ClearButtonEnabled = true, Margin = new Thickness(0, 8, 0, 8) };
      private readonly StackPanel headings_ = new StackPanel();
      private readonly ScrollViewer headingsScroll_ = new ScrollViewer { MaxHeight = 190, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
      private readonly FlowDocumentScrollViewer viewer_ = new FlowDocumentScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, IsToolBarVisible = false, Padding = new Thickness(0, 0, 8, 0) };
      private readonly Button wikiButton_;
      private readonly Button allButton_;
      private readonly Button closeButton_;

      private string shown_;
      private IReadOnlyDictionary<string, Block> headingBlocks_ = new Dictionary<string, Block>();

      /// <summary>
      /// The wiki base URL "Open on the wiki" is made against; empty hides the
      /// link. Kept only when it is a web address (<see cref="HelpPages.WikiBaseUrlOrEmpty"/>):
      /// the value reaches ShellExecute, and the server's setters are the
      /// only other guard - a row written by SQL, or by a server of another
      /// build, is not theirs to have checked.
      /// </summary>
      public string WikiBaseUrl
      {
         get => wikiBaseUrl_;
         set
         {
            wikiBaseUrl_ = HelpPages.WikiBaseUrlOrEmpty(value);
            UpdateWikiLink_();
         }
      }
      private string wikiBaseUrl_ = "";

      /// <summary>The name of the document showing, or null for the list of all of them.</summary>
      public string Shown => shown_;

      /// <summary>Raised by the pane's own close button; the shell closes the column.</summary>
      public event EventHandler CloseRequested;

      public HelpPane()
      {
         AutomationProperties.SetName(this, L("Help"));
         AutomationProperties.SetAutomationId(this, "help-pane");

         var root = new Grid { Margin = new Thickness(12, 8, 4, 8) };
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

         // The head: the document's title, and the close button.
         var head = new Grid();
         head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         title_.Text = L("Help");
         title_.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
         AutomationProperties.SetAutomationId(title_, "help-title");
         AutomationProperties.SetHeadingLevel(title_, AutomationHeadingLevel.Level2);
         head.Children.Add(title_);
         closeButton_ = new Button
         {
            Appearance = ControlAppearance.Transparent,
            Icon = new SymbolIcon(SymbolRegular.Dismiss24),
            Padding = new Thickness(6, 4, 6, 4),
            ToolTip = L("Close the help")
         };
         AutomationProperties.SetName(closeButton_, L("Close the help"));
         AutomationProperties.SetAutomationId(closeButton_, "help-close");
         closeButton_.Click += (s, e) => CloseRequested?.Invoke(this, EventArgs.Empty);
         Grid.SetColumn(closeButton_, 1);
         head.Children.Add(closeButton_);
         root.Children.Add(head);

         // The search over the open document: typing finds the first match,
         // Enter the next, round again from the top.
         search_.PlaceholderText = L("Search this document");
         AutomationProperties.SetName(search_, L("Search this document"));
         AutomationProperties.SetAutomationId(search_, "help-search");
         search_.TextChanged += (s, e) => Find_(false);
         search_.KeyDown += (s, e) =>
         {
            if (e.Key == Key.Enter)
            {
               Find_(true);
               e.Handled = true;
            }
         };
         Grid.SetRow(search_, 1);
         root.Children.Add(search_);

         // The document's other headings, each a link that scrolls to it.
         headingsScroll_.Content = headings_;
         headingsScroll_.Margin = new Thickness(0, 0, 0, 6);
         AutomationProperties.SetName(headingsScroll_, L("Headings"));
         AutomationProperties.SetAutomationId(headingsScroll_, "help-headings");
         Grid.SetRow(headingsScroll_, 2);
         root.Children.Add(headingsScroll_);

         // The text. Its copy is the Control Panel's (issue #48): Ctrl+C, the menu's Copy
         // and a selection dragged out carry plain text, written through ClipboardWriter,
         // and the right-click menu is the Copy / Select all every other text has, in the
         // Control Panel's language. They were the document viewer's own: a copy with RTF
         // and XAML beside the text, in the theme's colours (measured: Text, UnicodeText,
         // Rich Text Format and Xaml on the clipboard), and a menu of WPF's, "_Copy" and
         // "Select A_ll" in English (measured in German and Swedish too).
         AutomationProperties.SetAutomationId(viewer_, "help-body");
         viewer_.ContextMenu = SelectableText.CopyMenu(CopySelection_, () => SelectedText_().Length > 0,
            () => ApplicationCommands.SelectAll.Execute(null, viewer_));
         viewer_.AddHandler(CommandManager.PreviewExecutedEvent, new ExecutedRoutedEventHandler(OnViewerPreviewExecuted_));
         DataObject.AddSettingDataHandler(viewer_, OnViewerSettingData_);
         Grid.SetRow(viewer_, 3);
         root.Children.Add(viewer_);

         // The foot: the wiki's copy of this page, and the whole set.
         var foot = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
         wikiButton_ = new Button { Content = L("Open on the _wiki"), Appearance = ControlAppearance.Secondary, Margin = new Thickness(0, 0, 8, 0), Visibility = Visibility.Collapsed };
         AutomationProperties.SetAutomationId(wikiButton_, "help-wiki");
         wikiButton_.Click += (s, e) => OpenWiki_();
         foot.Children.Add(wikiButton_);
         allButton_ = new Button { Content = L("_All documentation"), Appearance = ControlAppearance.Secondary };
         AutomationProperties.SetAutomationId(allButton_, "help-all");
         allButton_.Click += (s, e) => ShowAll();
         foot.Children.Add(allButton_);
         Grid.SetRow(foot, 4);
         root.Children.Add(foot);

         Content = root;
      }

      /// <summary>
      /// The help for a page of the navigation, by its key: the document and
      /// the heading the map names. A key the map does not know shows the list
      /// of every document, which is the most a pane can do for it.
      /// </summary>
      public void ShowPage(string navigationKey)
      {
         HelpTarget target = HelpMap.For(navigationKey);
         if (target == null)
         {
            ShowAll();
            return;
         }

         ShowDocument(target.Name, target.Heading);
      }

      /// <summary>
      /// One document, opened on a heading; the document already showing is
      /// only scrolled. The scroll target is <see cref="MarkdownBlocks.IndexOfHeading"/>'s
      /// answer: the heading's block when the document has it, the top when it
      /// does not or none is named.
      /// </summary>
      public void ShowDocument(string name, string heading)
      {
         string spelled = HelpPages.Find(name);
         if (spelled == null)
         {
            shown_ = null;
            headings_.Children.Clear();
            headingBlocks_ = new Dictionary<string, Block>();
            title_.Text = name ?? "";
            viewer_.Document = new FlowDocument(new Paragraph(new Run(L("This document is not in this copy of the program."))));
            UpdateWikiLink_();
            return;
         }

         if (!string.Equals(shown_, spelled, StringComparison.Ordinal))
         {
            string text = HelpPages.Read(spelled) ?? "";
            IReadOnlyList<MdBlock> blocks = MarkdownBlocks.Parse(text);
            MarkdownFlow.Built built = MarkdownFlow.Build(blocks, (doc, fragment) => ShowDocument(doc, HeadingForFragment_(doc, fragment)));
            viewer_.Document = built.Document;
            headingBlocks_ = built.Headings;
            shown_ = spelled;
            search_.Text = "";

            IReadOnlyList<MdHeading> list = MarkdownBlocks.Headings(text);
            title_.Text = list.Count > 0 && list[0].Level == 1 ? MarkdownBlocks.PlainText(list[0].Text) : spelled;
            headings_.Children.Clear();
            for (int i = 0; i < list.Count; i++)
            {
               if (i == 0 && list[i].Level == 1)
                  continue;
               headings_.Children.Add(HeadingLink_(list[i]));
            }
            UpdateWikiLink_();
         }

         ScrollToHeading_(heading);
      }

      /// <summary>Every document by its title, each a link that opens it here.</summary>
      public void ShowAll()
      {
         shown_ = null;
         headings_.Children.Clear();
         headingBlocks_ = new Dictionary<string, Block>();
         search_.Text = "";
         title_.Text = L("All documentation");
         UpdateWikiLink_();

         var document = new FlowDocument { FontFamily = new System.Windows.Media.FontFamily(Typography.UiFontFamily), FontSize = Typography.Body, PagePadding = new Thickness(0) };
         document.SetResourceReference(FlowDocument.ForegroundProperty, "TextFillColorPrimaryBrush");
         foreach (string name in HelpPages.Names)
         {
            var link = new Hyperlink(new Run(HelpPages.TitleOf(name))) { ToolTip = name + ".md" };
            string opened = name;
            link.Click += (s, e) => ShowDocument(opened, null);
            var paragraph = new Paragraph { Margin = new Thickness(0, 0, 0, 6) };
            paragraph.Inlines.Add(link);
            paragraph.Inlines.Add(new LineBreak());
            var file = new Run(name + ".md") { FontSize = Typography.Caption };
            file.SetResourceReference(TextElement.ForegroundProperty, "TextFillColorTertiaryBrush");
            paragraph.Inlines.Add(file);
            document.Blocks.Add(paragraph);
         }
         viewer_.Document = document;
      }

      /// <summary>
      /// A heading of the list at the top. The label is the heading's text with
      /// its inline markup taken off - `What it does *not* do` as What it does
      /// not do - while the scroll target stays keyed by the raw text, which is
      /// how the map names a heading and how MarkdownFlow keeps its block.
      /// </summary>
      private FrameworkElement HeadingLink_(MdHeading heading)
      {
         string label = MarkdownBlocks.PlainText(heading.Text);
         var button = new Button
         {
            Content = label,
            Appearance = ControlAppearance.Transparent,
            FontSize = Typography.Caption,
            Padding = new Thickness(heading.Level == 2 ? 14 : 4, 2, 4, 2),
            HorizontalAlignment = HorizontalAlignment.Left,
            HorizontalContentAlignment = HorizontalAlignment.Left,
            Cursor = Cursors.Hand
         };
         AutomationProperties.SetName(button, label);
         string text = heading.Text;
         button.Click += (s, e) => ScrollToHeading_(text);
         return button;
      }

      private void ScrollToHeading_(string heading)
      {
         if (!string.IsNullOrEmpty(heading) && headingBlocks_.TryGetValue(heading, out Block block))
         {
            block.BringIntoView();
            return;
         }

         if (viewer_.Document != null)
            viewer_.Document.ContentStart.Paragraph?.BringIntoView();
      }

      /// <summary>
      /// A link inside a document names a heading as its slug (a-heading-like-this);
      /// the pane names headings by their text. The first heading whose slug is
      /// the link's is the one, else the top.
      /// </summary>
      private static string HeadingForFragment_(string doc, string fragment)
      {
         if (string.IsNullOrEmpty(fragment))
            return null;

         string text = HelpPages.Read(doc);
         if (text == null)
            return null;

         foreach (MdHeading heading in MarkdownBlocks.Headings(text))
         {
            if (string.Equals(Slug_(heading.Text), fragment, StringComparison.OrdinalIgnoreCase))
               return heading.Text;
         }
         return null;
      }

      private static string Slug_(string text)
      {
         var slug = new System.Text.StringBuilder();
         bool dash = false;
         foreach (char ch in (text ?? "").ToLowerInvariant())
         {
            if (ch == '`')
               continue;
            if (char.IsLetterOrDigit(ch) || ch == '_')
            {
               slug.Append(ch);
               dash = false;
            }
            else if (!dash && slug.Length > 0)
            {
               slug.Append('-');
               dash = true;
            }
         }
         return slug.ToString().TrimEnd('-');
      }

      private void UpdateWikiLink_()
      {
         wikiButton_.Visibility = string.IsNullOrEmpty(wikiBaseUrl_) || shown_ == null ? Visibility.Collapsed : Visibility.Visible;
         wikiButton_.ToolTip = shown_ == null ? null : wikiBaseUrl_ + shown_;
      }

      private void OpenWiki_()
      {
         if (string.IsNullOrEmpty(wikiBaseUrl_) || shown_ == null)
            return;

         try
         {
            Process.Start(new ProcessStartInfo(wikiBaseUrl_ + shown_) { UseShellExecute = true });
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            Services.Toast.Failure(L("The wiki could not be opened in a browser."), L("Help"));
         }
      }

      /// <summary>The words selected in the document, as plain text; empty when none are.</summary>
      private string SelectedText_()
         => viewer_.Selection == null || viewer_.Selection.IsEmpty ? "" : viewer_.Selection.Text ?? "";

      private void CopySelection_()
      {
         string text = SelectedText_();
         if (text.Length > 0)
            SelectableText.SetClipboard(text);
      }

      /// <summary>
      /// Ctrl+C (and Ctrl+Insert) with the keyboard in the document: the selection as
      /// plain text, before the viewer's own Copy can put rich text with it.
      /// </summary>
      private void OnViewerPreviewExecuted_(object sender, ExecutedRoutedEventArgs e)
      {
         if (e.Handled || e.Command != ApplicationCommands.Copy)
            return;

         CopySelection_();
         e.Handled = true;
      }

      /// <summary>A selection dragged out of the document carries its text and nothing else.</summary>
      private static void OnViewerSettingData_(object sender, DataObjectSettingDataEventArgs e)
      {
         if (e.Format != DataFormats.UnicodeText && e.Format != DataFormats.Text)
            e.CancelCommand();
      }

      private void Find_(bool next)
      {
         if (viewer_.Document == null)
            return;

         string term = search_.Text.Trim();
         if (term.Length < 2)
            return;

         TextPointer from = next && viewer_.Selection != null && !viewer_.Selection.IsEmpty
            ? viewer_.Selection.End
            : viewer_.Document.ContentStart;

         TextRange hit = FindText_(from, term) ?? (next ? FindText_(viewer_.Document.ContentStart, term) : null);
         if (hit == null)
            return;

         viewer_.Selection?.Select(hit.Start, hit.End);
         (hit.Start.Parent as FrameworkContentElement)?.BringIntoView();
      }

      private static TextRange FindText_(TextPointer from, string term)
      {
         TextPointer at = from;
         while (at != null)
         {
            if (at.GetPointerContext(LogicalDirection.Forward) == TextPointerContext.Text)
            {
               string run = at.GetTextInRun(LogicalDirection.Forward);
               int index = run.IndexOf(term, StringComparison.CurrentCultureIgnoreCase);
               if (index >= 0)
               {
                  TextPointer start = at.GetPositionAtOffset(index);
                  TextPointer end = start?.GetPositionAtOffset(term.Length);
                  if (start != null && end != null)
                     return new TextRange(start, end);
               }
            }
            at = at.GetNextContextPosition(LogicalDirection.Forward);
         }
         return null;
      }
   }
}
