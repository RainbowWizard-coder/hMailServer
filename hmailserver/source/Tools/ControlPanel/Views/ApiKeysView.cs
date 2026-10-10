// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using hMailServer.ControlPanel.Services;

using Typography = hMailServer.ControlPanel.Services.Typography;
using Path = System.Windows.Shapes.Path;
using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using EmptyState = hMailServer.ControlPanel.Views.Scaffold.EmptyState;
using FieldRow = hMailServer.ControlPanel.Views.Scaffold.FieldRow;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The REST administration API keys: which exist, what each one may do, and
   /// creating and revoking them.
   ///
   /// The keys were previously reachable only over the API they authenticate.
   /// Minting one meant a hand-written POST carrying the administrator password,
   /// against a listener that is off by default - so an administrator who wanted a
   /// scoped, expiring credential for a monitoring probe had, in practice, the
   /// choice between writing curl commands and handing out the administrator
   /// password itself. The second is what happens when the first is inconvenient,
   /// and it is exactly what scoped keys exist to prevent.
   ///
   /// Nothing here reaches the server: the key store is a file on the server's own
   /// machine, and the server re-reads it on every request, so a key created or
   /// revoked on this page is live immediately with no restart. That also means the
   /// page works while the REST listener is switched off, which is the state most
   /// installations are in when they first come here.
   ///
   /// A created key is shown once. There is no "show key" action anywhere on this
   /// page, because the store holds only a SHA-256 digest and offering one would be
   /// promising something the format cannot deliver.
   /// </summary>
   public class ApiKeysView : UserControl, IPageLifecycle
   {
      private readonly StackPanel list_ = new();

      // What the key store as a whole amounts to, at its level. The counts are
      // words, so the level confirms and never carries alone.
      private readonly InlineNotice summary_ = new();
      private readonly TextBlock storePath_ = new();
      private readonly EmptyState empty_ = new()
      {
         Icon = Wpf.Ui.Controls.SymbolRegular.Key24,
         Visibility = Visibility.Collapsed
      };

      /// <summary>The card shown after a key is created, holding the one copy of
      /// the token that will ever exist.</summary>
      private readonly Card newKeyCard_;
      private readonly TextBlock newKeyToken_ = new();
      private readonly TextBlock newKeyLabel_ = new();

      public ApiKeysView()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         // Refresh, then Create a key…, the Primary, at the right edge, where it
         // came first (review finding F289).
         var actions = new StackPanel { Orientation = Orientation.Horizontal };

         var reload = new Wpf.Ui.Controls.Button { Content = L("_Refresh"), Margin = new Thickness(0, 0, 8, 0) };
         ControlHelp.Explain(reload, L("Re-read the key store from disk"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(reload, "apikeys-reload");
         reload.Click += (s, e) => Reload_();
         actions.Children.Add(reload);

         var create = new Wpf.Ui.Controls.Button
         {
            Content = L("_Create a key…"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary
         };
         ControlHelp.Explain(create, L("Create a new REST API key"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(create, "apikeys-create");
         create.Click += (s, e) => CreateKey_();
         actions.Children.Add(create);

         root.Children.Add(new PageHeader
         {
            PageKey = "apikeys",
            Subtitle = L("Credentials for the REST administration API that are not the administrator password: each one expires, can be limited to reading only, to particular domains and to particular source addresses, and can be revoked on its own. The server re-reads the key store on every request, so anything changed here is live at once - no service restart."),
            Actions = actions
         });

         var cards = new StackPanel { Margin = new Thickness(0, 0, 12, 0), MaxWidth = 860, HorizontalAlignment = HorizontalAlignment.Left };

         cards.Children.Add(BuildSummaryCard_());
         newKeyCard_ = BuildNewKeyCard_();
         cards.Children.Add(newKeyCard_);
         cards.Children.Add(BuildListCard_());
         cards.Children.Add(BuildExplanationCard_());

         var scroll = new ScrollViewer { Content = cards, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         Content = root;
      }

      public void OnEnter() => Reload_();

      public void OnLeave()
      {
         // The token is cleared on the way out rather than being left on a cached
         // page for whoever walks past the console next.
         HideNewKey_();
      }

      // =========================================================================
      // Layout
      // =========================================================================

      /// <summary>One section of the page. Named Section_ and not Card_ because
      /// the component it builds is called Card.</summary>
      private static Card Section_(string cardTitle, string blurb, out StackPanel content)
      {
         content = new StackPanel();
         var card = new Card { Title = cardTitle, Description = blurb, Content = content };
         card.SetResourceReference(MarginProperty, "AppCardGap");
         return card;
      }

      private Card BuildSummaryCard_()
      {
         Card card = Section_(L("Key store"), null, out StackPanel content);

         summary_.Margin = new Thickness(0, 0, 0, 8);
         content.Children.Add(summary_);

         storePath_.TextWrapping = TextWrapping.Wrap;
         storePath_.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         content.Children.Add(storePath_);

         return card;
      }

      /// <summary>
      /// Where a new key is shown. It is the only place the clear text ever
      /// appears, so it says so, in the imperative, before the value itself.
      /// </summary>
      private Card BuildNewKeyCard_()
      {
         Card card = Section_(L("Copy this key now"),
            L("This is the only time it will ever be shown. The store keeps a SHA-256 digest, not the key, so it cannot be recovered or re-displayed - if it is lost, revoke it here and create another."),
            out StackPanel content);

         card.Visibility = Visibility.Collapsed;

         newKeyLabel_.SetResourceReference(StyleProperty, "TextCaption");
         newKeyLabel_.Margin = new Thickness(0, 0, 0, 6);
         content.Children.Add(newKeyLabel_);

         newKeyToken_.FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily);
         newKeyToken_.FontSize = Typography.Body;
         newKeyToken_.TextWrapping = TextWrapping.Wrap;
         newKeyToken_.Margin = new Thickness(0, 0, 0, 12);
         System.Windows.Automation.AutomationProperties.SetAutomationId(newKeyToken_, "apikeys-new-token");
         System.Windows.Automation.AutomationProperties.SetName(newKeyToken_, L("The new API key, shown once"));
         content.Children.Add(newKeyToken_);

         var buttons = new StackPanel { Orientation = Orientation.Horizontal };

         var copy = new Wpf.Ui.Controls.Button
         {
            Content = L("Copy to clip_board"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            Margin = new Thickness(0, 0, 8, 0)
         };
         ControlHelp.Explain(copy, L("Copy the new API key to the clipboard"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(copy, "apikeys-copy");
         copy.Click += (s, e) =>
         {
            // Another process can hold the clipboard open. Saying so is better
            // than a button that silently did nothing with a value that cannot
            // be shown again.
            if (!ClipboardWriter.TrySetText(newKeyToken_.Text, out string error))
            {
               MessageBox.Show(F("The clipboard could not be written: {0}\r\n\r\nSelect the key above and copy it by hand - it will not be shown again.", error),
                  L("Control Panel"));
            }
         };
         buttons.Children.Add(copy);

         var done = new Wpf.Ui.Controls.Button { Content = L("_I have copied it") };
         ControlHelp.Explain(done, L("Hide the new API key"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(done, "apikeys-dismiss");
         done.Click += (s, e) => HideNewKey_();
         buttons.Children.Add(done);

         content.Children.Add(buttons);
         return card;
      }

      private Card BuildListCard_()
      {
         Card card = Section_(L("API keys"),
            L("Only a digest of each key is stored, so a key cannot be read back from here or from the file. Revoking one removes its section from the store and takes effect on the very next request."),
            out StackPanel content);

         System.Windows.Automation.AutomationProperties.SetAutomationId(list_, "apikeys-list");
         content.Children.Add(list_);

         // With no key, one message, by the list, and the way to the first key
         // (review finding F207): the store's notice above said the same as
         // the list's "No keys yet." below it, at Information, a card apart.
         var first = new Wpf.Ui.Controls.Button { Content = L("Create the _first key…") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(first, "apikeys-create-first");
         first.Click += (s, e) => CreateKey_();
         empty_.Action = first;
         content.Children.Add(empty_);
         return card;
      }

      private static Card BuildExplanationCard_()
      {
         Card card = Section_(L("How a key is used"), null, out StackPanel content);

         content.Children.Add(Paragraph_(
            L("Send it as a bearer token:  Authorization: Bearer hmapi_...  to the REST listener configured on the API & monitoring page. The administrator password also works and is unrestricted, which is the reason to prefer a key: a key can be read-only, limited to named domains, limited to one source address, given an expiry, and revoked without changing anything else.")));

         content.Children.Add(Paragraph_(
            L("No key of any scope can create or revoke keys - that needs the administrator password, or this page. A key restricted to particular domains is also refused the delivery-queue endpoints outright, because the queue is server-wide and cannot be filtered by domain.")));

         content.Children.Add(Paragraph_(
            L("Every value fails closed. A section whose Scope is anything but the literal \"full\" is read-only, a key whose expiry is missing or unreadable counts as expired, and a section with no usable digest is ignored entirely. That is what makes the store safe to edit by hand: a typo can only ever narrow a key, never widen one.")));

         var links = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 10, 0, 0) };
         links.Children.Add(PageLink_("api", F("Open {0}", L(NavigationMap.TitleOf("api"))),
            L("Open API & monitoring, which owns the REST listener's port and TLS settings")));
         content.Children.Add(links);

         return card;
      }

      private static TextBlock Paragraph_(string text)
      {
         var block = new TextBlock { Text = text, Margin = new Thickness(0, 0, 0, 10) };
         block.SetResourceReference(StyleProperty, "TextBody");
         return block;
      }

      /// <summary>Same construction as SpamOverviewView.PageLink: the way out to the
      /// page that owns the thing being described.</summary>
      private static FrameworkElement PageLink_(string page, string caption, string accessibleName)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = caption,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            ToolTip = accessibleName
         };
         ButtonStyles.CompactPageLink(button);
         ControlHelp.NameLink(button, caption, accessibleName);
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "apikeys-open-" + page);
         button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo(page);
         return button;
      }

      // =========================================================================
      // Data
      // =========================================================================

      private void Reload_()
      {
         list_.Children.Clear();

         string path = ApiKeyStore.StoreFile;

         if (path == null)
         {
            SetSummary_(StatusLevel.Information,
               L("The key store's location is not known from here. It sits beside hMailServer.ini, which is found through this machine's registry and service table - so keys can only be managed from the server itself, not from a Control Panel connected to another host."));
            storePath_.Text = "";
            empty_.Visibility = Visibility.Collapsed;
            return;
         }

         storePath_.Text = L("Store: ") + path;

         List<ApiKeyRecord> keys = ApiKeyStore.Read();

         // With no key the store's notice says nothing the list's empty state
         // does not: the one message is by the list, with its action.
         summary_.Visibility = keys.Count == 0 ? Visibility.Collapsed : Visibility.Visible;

         if (keys.Count > 0)
         {
            int expired = keys.FindAll(k => k.IsExpired).Count;
            int full = keys.FindAll(k => !k.ReadOnly && !k.IsExpired).Count;
            int unusable = keys.FindAll(k => k.Unusable).Count;

            var parts = new List<string> { Plural(keys.Count, "{0} key", "{0} keys") };
            if (full > 0)
               parts.Add(F("{0} with full authority", full));
            if (expired > 0)
               parts.Add(Plural(expired, "{0} has expired and is refused", "{0} have expired and are refused"));
            if (unusable > 0)
               parts.Add(F("{0} with no usable digest, which the server ignores", unusable));

            SetSummary_(unusable > 0 ? StatusLevel.Warning : StatusLevel.Good, string.Join(", ", parts) + ".");
         }

         foreach (ApiKeyRecord key in keys)
            list_.Children.Add(BuildKeyRow_(key));

         StatusText.Show(empty_, null, keys.Count, null,
            L("No API keys yet, so every REST request has to carry the administrator password, which has full authority over every domain and cannot be scoped, expired or revoked on its own. Create a key to use in its place."));
      }

      private FrameworkElement BuildKeyRow_(ApiKeyRecord key)
      {
         var border = new Border
         {
            Margin = new Thickness(0, 0, 0, 8),
            Padding = new Thickness(12, 10, 12, 10),
            BorderThickness = new Thickness(1)
         };
         border.SetResourceReference(Border.CornerRadiusProperty, "AppControlCornerRadius");
         border.SetResourceReference(Border.BorderBrushProperty, "AppCardBorderBrush");

         var grid = new Grid();
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

         var details = new StackPanel();

         var headline = new TextBlock { TextWrapping = TextWrapping.Wrap };
         headline.SetResourceReference(StyleProperty, "TextBody");
         headline.Inlines.Add(new Run(key.Label.Length > 0 ? key.Label : L("(no label)")) { FontWeight = FontWeights.SemiBold });
         // A Run is an Inline and has no Opacity; the secondary text brush is how
         // the rest of the application recedes a caption, and it is theme-aware,
         // which a hard-coded opacity would not be.
         var idRun = new Run("   " + key.Id) { FontSize = Typography.Caption };
         idRun.SetResourceReference(TextElement.ForegroundProperty, "TextFillColorSecondaryBrush");
         headline.Inlines.Add(idRun);
         details.Children.Add(headline);

         // The status line carries a word as well as a shape and a colour, so the
         // difference between a live full-authority key and an expired read-only
         // one survives greyscale and colour blindness. The pill names the key's
         // state - Full authority, Read-only, Expired, Ignored - where it read a
         // severity word, "Warning" or "OK", which said how much attention the
         // key wanted and not what it was.
         StatusLevel level;
         string pill;
         string state;

         if (key.Unusable)
         {
            level = StatusLevel.Warning;
            pill = L("Ignored");
            state = L("The stored digest is not a usable SHA-256 value, so this section authenticates nothing. Revoke it and create a replacement.");
         }
         else if (key.IsExpired)
         {
            // An expired key is a fact that needs nothing done, Normal; one whose
            // expiry this page cannot read needs a person, a Warning.
            level = key.ExpiresAt == null ? StatusLevel.Warning : StatusLevel.Normal;
            pill = key.ExpiresAt == null ? L("Expiry unreadable") : L("Expired");

            // Two different states, and the unreadable one is not "expired". This
            // page parses the timestamp with one exact format; the server parses it
            // with its own, so a hand-edited value can be readable to the server and
            // not to this page. Saying "the server treats this as expired" would be
            // a claim about the server made from a failure of ours - and the remedy
            // (rewrite the line, or revoke and re-create) is the same either way
            // without needing to assert which of us is right.
            state = key.ExpiresAt == null
               ? L("This page reads an expiry only in the form YYYY-MM-DD HH:MM:SS, so whether the server still accepts this key cannot be told from here. If the line was edited by hand, correct it; otherwise revoke the key and create a replacement.")
               : F("Since {0}, every request it makes is refused.", key.ExpiresAt.Value.ToString("d MMMM yyyy HH:mm", CultureInfo.CurrentCulture)); // no-loc
         }
         else if (!key.ReadOnly)
         {
            level = StatusLevel.Warning;
            pill = L("Full authority");
            state = F("This key can change and delete things. Expires {0}.", key.ExpiresAt.Value.ToString("d MMMM yyyy HH:mm", CultureInfo.CurrentCulture)); // no-loc
         }
         else
         {
            level = StatusLevel.Good;
            pill = L("Read-only");
            state = F("Expires {0}.", key.ExpiresAt.Value.ToString("d MMMM yyyy HH:mm", CultureInfo.CurrentCulture)); // no-loc
         }

         // A grid and not a horizontal stack: a stack measures the sentence with
         // all the width it asks for, so in a narrow window the sentence ran on
         // under Revoke and was cut off where it should have wrapped. The pill
         // keeps its width and the sentence wraps in what is left of the row.
         var stateRow = new Grid { Margin = new Thickness(0, 6, 0, 0) };
         stateRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         stateRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

         // The pill carries the colour, the shape and the state's name together,
         // and is named "Warning: Full authority" by the component; the sentence
         // beside it says what that means for this key, and is named by itself.
         // It does not name the state again: "Read-only" in the pill beside
         // "Read-only. Expires ..." was said twice, to the eye and to a reader,
         // as were "Full authority", "Expired" and "Ignored" beside sentences
         // that began with them, and "Expiry unreadable" beside "The stored
         // expiry could not be read in the form this page expects ...".
         stateRow.Children.Add(new StatusPill
         {
            Level = level,
            Text = pill,
            Margin = new Thickness(0, 1, 8, 0),
            VerticalAlignment = VerticalAlignment.Top
         });

         // Left, so that a sentence held to its measure in a wide window stays
         // beside the pill rather than centred in the column.
         var stateText = new TextBlock { Text = state, TextWrapping = TextWrapping.Wrap, MaxWidth = 560, HorizontalAlignment = HorizontalAlignment.Left };
         stateText.SetResourceReference(StyleProperty, "TextCaption");
         Grid.SetColumn(stateText, 1);
         stateRow.Children.Add(stateText);

         details.Children.Add(stateRow);

         var restrictions = new List<string>
         {
            key.Domains.Count == 0 ? L("every domain") : F("domains: {0}", string.Join(", ", key.Domains)),
            key.AllowedFrom.Length == 0 ? L("any source address") : F("only from {0}", key.AllowedFrom)
         };

         var scope = new TextBlock
         {
            Text = L("Scope: ") + string.Join("; ", restrictions) + ".",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 6, 0, 0)
         };
         scope.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         details.Children.Add(scope);

         Grid.SetColumn(details, 0);
         grid.Children.Add(details);

         var revoke = new Wpf.Ui.Controls.Button   // per row: no access key, the rows are reached with the arrow keys
         {
            Content = L("Revoke"),
            VerticalAlignment = VerticalAlignment.Top,
            MinWidth = 84
         };
         // A row's revoke is a list delete: red words on a secondary button, as on
         // every list page; the filled red is the confirmation's own (F183).
         ButtonStyles.DangerText(revoke);
         System.Windows.Automation.AutomationProperties.SetName(revoke,
            F("Revoke the API key {0}", key.Label.Length > 0 ? key.Label : key.Id));
         System.Windows.Automation.AutomationProperties.SetAutomationId(revoke, "apikeys-revoke-" + key.Id);
         revoke.Click += (s, e) => RevokeKey_(key);
         Grid.SetColumn(revoke, 1);
         grid.Children.Add(revoke);

         border.Child = grid;
         return border;
      }

      private void SetSummary_(StatusLevel level, string text)
      {
         summary_.Level = level;
         summary_.Text = text;
         summary_.Visibility = Visibility.Visible;
      }

      // =========================================================================
      // Actions
      // =========================================================================

      private void CreateKey_()
      {
         var request = ApiKeyDialog.Ask(Window.GetWindow(this));
         if (request == null)
            return;

         ApiKeyStore.CreateResult result = ApiKeyStore.Create(
            request.Label, request.Full, request.Expires, request.AllowedFrom, request.Domains);

         if (!result.Succeeded)
         {
            MessageBox.Show(result.Error, L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
         }

         newKeyLabel_.Text = F("{0} - {1}, expires {2}", request.Label, request.Full ? L("Full authority") : L("Read-only"),
                             request.Expires.ToString("d MMMM yyyy HH:mm", CultureInfo.CurrentCulture)); // no-loc // no-loc
         newKeyToken_.Text = result.Token;
         newKeyCard_.Visibility = Visibility.Visible;
         newKeyCard_.BringIntoView();

         Reload_();
      }

      private void HideNewKey_()
      {
         newKeyToken_.Text = "";
         newKeyLabel_.Text = "";
         newKeyCard_.Visibility = Visibility.Collapsed;
      }

      private void RevokeKey_(ApiKeyRecord key)
      {
         string name = key.Label.Length > 0 ? key.Label : key.Id;

         if (!MessageBox.Confirm(Confirmations.RevokeApiKey(name, key.IsExpired), L("Control Panel"), L("_Revoke key"), destructive: true))
            return;

         if (!ApiKeyStore.Revoke(key.Id, out string error))
         {
            MessageBox.Show(error, L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
         }

         Reload_();
      }
   }

   /// <summary>
   /// What to create, asked for before anything is written. A dialog rather than
   /// inline fields because the answers are only meaningful together: a key's
   /// scope, expiry and restrictions are one decision, and the token that comes
   /// back cannot be changed afterwards.
   /// </summary>
   internal sealed class ApiKeyRequest
   {
      public string Label;
      public bool Full;
      public DateTime Expires;
      public string AllowedFrom;
      public string Domains;
   }

   /// <summary>
   /// On the standard frame (DialogFrame), as every other dialog is: a heading,
   /// the "shown once" note as its description, the fields as FieldRows with
   /// their hints under them, and a value the store would refuse reported on the
   /// field at fault, with the keyboard put in it, rather than in a message box
   /// after Create key was pressed. It was the one dialog off the frame: no
   /// heading, notes at 65 % opacity, and a 200-pixel date picker in the long
   /// date format, which cut the year off ("Wednesday, January 6, 2").
   /// </summary>
   internal static class ApiKeyDialog
   {
      public static ApiKeyRequest Ask(Window owner)
      {
         var dlg = new FramedDialog { Owner = owner, Title = L("Create a REST API key") };

         var panel = new StackPanel();

         var labelBox = new Wpf.Ui.Controls.TextBox { PlaceholderText = L("What this key is for, e.g. \"Grafana probe\"") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(labelBox, "apikeys-dialog-label");
         FieldRow labelRow = DialogFields.Field(L("_Label"), labelBox);
         panel.Children.Add(labelRow);

         // Read-only first and selected by default, matching the server: a create
         // request that names no scope gets a read-only key, because a key is most
         // often minted for something that only reads and the alternative default
         // hands out a credential that can delete accounts.
         panel.Children.Add(DialogFields.Caption(L("What it may do")));
         var readOnly = new RadioButton
         {
            Content = L("_Read-only - can list and read; every change is refused"),
            IsChecked = true,
            Margin = new Thickness(0, 0, 0, 4)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(readOnly, "apikeys-dialog-readonly");
         panel.Children.Add(readOnly);

         var full = new RadioButton
         {
            Content = L("_Full authority - can create, change and delete"),
            Margin = new Thickness(0, 0, 0, 12)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(full, "apikeys-dialog-full");
         panel.Children.Add(full);

         // The short date, which fits, and three quick picks beside it; the
         // picker and the picks wrap where they do not fit on one line.
         var expires = new DatePicker
         {
            SelectedDate = DateTime.Today.AddDays(ApiKeyStore.DefaultLifetimeDays),
            SelectedDateFormat = DatePickerFormat.Short,
            VerticalAlignment = VerticalAlignment.Center,
            Width = 200
         };
         System.Windows.Automation.AutomationProperties.SetName(expires, L("Date this key stops working"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(expires, "apikeys-dialog-expires");
         var expiry = new WrapPanel();
         expiry.Children.Add(expires);
         expiry.Children.Add(QuickPick_(L("_30 days"), 30, expires));
         expiry.Children.Add(QuickPick_(L("_90 days"), 90, expires));
         expiry.Children.Add(QuickPick_(L("_1 year"), 365, expires));
         FieldRow expiresRow = DialogFields.Field(L("_Expires"), expiry,
            F("Defaults to {0} days, which is what the API itself uses when a request does not name one. A key with no expiry is the property that makes the administrator password dangerous, so there is no \"never\" option.", ApiKeyStore.DefaultLifetimeDays));
         panel.Children.Add(expiresRow);

         var domainsBox = new Wpf.Ui.Controls.TextBox { PlaceholderText = L("example.com, example.net - empty means every domain") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(domainsBox, "apikeys-dialog-domains");
         FieldRow domainsRow = DialogFields.Field(L("_Domains it may act on (optional)"), domainsBox,
            L("A key with a domain list is also refused the delivery-queue endpoints, because the queue is server-wide and cannot be filtered by domain."));
         panel.Children.Add(domainsRow);

         // One address or one range: the store takes a single value, so the
         // caption is singular, and the example's "or" is the language's own.
         var sourceBox = new Wpf.Ui.Controls.TextBox { PlaceholderText = L("For example 10.0.0.5 or 10.0.0.0/24") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(sourceBox, "apikeys-dialog-source");
         FieldRow sourceRow = DialogFields.Field(L("_Source address or range it may be used from (optional)"), sourceBox, L("Leave empty for any address"));
         sourceRow.Margin = new Thickness(0);
         panel.Children.Add(sourceRow);

         ApiKeyRequest result = null;

         var ok = new Wpf.Ui.Controls.Button
         {
            Content = L("_Create key"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            MinWidth = 100
         };
         ok.Click += (s, e) =>
         {
            // Validated here as well as in the store, so a rejected value is a
            // message on the box that produced it rather than after the dialog
            // has closed and taken the other answers with it.
            DialogFields.ClearErrors(labelRow, expiresRow, domainsRow, sourceRow);

            string error = ApiKeyStore.ValidateLabel(labelBox.Text);
            if (error != null)
            {
               DialogFields.ShowError(labelRow, error);
               return;
            }

            if (expires.SelectedDate == null || expires.SelectedDate.Value.Date < DateTime.Today.AddDays(1).Date)
            {
               DialogFields.ShowError(expiresRow, L("Choose an expiry date at least a day away. A key that expires today would stop working part-way through the day it was made."));
               return;
            }

            ApiKeyStore.NormalizeDomains(domainsBox.Text, out error);
            if (error != null)
            {
               DialogFields.ShowError(domainsRow, error);
               return;
            }

            string source = sourceBox.Text.Trim();
            if (source.Length > 0 && !ApiKeyStore.LooksLikeSourceRestriction(source))
            {
               DialogFields.ShowError(sourceRow, L("The source restriction has to be an address (10.0.0.5), a range (10.0.0.1-10.0.0.99) or CIDR (10.0.0.0/24). Leave it empty to accept the key from any address."));
               return;
            }

            result = new ApiKeyRequest
            {
               Label = labelBox.Text.Trim(),
               Full = full.IsChecked is true,
               // End of the chosen day rather than midnight at its start: a key
               // asked to last until the 30th that stopped working on the 29th
               // would be a day short of what was chosen.
               Expires = expires.SelectedDate.Value.Date.AddDays(1).AddSeconds(-1),
               AllowedFrom = source,
               Domains = domainsBox.Text.Trim()
            };

            dlg.DialogResult = true;
            dlg.Close();
         };

         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 80 };
         cancel.Click += (s, e) => dlg.Close();

         dlg.Frame(L("Create a REST API key"), panel, ok, cancel,
            L("The key is shown once, when it is created, and never again. It is live immediately - the server re-reads its key store on every request."),
            width: 560);

         // Room for a field's message: a press refuses one field, and the longest
         // message - the expiry's, the source restriction's - wraps to two lines.
         dlg.KeepRoomForFieldErrors(2);

         return dlg.ShowDialog() == true ? result : null;
      }

      /// <summary>A button that sets the picker to so many days from today.</summary>
      private static Wpf.Ui.Controls.Button QuickPick_(string caption, int days, DatePicker picker)
      {
         var button = new Wpf.Ui.Controls.Button { Content = caption, Margin = new Thickness(8, 0, 0, 0) };
         button.Click += (s, e) => picker.SelectedDate = DateTime.Today.AddDays(days);
         return button;
      }
   }
}
