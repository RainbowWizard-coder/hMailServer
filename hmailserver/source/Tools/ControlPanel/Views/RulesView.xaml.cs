// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class RulesView : UserControl, IPageLifecycle, IRefreshablePage
   {
      public class RuleRow
      {
         public int Id { get; set; }
         public int Position { get; set; }
         public string Name { get; set; }

         // The rule in one line, its first criterion and its first action, so
         // that the rule that forwards is found without opening each (review
         // finding F309); the page's search reads it as it reads the name.
         public string Summary { get; set; }
         // On or Off, in the Enabled column's pill at its level: Good for a rule
         // that runs, Normal for one switched off (review finding F323), whose row
         // the grid draws in the secondary text (its CellStyle).
         public bool Active { get; set; }
         public string Enabled => Active ? L("On") : L("Off");
         public StatusLevel EnabledLevel => Active ? StatusLevel.Good : StatusLevel.Normal;

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString() => string.IsNullOrEmpty(Summary) ? Name + ", " + Enabled : Name + ", " + Summary + ", " + Enabled;
      }

      public class DetailRow
      {
         public int Id { get; set; }
         public string Description { get; set; }

         public override string ToString() => Description;
      }

      private int selectedRuleId_;
      private bool suppressMatchMode_;
      private Func<dynamic> rulesProvider_ = () => ServerSession.Current.Application.Rules;
      private bool serverLevel_ = true;

      /// <summary>
      /// Re-targets this view at a different rule collection (e.g. an account's
      /// rules) and hides the page header when embedded in another dialog.
      /// </summary>
      public void ConfigureForRules(Func<dynamic> rulesProvider, bool serverLevel, bool embedded)
      {
         rulesProvider_ = rulesProvider;
         serverLevel_ = serverLevel;
         if (embedded && HeaderPanel != null)
         {
            HeaderPanel.Visibility = Visibility.Collapsed;

            // On a dialog's tab the dialog's Save is the one Primary button, and
            // the tab has the dialog's own padding: the page's three Primary adds
            // stood beside that Save, and the page's 24-pixel padding inside the
            // tab's put the rules 24 pixels further in than every other tab's
            // content (review finding F195).
            AddRuleButton.Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary;
            AddCriterionButton.Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary;
            AddActionButton.Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary;
            if (HeaderPanel.Parent is Grid page)
               page.Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0);

            // On the account dialog's Rules tab, whose Save and Cancel do not reach
            // the rules: each is written as it is made, and the tab says so where the
            // page's header was (review finding F196).
            if (HeaderPanel.Parent is Grid root)
            {
               System.Windows.Controls.TextBlock note = CollectionEditorView.SavedAsMadeNote();
               note.Margin = new Thickness(0, 0, 0, 8);
               Grid.SetRow(note, 0);
               root.Children.Add(note);
            }
         }
      }

      private dynamic OpenRules() => rulesProvider_();

      public RulesView()
      {
         InitializeComponent();
         // Every button here acts on a selected rule, criterion or action: Add
         // criterion… and Add action… as well, and the match list, which were live
         // with no rule and answered "Select a rule first." in a message box (review
         // finding F225).
         hMailServer.ControlPanel.Services.SelectionGate.Bind(RuleGrid, MoveUpButton, MoveDownButton, ToggleButton, DeleteRuleButton,
            AddCriterionButton, AddActionButton, MatchMode);
         hMailServer.ControlPanel.Services.SelectionGate.Bind(CriteriaGrid, EditCriterionButton, RemoveCriterionButton);
         hMailServer.ControlPanel.Services.SelectionGate.Bind(ActionsGrid, EditActionButton, RemoveActionButton, ActionUpButton, ActionDownButton);

         // The three lists' keys (Views/GridKeys): Delete deletes the selected rule
         // after asking and Insert goes to the new rule's name; Enter and F2 edit a
         // criterion or an action, Delete removes it and Insert adds one. Enter in
         // the name adds the rule, and Escape empties it (Views/FormSubmit).
         GridKeys.Wire(RuleGrid, null, GridKeys.Pressing(DeleteRuleButton), GridKeys.Focusing(NewRuleName));
         GridKeys.Wire(CriteriaGrid, EditCriterionButton, RemoveCriterionButton, AddCriterionButton);
         GridKeys.Wire(ActionsGrid, EditActionButton, RemoveActionButton, AddActionButton);
         FormSubmit.Bind(AddRuleForm, AddRuleButton, NewRuleName);

         suppressMatchMode_ = true;
         MatchMode.Items.Add(L("Match all criteria (AND)"));
         MatchMode.Items.Add(L("Match any criterion (OR)"));
         MatchMode.SelectedIndex = 0;
         suppressMatchMode_ = false;

         ShowRuleParts_();

         // Alt+Up and Alt+Down move the selected action, as the glyph buttons
         // under the list do, whose captions no longer carry an access key.
         ActionsGrid.PreviewKeyDown += ActionsGrid_PreviewKeyDown;
      }

      private void ActionsGrid_PreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
      {
         if (System.Windows.Input.Keyboard.Modifiers != System.Windows.Input.ModifierKeys.Alt)
            return;

         System.Windows.Input.Key key = e.Key == System.Windows.Input.Key.System ? e.SystemKey : e.Key;
         if (key != System.Windows.Input.Key.Up && key != System.Windows.Input.Key.Down)
            return;

         e.Handled = true;
         if (ActionsGrid.SelectedItem is DetailRow)
         {
            MoveAction(key == System.Windows.Input.Key.Up);
            ActionsGrid.Focus();
         }
      }

      // The criteria and the actions side by side where each keeps 340 pixels -
      // the IF caption and its 230-pixel match list inside the card's padding -
      // and one above the other where they would not: side by side in the
      // narrowest window (760 by 520) and in the account dialog's Rules tab they
      // were 250 and 266 pixels wide, and the match list ran off its card.
      // And where either card's footer would not hold its buttons on one line: in German
      // the criteria's Edit, Remove and Add wrapped at 1280, Add alone on a second line.
      private const double RulePartMinWidth = 340;
      private static readonly double[] RulePartWeights = { 1, 1 };

      private void RuleParts_SizeChanged(object sender, SizeChangedEventArgs e)
      {
         double[] minWidths =
         {
            Math.Max(RulePartMinWidth, FooterOnOneLine_(CriteriaFooter, CriteriaCard)),
            Math.Max(RulePartMinWidth, FooterOnOneLine_(ActionsFooter, ActionsCard)),
         };
         bool sideBySide = SideBySide.Fits(e.NewSize.Width, RulePartWeights, minWidths);
         RuleParts.Columns = sideBySide ? 2 : 1;
         CriteriaCard.Margin = sideBySide ? new Thickness(0, 0, 6, 0) : new Thickness(0, 0, 0, 6);
         ActionsCard.Margin = sideBySide ? new Thickness(6, 0, 0, 0) : new Thickness(0, 6, 0, 0);
      }

      /// <summary>
      /// The width a card needs to hold its footer's buttons on one line: the buttons
      /// side by side with their margins, the card's padding on both sides, its border,
      /// and the six pixels between the two cards.
      /// </summary>
      private static double FooterOnOneLine_(Panel footer, Control card)
      {
         double line = 0;
         foreach (UIElement button in footer.Children)
         {
            if (button.Visibility == Visibility.Collapsed)
               continue;
            button.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            line += button.DesiredSize.Width;
         }

         return Math.Ceiling(line + card.Padding.Left + card.Padding.Right + 2 + 6);
      }

      public void OnEnter()
      {
         // A notice about a rule added before the page was left is done with.
         HideOutcome_();
         LoadScopes();
         Reload();
      }

      void IRefreshablePage.Refresh() => OnEnter();

      // The server's rules, or one domain's: the same page on Domain.Rules,
      // which is what a domain's rules are over COM. Hidden where the page is
      // embedded in the account dialog, where the scope is the account.
      private bool scopesLoaded_;
      private bool suppressScope_;

      // The domain whose rules the list shows, or null for the server's.
      private string scopeDomain_;

      private void LoadScopes()
      {
         if (ScopeBox == null)
            return;

         if (!serverLevel_)
         {
            ScopePanel.Visibility = Visibility.Collapsed;
            return;
         }

         if (scopesLoaded_)
            return;

         suppressScope_ = true;
         try
         {
            ScopeBox.Items.Clear();
            ScopeBox.Items.Add(L("The server"));
            dynamic domains = ServerSession.Current.Application.Domains;
            try
            {
               int count = (int)domains.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic d = domains.Item[i];
                  ScopeBox.Items.Add((string)d.Name);
                  ServerSession.Release(d);
               }
            }
            finally
            {
               ServerSession.Release(domains);
            }
            ScopeBox.SelectedIndex = 0;
            scopesLoaded_ = true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            ScopePanel.Visibility = Visibility.Collapsed;
         }
         finally
         {
            suppressScope_ = false;
         }
      }

      private void Scope_Changed(object sender, SelectionChangedEventArgs e)
      {
         if (suppressScope_ || !serverLevel_ || ScopeBox.SelectedIndex < 0)
            return;

         if (ScopeBox.SelectedIndex == 0)
         {
            scopeDomain_ = null;
            rulesProvider_ = () => ServerSession.Current.Application.Rules;
         }
         else
         {
            string domainName = (string)ScopeBox.SelectedItem;
            scopeDomain_ = domainName;
            rulesProvider_ = () =>
            {
               dynamic domains = ServerSession.Current.Application.Domains;
               try
               {
                  dynamic d = domains.ItemByName[domainName];
                  dynamic rules = d.Rules;
                  ServerSession.Release(d);
                  return rules;
               }
               finally
               {
                  ServerSession.Release(domains);
               }
            };
         }

         // The new rule's notice is about a rule of the list that is going.
         HideOutcome_();
         Reload();
      }

      public void OnLeave()
      {
      }

      private void Reload()
      {
         var rows = new List<RuleRow>();
         try
         {
            dynamic rules = OpenRules();
            try
            {
               int count = (int)rules.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic rule = rules.Item[i];
                  rows.Add(new RuleRow
                  {
                     Id = (int)rule.ID,
                     Position = i + 1,
                     Name = (string)rule.Name,
                     Summary = Summarise_(rule),
                     Active = (bool)rule.Active
                  });
                  ServerSession.Release(rule);
               }
            }
            finally
            {
               ServerSession.Release(rules);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "No rules defined yet" is a
            // fact this page has now: both go, with the criteria and actions of
            // the rule that was selected, the notice says what happened, with
            // Try again, and Add rule waits for a read that works (Services/ReadFailure).
            ReadFailure.Show(Notice, EmptyStatus, RuleGrid, L("Could not read the rules: {0}"), ex, Reload, AddRuleButton);
            CountPill.Visibility = Visibility.Collapsed;
            selectedRuleId_ = 0;
            CriteriaGrid.ItemsSource = null;
            ActionsGrid.ItemsSource = null;
            HeaderPanel.Subtitle = Subtitle_();
            HideOutcome_();
            ShowRuleParts_();
            return;
         }

         ReadFailure.Clear(Notice, AddRuleButton);
         RuleGrid.ItemsSource = rows;
         // The new rule's notice goes with the rule, deleted here or elsewhere.
         if (addedRuleId_ != 0 && !rows.Exists(r => r.Id == addedRuleId_))
            HideOutcome_();
         HeaderPanel.Subtitle = Subtitle_();
         CountPill.Visibility = Visibility.Visible;
         ShowListState_();

         CriteriaGrid.ItemsSource = null;
         ActionsGrid.ItemsSource = null;
         ShowRuleParts_();
      }

      /// <summary>
      /// The criteria and actions cards' empty states (review finding F225): with no
      /// rule selected, that a rule above is to be chosen; for a rule with no
      /// criteria, that it matches no message - with none to meet, the server never
      /// runs its actions (RuleApplier) - and with no actions, that it does nothing.
      /// The cards were blank whichever it was. None over a read that failed, whose
      /// notice says so.
      /// </summary>
      private void ShowRuleParts_()
      {
         bool failed = ReadFailure.IsShown(Notice);
         ShowPart_(CriteriaEmpty, CriteriaGrid, failed, RuleCards.CriteriaEmpty(selectedRuleId_ != 0));
         ShowPart_(ActionsEmpty, ActionsGrid, failed, RuleCards.ActionsEmpty(selectedRuleId_ != 0));
      }

      private static void ShowPart_(Scaffold.EmptyState empty, DataGrid grid, bool failed, string sentence)
      {
         bool none = !failed && (grid.ItemsSource == null || !grid.HasItems);
         empty.Text = none ? sentence : null;
         empty.Visibility = none ? Visibility.Visible : Visibility.Collapsed;
      }

      /// <summary>The switch says what it will do to the selected rule: Enable for one that is off, Disable for one that is on.</summary>
      private void ShowToggle_(RuleRow row)
      {
         if (row != null && row.Active)
            ToggleButton.Content = L("Di_sable");
         else
            ToggleButton.Content = L("Ena_ble");
      }

      /// <summary>What Add rule did, above the list, at its level, for the rule just added.</summary>
      private void SayOutcome_(StatusLevel level, string text)
      {
         OutcomeNotice.Level = level;
         OutcomeNotice.Text = text;
         OutcomeNotice.Visibility = Visibility.Visible;
         Announce.Request(OutcomeNotice);
      }

      private void HideOutcome_()
      {
         addedRuleId_ = 0;
         addedSwitchedOn_ = false;
         addedReady_ = false;
         OutcomeNotice.Content = null;
         OutcomeNotice.Visibility = Visibility.Collapsed;
      }

      // The rule Add rule just made, which the notice above the list is about, its
      // name, and whether the notice's Enable now has switched it on.
      private int addedRuleId_;
      private string addedRuleName_;
      private bool addedSwitchedOn_;
      private bool addedReady_;
      private Wpf.Ui.Controls.Button enableAddedButton_;

      /// <summary>
      /// The new rule's notice with Enable now (review finding F225): after Add rule the
      /// page is at the add form and the toolbar's Enable is scrolled out of view, so
      /// the notice that says to switch the rule on carries the button. It is pressed
      /// only once the rule has a criterion and an action (RuleCards.CanSwitchOn), and
      /// the notice says what it waits for; once the rule has both, the notice says
      /// so - afresh, to a screen reader too - and, as it comes to have both, puts the
      /// keyboard on Enable now, the rule's next step, with the notice in view: in a
      /// short window the page is at the criteria and actions then, the notice above
      /// the view. Shown again after each change to the rule's criteria and actions
      /// (RefreshDetails), while the rule is the selected one and has not been switched
      /// on from here.
      /// </summary>
      private void ShowAddedOutcome_()
      {
         if (addedRuleId_ == 0 || addedSwitchedOn_ || selectedRuleId_ != addedRuleId_)
            return;

         bool ready = RuleCards.CanSwitchOn(CriteriaGrid.Items.Count, ActionsGrid.Items.Count);
         bool becameReady = ready && !addedReady_;
         addedReady_ = ready;
         Wpf.Ui.Controls.Button enable = EnableAddedButton_();
         enable.IsEnabled = ready;
         string text = ready ? RuleCards.Ready(addedRuleName_) : RuleCards.Added(addedRuleName_);
         if (!(OutcomeNotice.Visibility == Visibility.Visible && ReferenceEquals(OutcomeNotice.Content, enable) && OutcomeNotice.Text == text))
         {
            OutcomeNotice.Content = enable;
            SayOutcome_(StatusLevel.Information, text);
         }

         // After the dialog that added the criterion or the action has given the
         // keyboard back to the page.
         if (becameReady)
         {
            Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Input, new Action(() =>
            {
               if (ReferenceEquals(OutcomeNotice.Content, enable) && enable.IsEnabled && OutcomeNotice.IsVisible)
                  FocusWithNotice_(enable);
            }));
         }
      }

      /// <summary>
      /// The keyboard on the new rule's next step - Add criterion…, then Enable now -
      /// and the notice in view with it where the window has the room: the scroll that
      /// brings the step into view is the least that shows it, and may leave the top
      /// of the notice above the view, as it left it under the header with the notice
      /// above the list. Measured once the step's own bring-into-view has run, at
      /// Loaded priority, after layout: a BringIntoView of the notice made before the
      /// button's focus was undone by the button's. Where the two do not fit, the
      /// step is what is shown.
      /// </summary>
      private void FocusWithNotice_(Control step)
      {
         if (!step.Focus())
            return;

         Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() =>
         {
            if (!OutcomeNotice.IsVisible || !step.IsVisible || !step.IsKeyboardFocused)
               return;

            Rect notice = OutcomeNotice.TransformToAncestor(PageScroll).TransformBounds(new Rect(OutcomeNotice.RenderSize));
            Rect shown = step.TransformToAncestor(PageScroll).TransformBounds(new Rect(step.RenderSize));
            if (notice.Top < 0 && shown.Bottom - notice.Top <= PageScroll.ViewportHeight)
               PageScroll.ScrollToVerticalOffset(Math.Max(0, PageScroll.VerticalOffset + notice.Top));
         }));
      }

      private Wpf.Ui.Controls.Button EnableAddedButton_()
      {
         if (enableAddedButton_ != null)
            return enableAddedButton_;

         var button = new Wpf.Ui.Controls.Button
         {
            Content = L("Enable _now"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            VerticalAlignment = VerticalAlignment.Center
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "rules-enable-added");
         button.Click += (_, _) => EnableAdded_();
         return enableAddedButton_ = button;
      }

      /// <summary>
      /// Enable now: the new rule switched on through the toolbar's road
      /// (WithSelectedRule, which saves it and reads the list again), the rule
      /// selected first where the selection has moved off it; then the notice says it
      /// is on, without the button, and the keyboard goes to the rule's row.
      /// </summary>
      private void EnableAdded_()
      {
         if (addedRuleId_ == 0)
            return;

         if ((RuleGrid.SelectedItem as RuleRow)?.Id != addedRuleId_)
         {
            if (RuleGrid.ItemsSource is not List<RuleRow> rows || rows.Find(r => r.Id == addedRuleId_) is not RuleRow row)
            {
               HideOutcome_();
               return;
            }

            ListSearchState.Reveal(SearchBar, RuleGrid, row);
            RuleGrid.SelectedItem = row;
         }

         string name = addedRuleName_;
         WithSelectedRule(rule =>
         {
            rule.Active = true;
            rule.Save();
            addedSwitchedOn_ = true;
         });
         if (!addedSwitchedOn_ || ReadFailure.IsShown(Notice))
            return;

         GridKeys.FocusSelectedRow(RuleGrid);
         OutcomeNotice.Content = null;
         SayOutcome_(StatusLevel.Good, RuleCards.SwitchedOn(name));
      }

      /// <summary>
      /// The subtitle: whose rules the list shows - the server's or the domain's
      /// chosen in Show rules for - where it said "4 rule(s)" of whichever was
      /// chosen (review finding F226). How many is the pill's beside the title, as
      /// on every list page (review finding F288), where the subtitle counted them.
      /// </summary>
      private string Subtitle_()
         => scopeDomain_ == null
            ? L("Server-wide message rules, evaluated top to bottom.")
            : F("Message rules of {0}, evaluated top to bottom.", scopeDomain_);

      private void Search_TextChanged(object sender, EventArgs e) => ShowListState_();

      /// <summary>
      /// The search over the rules read, the count beside the title and the empty
      /// state: "Shown: 1 of 4" while a search is typed in, and where it hides every
      /// row, the sentence that says so in place of the empty list's
      /// (Services/ListSearchState).
      /// </summary>
      private void ShowListState_() => ListSearchState.Show(RuleGrid, SearchBar.SearchText, CountPill,
         n => Plural(n, "{0} rule", "{0} rules"), EmptyStatus, L("No rules defined yet - create one below."));

      /// <summary>The header's Refresh: the list read again, as F5 reads it.</summary>
      private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();

      private static readonly string[] FieldNames =
         { "?", L("From"), L("To"), L("CC"), L("Subject"), L("Body"), L("Message size"), L("Recipient list"), L("Delivery attempts"),
           L("Attachment name"), L("Sensitive content"), L("Attachment type"), L("External recipients"), L("Readable text") };

      private static readonly string[] MatchNames =
         { "?", L("equals"), L("contains"), L("is less than"), L("is greater than"), L("matches regex"), L("does not contain"), L("does not equal"), L("matches wildcard") };

      private static readonly string[] ActionNames =
         { "?", L("Delete e-mail"), L("Forward e-mail"), L("Reply"), L("Move to IMAP folder"), L("Run script function"),
           L("Stop rule processing"), L("Set header value"), L("Send using route"), L("Create copy"), L("Bind to address"),
           L("Add disclaimer"), L("Reject with a text"), L("Require TLS for delivery"), L("Quarantine"), L("Warn the sender") };

      private static string Pick(string[] names, int index)
         => index >= 0 && index < names.Length ? names[index] : "#" + index;

      private void RuleGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         ShowToggle_(RuleGrid.SelectedItem as RuleRow);
         if (RuleGrid.SelectedItem is not RuleRow row)
         {
            selectedRuleId_ = 0;
            CriteriaGrid.ItemsSource = null;
            ActionsGrid.ItemsSource = null;
            ShowRuleParts_();
            return;
         }

         // The notice is about the rule just added: another rule chosen, it goes.
         if (addedRuleId_ != 0 && row.Id != addedRuleId_)
            HideOutcome_();

         selectedRuleId_ = row.Id;
         RefreshDetails();
      }

      private void RefreshDetails()
      {
         if (selectedRuleId_ == 0)
            return;

         var criteria = new List<DetailRow>();
         var actions = new List<DetailRow>();
         bool useAnd = true;

         dynamic rules = OpenRules();
         try
         {
            dynamic rule = rules.ItemByDBID[selectedRuleId_];
            if (rule == null)
               return;

            useAnd = (bool)rule.UseAND;

            dynamic criterias = rule.Criterias;
            int cc = (int)criterias.Count;
            for (int i = 0; i < cc; i++)
            {
               dynamic c = criterias.Item[i];
               criteria.Add(new DetailRow { Id = (int)c.ID, Description = DescribeCriterion(c) });
               ServerSession.Release(c);
            }
            ServerSession.Release(criterias);

            dynamic acts = rule.Actions;
            int ac = (int)acts.Count;
            for (int i = 0; i < ac; i++)
            {
               dynamic a = acts.Item[i];
               actions.Add(new DetailRow { Id = (int)a.ID, Description = DescribeAction(a) });
               ServerSession.Release(a);
            }
            ServerSession.Release(acts);

            ServerSession.Release(rule);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // leave lists as built so far
         }
         finally
         {
            ServerSession.Release(rules);
         }

         CriteriaGrid.ItemsSource = criteria;
         ActionsGrid.ItemsSource = actions;
         ShowRuleParts_();
         ShowAddedOutcome_();

         suppressMatchMode_ = true;
         MatchMode.SelectedIndex = useAnd ? 0 : 1;
         suppressMatchMode_ = false;
      }

      /// <summary>A criterion in the page's one pattern (Services/RulePartText): its field, its comparison and the value.</summary>
      private static string DescribeCriterion(dynamic c)
      {
         string field = (bool)c.UsePredefined
            ? Pick(FieldNames, (int)c.PredefinedField)
            : F("header '{0}'", (string)c.HeaderField);
         return RulePartText.Criterion(field, Pick(MatchNames, (int)c.MatchType), (string)c.MatchValue);
      }

      /// <summary>An action in the page's one pattern (Services/RulePartText): its verb and what it acts on.</summary>
      private static string DescribeAction(dynamic a)
      {
         int type = (int)a.Type;
         string name = Pick(ActionNames, type);
         try
         {
            switch (type)
            {
               case RulePartText.Forward: return RulePartText.Action(type, name, to: (string)a.To);
               case RulePartText.Reply: return RulePartText.Action(type, name, subject: (string)a.Subject);
               case RulePartText.MoveToFolder: return RulePartText.Action(type, name, folder: (string)a.IMAPFolder);
               case RulePartText.RunFunction: return RulePartText.Action(type, name, function: (string)a.ScriptFunction);
               case RulePartText.SetHeader: return RulePartText.Action(type, name, header: (string)a.HeaderName, value: (string)a.Value);
               case RulePartText.SendUsingRoute: return RulePartText.Action(type, name, route: (int)a.RouteID);
               case RulePartText.BindToAddress: return RulePartText.Action(type, name, value: (string)a.Value);
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
         return name;
      }

      /// <summary>
      /// A rule's summary for the list: its first criterion and its first action
      /// (Services/RulePartText.Summary), read as the list is; empty where the
      /// server would not say.
      /// </summary>
      private static string Summarise_(dynamic rule)
      {
         string criterion = null, action = null;
         try
         {
            dynamic criterias = rule.Criterias;
            try
            {
               if ((int)criterias.Count > 0)
               {
                  dynamic c = criterias.Item[0];
                  criterion = DescribeCriterion(c);
                  ServerSession.Release(c);
               }
            }
            finally
            {
               ServerSession.Release(criterias);
            }

            dynamic actions = rule.Actions;
            try
            {
               if ((int)actions.Count > 0)
               {
                  dynamic a = actions.Item[0];
                  action = DescribeAction(a);
                  ServerSession.Release(a);
               }
            }
            finally
            {
               ServerSession.Release(actions);
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // The summary is a convenience: a rule whose parts cannot be read is listed by its name, as before.
         }
         return RulePartText.Summary(criterion, action);
      }

      // ---- Rule-level operations ----

      private void WithSelectedRule(Action<dynamic> action)
      {
         if (RuleGrid.SelectedItem is not RuleRow row)
            return;

         int selectedIndex = RuleGrid.SelectedIndex;

         dynamic rules = OpenRules();
         try
         {
            dynamic rule = rules.Item[row.Position - 1];
            action(rule);
            ServerSession.Release(rule);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Rule operation failed: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(rules);
         }

         Reload();

         if (selectedIndex >= 0 && selectedIndex < RuleGrid.Items.Count)
            RuleGrid.SelectedIndex = selectedIndex;
      }

      private void MoveUp_Click(object sender, RoutedEventArgs e) => WithSelectedRule(rule => rule.MoveUp());

      private void MoveDown_Click(object sender, RoutedEventArgs e) => WithSelectedRule(rule => rule.MoveDown());

      private void Toggle_Click(object sender, RoutedEventArgs e)
      {
         // The new rule's notice says to switch it on: once it is, the notice is done.
         HideOutcome_();
         WithSelectedRule(rule =>
         {
            rule.Active = !(bool)rule.Active;
            rule.Save();
         });
      }

      private void Delete_Click(object sender, RoutedEventArgs e)
      {
         if (RuleGrid.SelectedItem is not RuleRow row)
            return;

         if (!MessageBox.Confirm(F("Delete the rule {0}?", row.Name), L("Control Panel"), L("De_lete rule"), destructive: true))
            return;

         WithSelectedRule(rule => rule.Delete());
      }

      private void AddRule_Click(object sender, RoutedEventArgs e)
      {
         string name = NewRuleName.Text.Trim();

         // On the box, with the keyboard in it, where a message box said it
         // (review finding F212).
         DialogFields.ClearErrors(NewRuleNameRow);
         if (name.Length == 0)
         {
            DialogFields.ShowError(NewRuleNameRow, L("Enter a name for the new rule."));
            return;
         }

         // A new rule starts switched off: it is saved before it has criteria and
         // actions, and with its first of each it would act on live mail while the
         // rest is still being added - under AND, more mail than it is meant to
         // (review finding F225). The notice says to switch it on once it is whole.
         int addedId;
         dynamic rules = OpenRules();
         try
         {
            dynamic rule = rules.Add();
            rule.Name = name;
            rule.Active = false;
            rule.Save();
            addedId = (int)rule.ID;
            ServerSession.Release(rule);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not create the rule: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(rules);
         }

         NewRuleName.Text = "";
         HideOutcome_();
         Reload();
         if (ReadFailure.IsShown(Notice))
            return;

         // The new rule selected and in view, the keyboard on Add criterion…, its
         // next step, where it was the list's last row, often out of view, with
         // nothing selected. A search typed in before that would leave it out of
         // the list is cleared.
         if (RuleGrid.ItemsSource is List<RuleRow> rows && rows.Find(r => r.Id == addedId) is RuleRow added)
         {
            ListSearchState.Reveal(SearchBar, RuleGrid, added);
            RuleGrid.SelectedItem = added;
            ShowRowInList_(added);
            addedRuleId_ = addedId;
            addedRuleName_ = name;
            addedSwitchedOn_ = false;
            addedReady_ = false;
            ShowAddedOutcome_();
            Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Input, new Action(() => FocusWithNotice_(AddCriterionButton)));
         }
      }

      /// <summary>
      /// Brings a row into the list's own view: a new rule is the list's last row, so
      /// the list scrolls to its end without moving the page, whose ScrollIntoView
      /// would scroll the toolbar away to show the row - the page keeps its place
      /// at the add form just pressed. A row anywhere else is brought into view as
      /// the grid brings it.
      /// </summary>
      private void ShowRowInList_(RuleRow row)
      {
         RuleGrid.UpdateLayout();
         ScrollViewer list = FirstScroller_(RuleGrid);
         if (list != null && RuleGrid.Items.Count > 0 && ReferenceEquals(RuleGrid.Items[RuleGrid.Items.Count - 1], row))
            list.ScrollToEnd();
         else
            RuleGrid.ScrollIntoView(row);
      }

      private static ScrollViewer FirstScroller_(DependencyObject root)
      {
         for (int i = 0; i < System.Windows.Media.VisualTreeHelper.GetChildrenCount(root); i++)
         {
            DependencyObject child = System.Windows.Media.VisualTreeHelper.GetChild(root, i);
            if (child is ScrollViewer scroller)
               return scroller;
            ScrollViewer inner = FirstScroller_(child);
            if (inner != null)
               return inner;
         }

         return null;
      }

      private void MatchMode_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         if (suppressMatchMode_ || selectedRuleId_ == 0)
            return;

         bool useAnd = MatchMode.SelectedIndex == 0;
         dynamic rules = OpenRules();
         try
         {
            dynamic rule = rules.ItemByDBID[selectedRuleId_];
            if (rule != null)
            {
               rule.UseAND = useAnd;
               rule.Save();
               ServerSession.Release(rule);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not change the match mode: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(rules);
         }
      }

      // ---- Criteria operations ----

      private void AddCriterion_Click(object sender, RoutedEventArgs e)
      {
         if (selectedRuleId_ == 0)
            return;
         new RuleCriteriaDialog(Window.GetWindow(this), selectedRuleId_, 0, rulesProvider_).ShowDialog();
         RefreshDetails();
      }

      private void EditCriterion_Click(object sender, RoutedEventArgs e)
      {
         if (selectedRuleId_ == 0 || CriteriaGrid.SelectedItem is not DetailRow row)
            return;
         new RuleCriteriaDialog(Window.GetWindow(this), selectedRuleId_, row.Id, rulesProvider_).ShowDialog();
         RefreshDetails();
      }

      private void RemoveCriterion_Click(object sender, RoutedEventArgs e)
      {
         if (selectedRuleId_ == 0 || CriteriaGrid.SelectedItem is not DetailRow row)
            return;

         dynamic rules = OpenRules();
         try
         {
            dynamic rule = rules.ItemByDBID[selectedRuleId_];
            dynamic criterias = rule.Criterias;
            criterias.DeleteByDBID(row.Id);
            rule.Save();
            ServerSession.Release(criterias);
            ServerSession.Release(rule);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not remove the criterion: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(rules);
         }

         RefreshDetails();
      }

      // ---- Action operations ----

      private void AddAction_Click(object sender, RoutedEventArgs e)
      {
         if (selectedRuleId_ == 0)
            return;
         new RuleActionDialog(Window.GetWindow(this), selectedRuleId_, 0, rulesProvider_, serverLevel_).ShowDialog();
         RefreshDetails();
      }

      private void EditAction_Click(object sender, RoutedEventArgs e)
      {
         if (selectedRuleId_ == 0 || ActionsGrid.SelectedItem is not DetailRow row)
            return;
         new RuleActionDialog(Window.GetWindow(this), selectedRuleId_, row.Id, rulesProvider_, serverLevel_).ShowDialog();
         RefreshDetails();
      }

      private void RemoveAction_Click(object sender, RoutedEventArgs e)
      {
         if (selectedRuleId_ == 0 || ActionsGrid.SelectedItem is not DetailRow row)
            return;

         dynamic rules = OpenRules();
         try
         {
            dynamic rule = rules.ItemByDBID[selectedRuleId_];
            dynamic actions = rule.Actions;
            actions.DeleteByDBID(row.Id);
            rule.Save();
            ServerSession.Release(actions);
            ServerSession.Release(rule);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not remove the action: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(rules);
         }

         RefreshDetails();
      }

      private void ActionUp_Click(object sender, RoutedEventArgs e) => MoveAction(true);

      private void ActionDown_Click(object sender, RoutedEventArgs e) => MoveAction(false);

      private void MoveAction(bool up)
      {
         if (selectedRuleId_ == 0 || ActionsGrid.SelectedItem is not DetailRow row)
            return;

         dynamic rules = OpenRules();
         try
         {
            dynamic rule = rules.ItemByDBID[selectedRuleId_];
            dynamic actions = rule.Actions;
            dynamic a = actions.ItemByDBID[row.Id];
            if (up) a.MoveUp(); else a.MoveDown();
            a.Save();
            rule.Save();
            ServerSession.Release(a);
            ServerSession.Release(actions);
            ServerSession.Release(rule);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not move the action: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(rules);
         }

         RefreshDetails();

         // The moved action stays selected, so that the next Up or Down moves it
         // again rather than finding nothing selected.
         foreach (object item in ActionsGrid.Items)
         {
            if (item is DetailRow moved && moved.Id == row.Id)
            {
               ActionsGrid.SelectedItem = moved;
               ActionsGrid.ScrollIntoView(moved);
               break;
            }
         }
      }
   }
}
