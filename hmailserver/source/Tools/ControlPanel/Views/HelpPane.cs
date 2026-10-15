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
   /// which is what the server serves at /api/v1/help and what many of the
   /// wiki's pages are written from, under names of their own; the page is
   /// mapped to its document and heading
   /// by <see cref="HelpMap"/>, read by <see cref="MarkdownBlocks"/> and drawn
   /// by <see cref="MarkdownFlow"/> into a FlowDocumentScrollViewer, scrolled
   /// to the heading, with the document's other headings as a list at the
   /// top, a search box over the open document, and two links at the foot:
   /// the project wiki's page made from the document, where it has one
   /// (<see cref="HelpPages.WikiUrl"/>), and every document by its title, grouped as
   /// the sidebar is, with Back to what a link in the pane left (HelpPane.List.cs).
   /// The shell (MainWindow) owns the pane's column, its width and whether it is
   /// open, and tells it the page as the navigation changes.
   /// </summary>
   public partial class HelpPane : UserControl
   {
      private readonly TextBlock title_ = new TextBlock { FontSize = Typography.Body, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
      private readonly TextBox search_ = new TextBox { FontSize = Typography.Control, ClearButtonEnabled = true, Margin = new Thickness(0, 8, 0, 8) };

      // What the search found, under its box: "Match 2 of 5 - press Enter for the next."
      // or "No match for "qqzz" in this document.", where a term that matched
      // nothing left the pane as it was and said nothing (review finding F027). A
      // polite live region named by its text, so that a screen reader hears it
      // as it changes. Four pixels under the box, which keeps eight under it.
      private readonly TextBlock findStatus_ = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, -DesignTokens.Space.Xs, 0, DesignTokens.Space.Sm), Visibility = Visibility.Collapsed };
      private readonly StackPanel headings_ = new StackPanel();
      private readonly ScrollViewer headingsScroll_ = new ScrollViewer { MaxHeight = 190, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
      private readonly FlowDocumentScrollViewer viewer_ = new FlowDocumentScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, IsToolBarVisible = false, Padding = new Thickness(0, 0, 8, 0) };
      private readonly Button wikiButton_;
      private readonly Button allButton_;
      private readonly Button closeButton_;

      private string shown_;

      // The page whose "no guide" the pane shows, or null; so that a page that has
      // none is not drawn again as its tabs change.
      private string noGuideFor_;
      private IReadOnlyDictionary<string, Block> headingBlocks_ = new Dictionary<string, Block>();

      /// <summary>
      /// The wiki base URL "Open on the wiki" is made against, with the project
      /// wiki's page for the document added, whatever the base; empty hides the
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

         // The head: Back while a link in the pane has shown something else, the
         // document's title, and the close button.
         var head = new Grid();
         head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         head.Children.Add(BuildBackButton_());
         title_.Text = L("Help");
         title_.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
         AutomationProperties.SetAutomationId(title_, "help-title");
         AutomationProperties.SetHeadingLevel(title_, AutomationHeadingLevel.Level2);
         Grid.SetColumn(title_, 1);
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
         Grid.SetColumn(closeButton_, 2);
         head.Children.Add(closeButton_);
         root.Children.Add(head);

         // The search over the open document: typing finds the first match,
         // Enter the next, round again from the top. While the list of every
         // document shows it searches all of them, the line under it saying how
         // many it lists and Enter opening the first (HelpPane.List.cs).
         SearchLabel_();
         AutomationProperties.SetAutomationId(search_, "help-search");
         search_.TextChanged += (s, e) => SearchChanged_();
         search_.KeyDown += (s, e) =>
         {
            if (e.Key == Key.Enter)
            {
               SearchEnter_();
               e.Handled = true;
            }
         };
         findStatus_.SetResourceReference(StyleProperty, "TextCaption");
         AutomationProperties.SetAutomationId(findStatus_, "help-search-result");
         AutomationProperties.SetLiveSetting(findStatus_, AutomationLiveSetting.Polite);
         var searchBlock = new StackPanel();
         searchBlock.Children.Add(search_);
         searchBlock.Children.Add(findStatus_);
         Grid.SetRow(searchBlock, 1);
         root.Children.Add(searchBlock);

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

         // The foot: the wiki's page made from this document, where there is one,
         // and the whole set.
         var foot = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
         wikiButton_ = new Button { Content = L("Open on the _wiki"), Appearance = ControlAppearance.Secondary, Margin = new Thickness(0, 0, 8, 0), Visibility = Visibility.Collapsed };
         AutomationProperties.SetAutomationId(wikiButton_, "help-wiki");
         wikiButton_.Click += (s, e) => OpenWiki_();
         foot.Children.Add(wikiButton_);
         allButton_ = new Button { Content = L("_All documentation"), Appearance = ControlAppearance.Secondary };
         AutomationProperties.SetAutomationId(allButton_, "help-all");
         // Pressed, it shows the list and cannot be pressed again there
         // (ShowAll): the keyboard goes to the search over the list rather than
         // being left on a button that has just been disabled.
         allButton_.Click += (s, e) =>
         {
            bool hadKeyboard = allButton_.IsKeyboardFocused;
            GoTo_(ShowAll);
            if (hadKeyboard)
               search_.Focus();
         };
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
      public void ShowPage(string navigationKey) => ShowPage(navigationKey, null);

      /// <summary>
      /// The help for a page on one of its tabs, by the tab's English header: the
      /// tab's document where the map has one (HelpMap.Tabs), the page's
      /// otherwise. A page no document is about says so beside its purpose and the
      /// list of every document, rather than open one about something else.
      /// </summary>
      public void ShowPage(string navigationKey, string tab)
      {
         ForgetHistory_();
         HelpTarget target = HelpMap.For(navigationKey, tab);
         if (target == null)
         {
            ShowAll();
            return;
         }

         if (!target.IsGuide)
         {
            ShowNoGuide_(navigationKey);
            return;
         }

         ShowDocument(target.Name, target.Heading);
      }

      /// <summary>
      /// A page with no guide of its own: its title, what it is for, that no
      /// document covers it yet, and every document by its title, each a link
      /// that opens it here (review finding F270).
      /// </summary>
      private void ShowNoGuide_(string navigationKey)
      {
         if (shown_ == null && string.Equals(noGuideFor_, navigationKey, StringComparison.OrdinalIgnoreCase))
            return;

         NavNode page = NavigationMap.Find(navigationKey);
         shown_ = null;
         SearchLabel_();
         noGuideFor_ = navigationKey;
         headings_.Children.Clear();
         headingBlocks_ = new Dictionary<string, Block>();
         search_.Text = "";
         title_.Text = page == null ? L("Help") : L(page.Title);
         UpdateWikiLink_();
         // Every document is listed below already.
         allButton_.IsEnabled = false;

         List<Block> intro = NoGuideIntro_(page);
         ShowFromTheTop_(DocumentList_(intro));
      }

      /// <summary>
      /// One document, opened on a heading; the document already showing is
      /// only scrolled. The scroll target is <see cref="MarkdownBlocks.IndexOfHeading"/>'s
      /// answer: the heading's block when the document has it, the top when it
      /// does not or none is named.
      /// </summary>
      public void ShowDocument(string name, string heading)
      {
         noGuideFor_ = null;
         allButton_.IsEnabled = true;
         string spelled = HelpPages.Find(name);
         if (spelled == null)
         {
            shown_ = null;
            SearchLabel_();
            headings_.Children.Clear();
            headingBlocks_ = new Dictionary<string, Block>();
            // The search goes with the document it was made in, and the line under
            // it with it, as every other change of what the pane shows clears them:
            // left, "Match 2 of 5 - press Enter for the next." stood over a
            // document that is not there.
            search_.Text = "";
            title_.Text = name ?? "";
            FlowDocument missing = MarkdownFlow.NewDocument();
            missing.Blocks.Add(new Paragraph(new Run(L("This document is not in this copy of the program."))));
            ShowFromTheTop_(missing);
            UpdateWikiLink_();
            return;
         }

         if (!string.Equals(shown_, spelled, StringComparison.Ordinal))
         {
            string text = HelpPages.Read(spelled) ?? "";
            IReadOnlyList<MdBlock> blocks = MarkdownBlocks.Parse(text);
            MarkdownFlow.Built built = MarkdownFlow.Build(blocks, (doc, fragment) => GoTo_(() => ShowDocument(doc, HeadingForFragment_(doc, fragment))));
            headingBlocks_ = built.Headings;
            ShowFromTheTop_(built.Document);
            shown_ = spelled;
            SearchLabel_();
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
         SearchLabel_();
         noGuideFor_ = null;
         headings_.Children.Clear();
         headingBlocks_ = new Dictionary<string, Block>();
         search_.Text = "";
         title_.Text = L("All documentation");
         UpdateWikiLink_();
         // The list is what the button shows: pressed again it rebuilt the same
         // list (review finding F027). A document opened from it enables it again.
         allButton_.IsEnabled = false;
         ShowFromTheTop_(DocumentList_(null));
      }

      /// <summary>
      /// A document listing every document by its title and file name, each title
      /// a link that opens it here, after the <paramref name="before"/> blocks:
      /// under the sidebar's groups, then Reference and Building and packaging,
      /// and only the documents the search holds (AddDocumentSections_).
      /// </summary>
      private FlowDocument DocumentList_(IEnumerable<Block> before)
      {
         // Ragged right and unhyphenated, as a document the pane renders is
         // (MarkdownFlow.NewDocument): a FlowDocument justifies by default, which
         // spread a long title's words.
         FlowDocument document = MarkdownFlow.NewDocument();
         foreach (Block block in before ?? Array.Empty<Block>())
            document.Blocks.Add(block);
         AddDocumentSections_(document);
         return document;
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
         // 24 pixels tall at least, the target WCAG 2.5.8 asks for: with two
         // pixels above and below its 12-pixel words a link was about 20 (review
         // finding F027).
         var button = new Button
         {
            Content = label,
            Appearance = ControlAppearance.Transparent,
            FontSize = Typography.Caption,
            Padding = new Thickness(heading.Level == 2 ? 14 : 4, 4, 4, 4),
            MinHeight = 24,
            HorizontalAlignment = HorizontalAlignment.Left,
            HorizontalContentAlignment = HorizontalAlignment.Left,
            Cursor = Cursors.Hand
         };
         AutomationProperties.SetName(button, label);
         string text = heading.Text;
         button.Click += (s, e) => ScrollToHeading_(text);
         return button;
      }

      /// <summary>
      /// Gives the viewer a new document, at its top. The viewer's ScrollViewer
      /// keeps its offset when the document is replaced, so a document opened after
      /// another opened at the other's place - the foot of the list of documents on a
      /// page with no guide after a long document's heading, the middle of the SNMP
      /// document when the Logging page's SNMP tab was chosen after the syslog
      /// document had been scrolled. The offset is set back on the ScrollViewer
      /// itself: a block's BringIntoView, which this asked of the first block, does
      /// nothing until the new document has been laid out.
      /// </summary>
      private void ShowFromTheTop_(FlowDocument document)
      {
         viewer_.Document = document;
         DocumentScroller_()?.ScrollToHome();
      }

      /// <summary>
      /// Scrolls the document showing so that a heading is at the top of the pane,
      /// or to the document's top when the heading is not one of its own or none is
      /// named. It is done at Loaded priority, once the layout has run: asked
      /// straight after <see cref="ShowFromTheTop_"/>, it would find nothing,
      /// because the viewer knows where a block is only in a document it has laid
      /// out - which is why a page's heading was reached only when its document was
      /// already showing. The heading's line is put at the top by the ScrollViewer's
      /// offset, not by the block's BringIntoView, which scrolls as little as shows
      /// the block and so leaves a heading already on screen where it is: the
      /// anti-spam page's Greylisting tab opened its document at the top with "What
      /// greylisting does" halfway down. When another document has replaced this
      /// one by then, that one is left as it is.
      /// </summary>
      private void ScrollToHeading_(string heading)
      {
         FlowDocument document = viewer_.Document;
         if (document == null)
            return;

         Block block = null;
         if (!string.IsNullOrEmpty(heading))
            headingBlocks_.TryGetValue(heading, out block);

         Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() =>
         {
            if (!ReferenceEquals(viewer_.Document, document))
               return;

            ScrollViewer scroller = DocumentScroller_();
            if (block == null)
            {
               scroller?.ScrollToHome();
               return;
            }

            // Where the heading's first line is in the viewport, which the viewer
            // scrolls itself: 0 at its top, negative above it.
            Rect line = block.ContentStart.GetCharacterRect(LogicalDirection.Forward);
            if (scroller != null && !line.IsEmpty)
               scroller.ScrollToVerticalOffset(scroller.VerticalOffset + line.Top);
            else
               block.BringIntoView();
         }));
      }

      /// <summary>
      /// The viewer's own ScrollViewer, the one in its template; null before the
      /// template is applied, when the viewer has never been scrolled.
      /// </summary>
      private ScrollViewer DocumentScroller_()
      {
         if (viewer_.Template?.FindName("PART_ContentHost", viewer_) is ScrollViewer part)
            return part;

         var pending = new Stack<DependencyObject>();
         pending.Push(viewer_);
         while (pending.Count > 0)
         {
            DependencyObject node = pending.Pop();
            if (node is ScrollViewer found)
               return found;
            for (int i = System.Windows.Media.VisualTreeHelper.GetChildrenCount(node) - 1; i >= 0; i--)
               pending.Push(System.Windows.Media.VisualTreeHelper.GetChild(node, i));
         }
         return null;
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

      /// <summary>
      /// The project wiki's page for the document showing, or null: the base with
      /// the page the project's wiki made from the document (HelpPages.WikiUrl), so a
      /// document it has no page for, the list of documents and a page with no guide have
      /// no wiki link, rather than one to GitLab's "page does not exist".
      /// </summary>
      private string WikiUrl_() => shown_ == null ? null : HelpPages.WikiUrl(wikiBaseUrl_, shown_);

      private void UpdateWikiLink_()
      {
         string url = WikiUrl_();
         wikiButton_.Visibility = url == null ? Visibility.Collapsed : Visibility.Visible;
         wikiButton_.ToolTip = url;
      }

      private void OpenWiki_()
      {
         string url = WikiUrl_();
         if (url == null)
            return;

         try
         {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
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
         {
            ShowFindStatus_(null);
            return;
         }

         string term = search_.Text.Trim();
         if (term.Length < 2)
         {
            ShowFindStatus_(null);
            return;
         }

         TextPointer from = next && viewer_.Selection != null && !viewer_.Selection.IsEmpty
            ? viewer_.Selection.End
            : viewer_.Document.ContentStart;

         TextRange hit = FindText_(from, term) ?? (next ? FindText_(viewer_.Document.ContentStart, term) : null);
         if (hit == null)
         {
            ShowFindStatus_(F("No match for \"{0}\" in this document.", term));
            return;
         }

         viewer_.Selection?.Select(hit.Start, hit.End);
         (hit.Start.Parent as FrameworkContentElement)?.BringIntoView();

         // Which of the document's matches this is, counted the way Find_ moves
         // through them: from the top, one run at a time.
         int total = 0;
         int at = 0;
         for (TextRange match = FindText_(viewer_.Document.ContentStart, term); match != null; match = FindText_(match.End, term))
         {
            total++;
            if (at == 0 && match.Start.CompareTo(hit.Start) >= 0)
               at = total;
         }
         ShowFindStatus_(F("Match {0} of {1} - press Enter for the next.", Math.Max(at, 1), Math.Max(total, 1)));
      }

      /// <summary>The line under the search box: what it found, or nothing while there is no term to find.</summary>
      private void ShowFindStatus_(string text)
      {
         findStatus_.Text = text ?? "";
         findStatus_.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
         if (!string.IsNullOrEmpty(text))
            Announce.Request(findStatus_);
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
