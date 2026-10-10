// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Documents;
using Wpf.Ui.Controls;
using hMailServer.ControlPanel.Services;
using Button = Wpf.Ui.Controls.Button;
using Typography = hMailServer.ControlPanel.Services.Typography;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The help pane's list of every document and its way back (review finding
   /// F315). The list was some 120 documents by title in one run, the project's own
   /// - ContinuousIntegration, Fuzzing, Nix - among the operators' guides; the
   /// search box over it said "Search this document" and searched the list's own
   /// words; and a document opened from the list, or from a link in another
   /// document, had no way back. The list is grouped as the sidebar is now, then
   /// Reference and Building and packaging, each heading's documents by title
   /// (<see cref="HelpDocumentList"/>); while it shows, the box is "Search all
   /// documentation" and narrows it to the documents whose title or text holds
   /// the words, Enter opening the first; and Back, at the head of the pane while
   /// a link in it has shown something else, returns to what was shown before.
   /// </summary>
   public partial class HelpPane
   {
      // What the pane showed before each link in it showed something else, newest
      // on top: a document's name, or null for the list of every document - with
      // the page that has no guide, when the list was that page's. A page's help,
      // which the shell asks for, starts it again.
      private readonly Stack<(string Doc, string NoGuide)> back_ = new();
      private Button backButton_;

      // Each document's title and text, read once: the list's search reads every
      // one of them as each character is typed.
      private static readonly Dictionary<string, string> titles_ = new(StringComparer.OrdinalIgnoreCase);
      private static readonly Dictionary<string, string> texts_ = new(StringComparer.OrdinalIgnoreCase);

      /// <summary>Back, at the start of the pane's head; there only while it has somewhere to go.</summary>
      private Button BuildBackButton_()
      {
         backButton_ = new Button
         {
            Appearance = ControlAppearance.Transparent,
            Icon = new SymbolIcon(SymbolRegular.ArrowLeft24),
            Padding = new Thickness(6, 4, 6, 4),
            Margin = new Thickness(0, 0, DesignTokens.Space.Xs, 0),
            Visibility = Visibility.Collapsed
         };
         AutomationProperties.SetAutomationId(backButton_, "help-back");
         backButton_.Click += (s, e) => Back_();
         return backButton_;
      }

      /// <summary>What the pane shows, as Back keeps it.</summary>
      private (string Doc, string NoGuide) State_() => (shown_, shown_ == null ? noGuideFor_ : null);

      /// <summary>
      /// Shows something else from a link in the pane, and keeps what was shown for
      /// Back - unless it is shown still, as after a link to a heading of the same
      /// document.
      /// </summary>
      private void GoTo_(Action show)
      {
         (string Doc, string NoGuide) before = State_();
         show();
         if (State_() != before)
            back_.Push(before);
         UpdateBack_();
      }

      private void Back_()
      {
         if (back_.Count == 0)
            return;

         (string doc, string noGuide) = back_.Pop();
         if (doc != null)
            ShowDocument(doc, null);
         else if (noGuide != null)
            ShowNoGuide_(noGuide);
         else
            ShowAll();
         UpdateBack_();
      }

      /// <summary>The shell's page starts the pane's history again: Back is for the links in the pane.</summary>
      private void ForgetHistory_()
      {
         back_.Clear();
         UpdateBack_();
      }

      /// <summary>Back, named by where it goes - "Back to All documentation" - or hidden with nowhere to go.</summary>
      private void UpdateBack_()
      {
         if (backButton_ == null)
            return;

         if (back_.Count == 0)
         {
            backButton_.Visibility = Visibility.Collapsed;
            return;
         }

         (string doc, string noGuide) = back_.Peek();
         string where = doc != null
            ? TitleOf_(doc)
            : noGuide != null ? L(NavigationMap.TitleOf(noGuide)) : L("All documentation");
         string name = F("Back to {0}", where);
         backButton_.ToolTip = name;
         AutomationProperties.SetName(backButton_, name);
         backButton_.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// The search box says what it searches: the open document, or - while the
      /// list of every document shows - all of them, where it said "Search this
      /// document" over the list as well.
      /// </summary>
      private void SearchLabel_()
      {
         string label = shown_ == null ? L("Search all documentation") : L("Search this document");
         search_.PlaceholderText = label;
         AutomationProperties.SetName(search_, label);
      }

      /// <summary>A key in the search: the next match in the document, or the list narrowed to the words.</summary>
      private void SearchChanged_()
      {
         if (shown_ != null)
            Find_(false);
         else
            FilterList_();
      }

      /// <summary>Enter in the search: the next match in the document, or the first document the list holds.</summary>
      private void SearchEnter_()
      {
         if (shown_ != null)
         {
            Find_(true);
            return;
         }

         foreach (HelpDocumentSection section in Sections_(search_.Text.Trim()))
         {
            if (section.Names.Count > 0)
            {
               string first = section.Names[0];
               GoTo_(() => ShowDocument(first, null));
               return;
            }
         }
      }

      /// <summary>The list again, narrowed to what the search holds, under the page's own words when it has no guide.</summary>
      private void FilterList_()
      {
         List<Block> intro = noGuideFor_ != null ? NoGuideIntro_(NavigationMap.Find(noGuideFor_)) : null;
         ShowFromTheTop_(DocumentList_(intro));
      }

      /// <summary>What the pane says above the list for a page with no guide: its purpose, and that no guide covers it.</summary>
      private static List<Block> NoGuideIntro_(NavNode page)
      {
         var intro = new List<Block>();
         if (page != null && !string.IsNullOrEmpty(page.Purpose))
            intro.Add(new Paragraph(new Run(L(page.Purpose))) { Margin = new Thickness(0, 0, 0, 8) });
         var none = new Paragraph(new Run(L("No guide covers this page yet. Every document is listed below."))) { Margin = new Thickness(0, 0, 0, 14) };
         none.SetResourceReference(TextElement.ForegroundProperty, "TextFillColorSecondaryBrush");
         intro.Add(none);
         return intro;
      }

      /// <summary>
      /// The list's headings and documents into <paramref name="document"/>: each
      /// heading, then its documents by title and file name, each title a link that
      /// opens it here - only the documents the search holds while it holds two
      /// characters or more, and a sentence saying so where none does.
      /// </summary>
      private void AddDocumentSections_(FlowDocument document)
      {
         string term = search_.Text.Trim();
         IReadOnlyList<HelpDocumentSection> sections = Sections_(term);
         if (sections.Count == 0)
         {
            var nothing = new Paragraph(new Run(F("No document contains \"{0}\".", term)));
            nothing.SetResourceReference(TextElement.ForegroundProperty, "TextFillColorSecondaryBrush");
            document.Blocks.Add(nothing);
            return;
         }

         bool first = true;
         foreach (HelpDocumentSection section in sections)
         {
            var heading = new Paragraph(new Run(L(section.Heading)))
            {
               FontWeight = FontWeights.SemiBold,
               Margin = new Thickness(0, first ? 0 : DesignTokens.Space.Lg, 0, 6)
            };
            document.Blocks.Add(heading);
            first = false;

            foreach (string name in section.Names)
            {
               var link = new Hyperlink(new Run(TitleOf_(name))) { ToolTip = name + ".md" };
               string opened = name;
               link.Click += (s, e) => GoTo_(() => ShowDocument(opened, null));
               var paragraph = new Paragraph { Margin = new Thickness(0, 0, 0, 6) };
               paragraph.Inlines.Add(link);
               paragraph.Inlines.Add(new LineBreak());
               var file = new Run(name + ".md") { FontSize = Typography.Caption };
               file.SetResourceReference(TextElement.ForegroundProperty, "TextFillColorTertiaryBrush");
               paragraph.Inlines.Add(file);
               document.Blocks.Add(paragraph);
            }
         }
      }

      private static IReadOnlyList<HelpDocumentSection> Sections_(string term = null)
         => HelpDocumentList.Sections(HelpPages.Names, TitleOf_, TextOf_, term);

      private static string TitleOf_(string name)
      {
         if (!titles_.TryGetValue(name, out string title))
            titles_[name] = title = HelpPages.TitleOf(name);
         return title;
      }

      private static string TextOf_(string name)
      {
         if (!texts_.TryGetValue(name, out string text))
            texts_[name] = text = HelpPages.Read(name) ?? "";
         return text;
      }
   }
}
