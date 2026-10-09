// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using hMailServer.ControlPanel.Services;

using Typography = hMailServer.ControlPanel.Services.Typography;
using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using FieldRow = hMailServer.ControlPanel.Views.Scaffold.FieldRow;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The directory as an account source: which mailboxes the directory says should
   /// exist here, and the two buttons that show it and then do it.
   ///
   /// Separate from the LDAP authentication page on purpose, because they answer
   /// different questions. That page is about whether a person who already has a
   /// mailbox can log into it. This one is about which people have a mailbox at all -
   /// and it is the only page in the product that creates accounts in bulk, which is
   /// why almost everything below is about making what is about to happen visible
   /// before it happens.
   ///
   /// Three rules shape the whole page:
   ///
   /// APPLY IS LOCKED UNTIL A PREVIEW HAS BEEN READ. Not a dialog that can be clicked
   /// through - the button is disabled until a preview has run with the same options,
   /// and any change to those options locks it again. The server makes the preview
   /// trustworthy (one shared decider, so a preview cannot describe an action the
   /// apply would not take); this makes it unavoidable.
   ///
   /// THE DISABLE SWITCH IS PART OF THE PREVIEW. Marking absent accounts inactive is
   /// the one thing here that takes a working mailbox away, so it is passed to the
   /// preview as well as the apply. A preview run without it would report those
   /// accounts as "left alone" and the apply would then disable them - the preview and
   /// the apply answering different questions, which is the failure the shared decider
   /// exists to prevent and which this page must not reintroduce.
   ///
   /// THE ROWS THAT COST SOMETHING ARE COUNTED SEPARATELY. Four hundred rows of equal
   /// weight is a log, not a screen. Creates are what was asked for; skips are
   /// mailboxes that were expected and will not appear, and disables take one away.
   /// Those two get their own number, and it is the number the confirmation quotes.
   ///
   /// The parsing has no WPF in it and lives in <see cref="DirectorySyncReport"/>,
   /// where it is tested directly.
   /// </summary>
   public class DirectorySyncView : UserControl, IPageLifecycle
   {
      private const string IniSection = "LDAP"; // no-loc

      private readonly IniFeatureStore store_ = new IniFeatureStore();

      private readonly Wpf.Ui.Controls.TextBox filter_ = new();
      private readonly Wpf.Ui.Controls.TextBox mailAttribute_ = new();
      private readonly Wpf.Ui.Controls.TextBox usernameAttribute_ = new();
      private readonly Wpf.Ui.Controls.TextBox displayNameAttribute_ = new();
      private readonly Wpf.Ui.Controls.TextBox maxUsers_ = new();
      private readonly Wpf.Ui.Controls.TextBox scheduleMinutes_ = new();

      private readonly ComboBox domainChoice_ = new();
      private readonly CheckBox disableMissing_ = new();

      private Wpf.Ui.Controls.Button previewButton_;
      private Wpf.Ui.Controls.Button applyButton_;
      private Wpf.Ui.Controls.Button saveButton_;

      // Whether a run can happen at all, as a notice at the level of the answer:
      // yes is good, a missing prerequisite is a warning, and an answer that
      // cannot be reached from this machine is information.
      private readonly InlineNotice stateText_ = new();

      // What the last run said, and what in it needs a person - a notice and a
      // caption, because the second is a qualification of the first.
      private readonly InlineNotice summaryText_ = new() { Visibility = Visibility.Collapsed };
      private readonly TextBlock attentionText_ = new();
      private readonly StackPanel rows_ = new();

      // Where the page stands, by the one save-state model of the settings pages
      // (IniPageSaveState): the pill beside the title, the notice under the header,
      // and the line that names the file. They were a footer under the scroller,
      // the result - Could not save among them - in grey 12-pixel text, with Reload
      // and Save changes beside it.
      private readonly StatusPill statePill_ = new() { Visibility = Visibility.Collapsed };
      private readonly InlineNotice statusNotice_ = new() { Visibility = Visibility.Collapsed };
      private readonly TextBlock pathText_ = new();
      private readonly IniPageSaveState saveState_;

      // Why hMailServer.ini is here and cannot be read, or null. From 6.3.6 the
      // file belongs to SYSTEM, the Administrators and the service
      // (hMailServer.exe /ProtectFolders), so a Control Panel the administrator
      // did not run as administrator reads every [LDAP] value as its fallback -
      // and "Enabled" as 0, which this page then reported as "directory access is
      // switched off", the misstatement RefreshState_ says it must never make.
      // The page says that the file could not be read instead, in a Critical
      // notice above the cards, fills no editor and disables them with Save;
      // Preview and Apply stay, since they run inside the server.
      private string denied_;
      private readonly InlineNotice deniedNotice_ = new() { Level = StatusLevel.Critical, Visibility = Visibility.Collapsed };

      /// <summary>
      /// The options the last successful preview was run with, or null when no preview
      /// has been run since the page was opened or since an option changed. This is
      /// what Apply is gated on - not a flag, because a flag cannot tell whether the
      /// preview that was read is a preview of what is about to happen.
      /// </summary>
      private string previewedOptions_;

      /// <summary>
      /// What that preview read, for the question Apply asks: how many mailboxes it
      /// creates, updates and marks inactive, and how many entries it skips (review
      /// finding F128). Read only while previewedOptions_ is the options in force.
      /// </summary>
      private DirectorySyncReport previewReport_;

      private bool loading_;

      /// <summary>
      /// Whether the server's domain list was read in full. False means "unknown",
      /// never "none" - a distinction the state card depends on, because a server this
      /// page cannot talk to and a server with no linked domains are different
      /// problems with different fixes.
      /// </summary>
      private bool domainsRead_;

      public DirectorySyncView()
      {
         saveState_ = new IniPageSaveState(statePill_, statusNotice_, () => HasUnsavedChanges);

         // The page scrolls whole, header and all, as the feature settings pages do:
         // Reload and Save changes in the header's actions, the pill beside the title,
         // and under the header the notice that says where the page stands and the
         // line that names the file - then the cards. The page padding is the
         // scroller's own, as FeatureSettingsView.xaml's is, so the scroll bar sits
         // at the window's edge and the header's actions end where theirs do; it was
         // the scroller's margin, with twelve pixels more inside, which put both
         // twelve pixels further in than on every other page of the group.
         var page = new StackPanel();

         System.Windows.Automation.AutomationProperties.SetAutomationId(statePill_, "dirsync-state-pill");
         page.Children.Add(new PageHeader
         {
            Title = L("Directory synchronisation"),
            Subtitle = L("Creates and updates mailboxes to match an LDAP directory. Only domains that have an Active Directory domain name set take part, so this cannot provision into a hosted domain by accident. Nothing here ever deletes an account or a message: the most it does is mark one inactive, and only when you ask it to."),
            Status = statePill_,
            Actions = BuildActions_()
         });

         var cards = new StackPanel { MaxWidth = 860, HorizontalAlignment = HorizontalAlignment.Left };
         System.Windows.Automation.AutomationProperties.SetAutomationId(statusNotice_, "dirsync-status");
         cards.Children.Add(statusNotice_);
         pathText_.TextWrapping = TextWrapping.Wrap;
         pathText_.MaxWidth = 760;
         pathText_.HorizontalAlignment = HorizontalAlignment.Left;
         pathText_.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         pathText_.SetResourceReference(MarginProperty, "AppCardGap");
         System.Windows.Automation.AutomationProperties.SetAutomationId(pathText_, "dirsync-path");
         cards.Children.Add(pathText_);
         System.Windows.Automation.AutomationProperties.SetAutomationId(deniedNotice_, "dirsync-ini-denied");
         cards.Children.Add(deniedNotice_);
         cards.Children.Add(BuildStateCard_());
         cards.Children.Add(BuildSelectionCard_());
         cards.Children.Add(BuildRunCard_());
         cards.Children.Add(BuildScheduleCard_());
         page.Children.Add(cards);

         var scroll = new ScrollViewer { Content = page, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         scroll.SetResourceReference(PaddingProperty, "AppPagePadding");
         Content = scroll;

         // Every editor a save writes, wired to the pill: Unsaved changes from the
         // first edit, for as long as one differs from what was read or saved. The
         // fill of a read is not an edit, and the read settles the pill itself.
         foreach (Wpf.Ui.Controls.TextBox box in new[] { filter_, mailAttribute_, usernameAttribute_, displayNameAttribute_, maxUsers_, scheduleMinutes_ })
            box.TextChanged += (s, e) => { if (!loading_) saveState_.Follow(); };
      }

      public void OnEnter()
      {
         Load_();
      }

      public void OnLeave()
      {
      }

      /// <summary>What the six saved editors held when they were last read from the INI or saved to it.</summary>
      private readonly EditorBaseline baseline_ = new();

      /// <summary>The editors Save writes. The domain and the inactive switch are a run's, and are not saved.</summary>
      private IEnumerable<string> EditorValues_() => new[]
      {
         filter_.Text, mailAttribute_.Text, usernameAttribute_.Text, displayNameAttribute_.Text, maxUsers_.Text, scheduleMinutes_.Text
      };

      /// <summary>
      /// Whether a saved editor holds something other than what was last read or
      /// saved. OnEnter reads the INI again, so such an edit is gone when the page is
      /// next opened.
      /// </summary>
      public bool HasUnsavedChanges => store_.IsAvailable && baseline_.Differs(EditorValues_());

      /// <summary>The page's own Save; true when it wrote the settings.</summary>
      public bool SaveChanges() => Save_();

      /// <summary>Reload: asked about first while an edit is unsaved, since it replaces the editors.</summary>
      private void Reload_()
      {
         if (HasUnsavedChanges &&
             !MessageBox.Confirm(UnsavedChanges.ReloadQuestion, L("Control Panel"), L("_Discard"), destructive: true, cancelByDefault: true))
            return;

         Load_();
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

      /// <summary>
      /// One field: the caption with its Alt key, the editor, the sentence that
      /// explains it. FieldRow wires the mnemonic and gives the editor its
      /// accessible name and help text from the two, so only the automation id -
      /// which the page owns - is set here.
      /// </summary>
      private static FieldRow LabelledBox_(string label, Wpf.Ui.Controls.TextBox box,
         string automationId, string caption, string placeholder = "")
      {
         box.MaxWidth = 620;
         box.MinWidth = 320;
         box.HorizontalAlignment = HorizontalAlignment.Left;
         box.PlaceholderText = placeholder;
         System.Windows.Automation.AutomationProperties.SetAutomationId(box, automationId);

         return new FieldRow { Label = label, Content = box, Hint = caption };
      }

      /// <summary>
      /// Whether provisioning can happen at all, worked out and stated before anything
      /// is pressed. Three things have to be true and each has a different fix, so the
      /// card names which one is missing rather than saying "not configured".
      /// </summary>
      private Card BuildStateCard_()
      {
         Card card = Section_(L("Can this server provision from the directory?"), null, out StackPanel content);

         stateText_.Margin = new Thickness(0);
         System.Windows.Automation.AutomationProperties.SetAutomationId(stateText_, "dirsync-state");
         System.Windows.Automation.AutomationProperties.SetLiveSetting(stateText_,
            System.Windows.Automation.AutomationLiveSetting.Polite);
         content.Children.Add(stateText_);

         return card;
      }

      private Card BuildSelectionCard_()
      {
         Card card = Section_(L("Which directory entries become mailboxes"),
            L("These are read by the server from the [LDAP] section of hMailServer.INI, and re-read within two seconds of a save, so no restart is needed. The defaults suit Active Directory; a directory that is not Windows will not have sAMAccountName and needs the attribute names changed."),
            out StackPanel content);

         content.Children.Add(LabelledBox_(L("Search _filter"), filter_, "dirsync-filter",
            L("The LDAP filter that selects the people who should have a mailbox. The default takes enabled person objects that carry a mail address - which deliberately excludes disabled leavers, service accounts and computers, all of which a broader filter would provision mailboxes for."),
            // The server's kDefaultSyncFilter, character for character, INCLUDING the
            // userAccountControl clause. That clause is the whole of what the caption
            // above promises, and an earlier version of this placeholder omitted it -
            // which made this control the only place in the product that stated the
            // default filter, and stated it wrong. An administrator narrowing the
            // filter to one OU starts from what they can see here; starting from a
            // version without the exclusion produces a filter that provisions a live,
            // mail-accepting mailbox for every disabled leaver the directory still
            // holds, and LdapSettings honours it because it is not empty.
            placeholder: "(&(objectClass=user)(objectCategory=person)(mail=*)"
                         + "(!(userAccountControl:1.2.840.113556.1.4.803:=2)))"));

         content.Children.Add(LabelledBox_(L("Address attri_bute"), mailAttribute_, "dirsync-mail-attribute",
            L("Which attribute holds the mailbox address. proxyAddresses is understood: its scheme prefix is removed and the primary SMTP: value is preferred over the lower-case aliases."),
            placeholder: "mail"));

         content.Children.Add(LabelledBox_(L("_Logon name attribute"), usernameAttribute_, "dirsync-username-attribute",
            L("Which attribute holds the name the person logs in with. This is what is stored against the account and what the directory is asked about at every logon afterwards, so guessing it from the address is not good enough - an entry that does not carry it is reported rather than provisioned."),
            placeholder: "sAMAccountName"));

         content.Children.Add(LabelledBox_(L("_Display name attribute"), displayNameAttribute_, "dirsync-displayname-attribute",
            L("Split into first and last name, and only ever written to an account that has neither. A name typed here by an administrator is never overwritten by the directory."),
            placeholder: "displayName"));

         content.Children.Add(LabelledBox_(L("_Maximum entries to read"), maxUsers_, "dirsync-max-users",
            L("A ceiling on one pass, so a search base pointed one level too high cannot be answered by reading an entire forest. If it is hit, the run says so and disables nothing at all - beyond the limit, an entry that exists is indistinguishable from one that was deleted."),
            placeholder: "5000"));

         return card;
      }

      private Card BuildRunCard_()
      {
         Card card = Section_(L("Preview, then apply"),
            L("Preview changes nothing and reports exactly what Apply would do - they share one decider in the server, so a preview cannot describe an action the apply would not take. Apply stays disabled until a preview has been run with the options as they stand."),
            out StackPanel content);

         domainChoice_.MinWidth = 320;
         domainChoice_.MaxWidth = 620;
         domainChoice_.HorizontalAlignment = HorizontalAlignment.Left;
         System.Windows.Automation.AutomationProperties.SetAutomationId(domainChoice_, "dirsync-domain");
         domainChoice_.SelectionChanged += (s, e) => InvalidatePreview_();
         content.Children.Add(new FieldRow
         {
            Label = L("Domain"),
            Content = domainChoice_,
            Hint = L("Only domains with an Active Directory domain name set are listed - that field is what links a mail domain to a directory. Bringing one domain over at a time is the sane way to do this the first time.")
         });

         disableMissing_.Content = L("Also mark accounts _inactive when their directory entry has gone");
         disableMissing_.Margin = new Thickness(0, 0, 0, 4);
         System.Windows.Automation.AutomationProperties.SetName(disableMissing_,
            L("Also mark accounts inactive when their directory entry has gone"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(disableMissing_, "dirsync-disable-missing");
         System.Windows.Automation.AutomationProperties.SetHelpText(disableMissing_,
            L("Off by default. An account that stops being visible is far more often a filter that changed than a person who left, and the two mistakes do not cost the same. No message is ever deleted and no account is removed - re-enabling one restores everything."));
         disableMissing_.Checked += (s, e) => InvalidatePreview_();
         disableMissing_.Unchecked += (s, e) => InvalidatePreview_();
         content.Children.Add(disableMissing_);

         var disableHint = new TextBlock
         {
            Text = L("Off by default. An account that stops being visible in the directory is far more often a search that changed than a person who left, and the two mistakes do not cost the same. Nothing is disabled at all when the enumeration was cut short or came back empty."),
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(24, 0, 0, 16)
         };
         disableHint.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         content.Children.Add(disableHint);

         var buttons = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 12) };

         previewButton_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Preview"),
            Margin = new Thickness(0, 0, 8, 0),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary
         };
         ControlHelp.Explain(previewButton_,
            L("Show what a synchronisation would do, changing nothing"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(previewButton_, "dirsync-preview");
         previewButton_.Click += async (s, e) => await RunAsync_(false);
         buttons.Children.Add(previewButton_);

         applyButton_ = new Wpf.Ui.Controls.Button { Content = L("_Apply"), IsEnabled = false };
         ControlHelp.Explain(applyButton_,
            L("Create and update the accounts the preview listed"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(applyButton_, "dirsync-apply");
         applyButton_.Click += async (s, e) => await ApplyAsync_();
         buttons.Children.Add(applyButton_);

         content.Children.Add(buttons);

         summaryText_.Margin = new Thickness(0, 0, 0, 8);
         System.Windows.Automation.AutomationProperties.SetAutomationId(summaryText_, "dirsync-summary");
         System.Windows.Automation.AutomationProperties.SetLiveSetting(summaryText_,
            System.Windows.Automation.AutomationLiveSetting.Polite);
         content.Children.Add(summaryText_);

         attentionText_.TextWrapping = TextWrapping.Wrap;
         attentionText_.SetResourceReference(StyleProperty, "TextCaption");
         attentionText_.Margin = new Thickness(0, 0, 0, 10);
         System.Windows.Automation.AutomationProperties.SetAutomationId(attentionText_, "dirsync-attention");
         content.Children.Add(attentionText_);

         rows_.Margin = new Thickness(0, 4, 0, 0);
         content.Children.Add(rows_);

         return card;
      }

      private Card BuildScheduleCard_()
      {
         Card card = Section_(L("Unattended synchronisation"),
            L("A schedule creates and updates accounts with nobody watching. It never disables one, whatever the checkbox above is set to - departures are left to a person who has read a preview. Switch this on only after a preview has told you what it is going to do."),
            out StackPanel content);

         content.Children.Add(LabelledBox_(L("Run _every (minutes)"), scheduleMinutes_, "dirsync-schedule",
            L("0 switches the schedule off, which is the default and is what every existing installation stays at. Values between 1 and 14 are treated as 15, and anything over a week as a week. This one needs a service restart: the task is registered when the server starts."),
            placeholder: "0"));

         return card;
      }

      /// <summary>Reload and Save changes, for the header's actions: Save the primary.</summary>
      private FrameworkElement BuildActions_()
      {
         var buttons = new StackPanel { Orientation = Orientation.Horizontal };

         var reload = new Wpf.Ui.Controls.Button { Content = L("_Reload"), Margin = new Thickness(0, 0, 8, 0) };
         ControlHelp.Explain(reload, L("Re-read these settings from hMailServer.INI"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(reload, "dirsync-reload");
         reload.Click += (s, e) => Reload_();
         buttons.Children.Add(reload);

         saveButton_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Save changes"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary
         };
         ControlHelp.Explain(saveButton_, L("Save these settings to hMailServer.INI"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(saveButton_, "dirsync-save");
         saveButton_.Click += (s, e) => Save_();
         buttons.Children.Add(saveButton_);

         return buttons;
      }

      // =========================================================================
      // Reading and writing the [LDAP] section
      // =========================================================================
      //
      // The same constraint LdapSettingsView carries and for the same reason:
      // IniFeatureStore writes [Settings], and these keys live in [LDAP], so a write
      // through it would land where the server never looks. The COM ini accessors are
      // section-less for the same reason, so these editors need the file itself.
      //
      // Preview and Apply do NOT: they are COM calls that run inside the server. So a
      // Control Panel that cannot reach the INI can still run a synchronisation - it
      // just cannot change what the synchronisation selects.

      private string IniRead_(string key, string fallback)
      {
         if (!store_.IsAvailable)
            return fallback;

         // 4096, the same buffer LdapSettings::ReadString_ uses, so this page sees
         // exactly the value - and exactly the truncation - the server sees.
         return ProfileApi.ReadString(IniSection, key, fallback, store_.IniPath, 4096).Trim();
      }

      private void Load_()
      {
         loading_ = true;

         try
         {
            // Before any value is read: a file that is here and cannot be read
            // answers every read with its fallback, which is a selection this page
            // must not show as the server's.
            denied_ = store_.ReadDenied();
            deniedNotice_.Text = denied_ == null ? null : F("hMailServer.ini could not be read: {0} From 6.3.6 the file belongs to the administrators. Close the Control Panel and start it with Run as administrator to see and change these settings.", denied_);
            deniedNotice_.Visibility = denied_ == null ? Visibility.Collapsed : Visibility.Visible;

            if (denied_ != null)
            {
               filter_.Text = "";
               mailAttribute_.Text = "";
               usernameAttribute_.Text = "";
               displayNameAttribute_.Text = "";
               maxUsers_.Text = "";
               scheduleMinutes_.Text = "";
            }
            else
            {
               filter_.Text = IniRead_("SyncFilter", "");
               mailAttribute_.Text = IniRead_("SyncMailAttribute", "mail");
               usernameAttribute_.Text = IniRead_("SyncUsernameAttribute", "sAMAccountName");
               displayNameAttribute_.Text = IniRead_("SyncDisplayNameAttribute", "displayName");
               maxUsers_.Text = IniRead_("SyncMaxUsers", "5000");
               scheduleMinutes_.Text = IniRead_("SyncScheduleMinutes", "0");
            }

            bool editable = store_.IsAvailable && denied_ == null;
            filter_.IsEnabled = editable;
            mailAttribute_.IsEnabled = editable;
            usernameAttribute_.IsEnabled = editable;
            displayNameAttribute_.IsEnabled = editable;
            maxUsers_.IsEnabled = editable;
            scheduleMinutes_.IsEnabled = editable;
            if (saveButton_ != null)
               saveButton_.IsEnabled = editable;

            LoadDomains_();
            RefreshState_();

            // The file the editors show, under the header, while they can be edited.
            // When they cannot, the line goes, and where the page stands is said as
            // the Directory authentication page says it, in the notice under the
            // header: it was this caption line, in the quietest text the page has,
            // under a red Unavailable pill that said nothing.
            pathText_.Text = editable ? F("Editing {0}", store_.IniPath) : null;
            pathText_.Visibility = editable ? Visibility.Visible : Visibility.Collapsed;
         }
         finally
         {
            loading_ = false;
         }

         baseline_.Take(EditorValues_());

         // Unavailable says why in a Critical notice when hMailServer.INI is not on
         // this machine. A file that is here and could not be read is said already,
         // in the Critical notice above the cards (deniedNotice_), and the state card
         // says that Preview still runs, so a second box would only repeat it.
         //
         // A schedule in the file that the running server did not start with waits
         // for a restart, whenever it was saved: the page says so as it opens, with
         // the restart, as it did after the save.
         if (store_.IsAvailable && denied_ == null)
         {
            DirectorySyncSchedule.Shared.ReadFile(store_.IniPath);
            if (DirectorySyncSchedule.Shared.Waits)
               saveState_.Settle(SettingsPageState.WaitsForRestart,
                  L("The saved schedule takes effect when the service restarts: the server is still running the one it started with."),
                  action: RestartAction_());
            else
               saveState_.Settle(SettingsPageState.Clean, null);
         }
         else if (denied_ != null)
            saveState_.Settle(SettingsPageState.Unavailable, null);
         else
            saveState_.Settle(SettingsPageState.Unavailable,
               L("hMailServer.INI cannot be reached from this computer, so these selections cannot be edited here. Preview and Apply still work: they run inside the server."));
         InvalidatePreview_();
      }

      /// <summary>Writes the six settings; true when they were written.</summary>
      private bool Save_()
      {
         if (!store_.IsAvailable || denied_ != null)
            return false;

         try
         {
            Write_("SyncFilter", filter_.Text.Trim());
            Write_("SyncMailAttribute", mailAttribute_.Text.Trim());
            Write_("SyncUsernameAttribute", usernameAttribute_.Text.Trim());
            Write_("SyncDisplayNameAttribute", displayNameAttribute_.Text.Trim());
            Write_("SyncMaxUsers", maxUsers_.Text.Trim());
            Write_("SyncScheduleMinutes", scheduleMinutes_.Text.Trim());

            // Flush the cache the API keeps, so the value is on disk before the server
            // next compares the file's write time.
            ProfileApi.Flush(store_.IniPath);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Under the header, in a Critical notice, where it was grey caption text
            // in the footer; the pill keeps saying Unsaved changes, which they still are.
            saveState_.Refuse(F("Could not save: {0}", ex.Message));
            return false;
         }

         // A saved change alters what a run would select, so a preview taken before
         // it no longer describes what Apply would do.
         InvalidatePreview_();
         baseline_.Take(EditorValues_());

         // The selection is live within two seconds; the schedule is registered as the
         // server starts, so one other than the server started with waits for a
         // restart, and the pill says so - where the sentence said a change to the
         // schedule needs a restart after every save, whether or not it had changed.
         // Measured from the schedule the server started with (DirectorySyncSchedule),
         // never from the last save: a second save after a schedule change, with no
         // restart between, said that no restart was needed. And measured as the server
         // registers it, from the file as written: while directory access is off nothing
         // runs whatever the minutes say, and 5 is 15.
         string time = DateTime.Now.ToLongTimeString();
         bool scheduleWaits = DirectorySyncSchedule.Shared.SavedFile(store_.IniPath);
         if (scheduleWaits)
            saveState_.Settle(SettingsPageState.WaitsForRestart, F("Saved {0} - the server picks the selection up within two seconds; the new schedule takes effect when the service restarts.", time),
               action: RestartAction_());
         else
            saveState_.Settle(SettingsPageState.Saved, F("Saved {0} - the server picks the selection up within two seconds; no service restart is needed.", time));
         return true;
      }

      /// <summary>
      /// The action of the notice that says a new schedule waits for a restart:
      /// Restart the service, the window's pending bar's restart, which asks first
      /// (IniPageSaveState.RestartAction). Null when the server is not the one on this
      /// machine, which a restart from here cannot reach.
      /// </summary>
      private Wpf.Ui.Controls.Button RestartAction_()
         => saveState_.RestartAction("dirsync-restart-service",
            L("The new schedule takes effect only when the hMailServer service restarts. Restart the service now? Every client connected to the server is disconnected while it stops and starts again."));

      /// <summary>
      /// Writes one [LDAP] key, and throws when the file refused it: the six writes
      /// were not checked, so a file the Control Panel may read and not write was
      /// reported saved.
      /// </summary>
      private void Write_(string key, string value)
      {
         if (!ProfileApi.WriteString(IniSection, key, value, store_.IniPath))
            throw new System.ComponentModel.Win32Exception(System.Runtime.InteropServices.Marshal.GetLastWin32Error(),
               F("Could not write {0} to {1}", key, store_.IniPath));
      }

      // =========================================================================
      // The server side
      // =========================================================================

      /// <summary>
      /// Fills the domain list with the domains that are actually eligible.
      ///
      /// Listing every domain would be the wrong help: a domain with no Active
      /// Directory domain name cannot be provisioned into, and choosing one would
      /// produce a run that reports every entry as skipped. Listing only the linked
      /// ones makes the prerequisite visible as an absence.
      /// </summary>
      private void LoadDomains_()
      {
         domainChoice_.Items.Clear();
         domainChoice_.Items.Add(L("All linked domains"));
         domainsRead_ = false;

         dynamic domains = null;

         try
         {
            domains = ServerSession.Current.Application.Domains;

            int count = (int)domains.Count;

            for (int i = 0; i < count; i++)
            {
               dynamic domain = null;

               try
               {
                  domain = domains.Item[i];

                  string linked = (string)domain.ADDomainName;

                  if (!string.IsNullOrEmpty(linked) && linked.Trim().Length > 0)
                     domainChoice_.Items.Add((string)domain.Name);
               }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
               {
                  // One domain that cannot be read costs that domain's row, not the
                  // list. A page that threw here would be unusable because of a single
                  // malformed record.
               }
               finally
               {
                  ServerSession.Release((object)domain);
               }
            }

            // Set only after the whole walk, so a connection that died half way
            // through leaves this false rather than reporting a partial list as a
            // complete one. A page that said "no domain is linked" on the strength of
            // half a list would be sending an administrator to fix something that is
            // already correct.
            domainsRead_ = true;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // No server connection. The state card says so, and says it is a
            // connection problem rather than a configuration one; the list keeps its
            // "All" entry so the control is never empty.
         }
         finally
         {
            ServerSession.Release((object)domains);
         }

         domainChoice_.SelectedIndex = 0;
      }

      /// <summary>
      /// Says whether a run can do anything, and if not, which of the three
      /// prerequisites is missing. Each has a different fix and a different page.
      /// </summary>
      private void RefreshState_()
      {
         int linkedDomains = Math.Max(0, domainChoice_.Items.Count - 1);

         // Two things this card must never do, both of which are the same mistake:
         // report an unread value as a configured one. IniRead_ answers its fallback
         // when the INI cannot be reached, so "Enabled" reads as 0 on a Control Panel
         // running on another machine - and saying "directory access is switched off"
         // there would send an administrator to turn on something that is already on.
         // The same for the domain list: a server this page cannot talk to has no
         // readable domains, which is not the same fact as having no linked ones.
         if (denied_ != null)
         {
            State_(StatusLevel.Information, L("Cannot tell from here. hMailServer.ini could not be read, and it is the [LDAP] section of that file, not anything on this page, which decides whether a run can happen. Press Preview: it runs inside the server and will say exactly what is missing, if anything is."));
            return;
         }

         if (!store_.IsAvailable)
         {
            State_(StatusLevel.Information, L("Cannot tell from here. hMailServer.INI is not reachable from this computer, so the [LDAP] section cannot be read - and it is that section, not anything on this page, which decides whether a run can happen. Press Preview: it runs inside the server and will say exactly what is missing, if anything is."));
            return;
         }

         if (!domainsRead_)
         {
            State_(StatusLevel.Information, L("Cannot tell from here - the server's domain list could not be read, so which domains are linked to a directory is unknown. That is a connection problem rather than a configuration one."));
            return;
         }

         bool ldapOn = string.Equals(IniRead_("Enabled", "0"), "1", StringComparison.Ordinal);
         string searchBase = IniRead_("SearchBase", "");

         var problems = new List<string>();

         if (!ldapOn)
            problems.Add(L("directory access is switched off ([LDAP] Enabled=0 - the Directory authentication page turns it on)"));

         if (searchBase.Length == 0)
            problems.Add(L("no search base is set, so there is nowhere to enumerate from"));

         if (linkedDomains == 0)
            problems.Add(L("no domain on this server has an Active Directory domain name set, so no domain is eligible (set it on the domain's own page)"));

         if (problems.Count == 0)
         {
            State_(StatusLevel.Good, Plural(linkedDomains,
               "Yes. {0} domain is linked to a directory and will take part. Run a preview first: the shape of a directory is rarely what anyone expects, and the skipped rows are where the surprises are.",
               "Yes. {0} domains are linked to a directory and will take part. Run a preview first: the shape of a directory is rarely what anyone expects, and the skipped rows are where the surprises are."));
            return;
         }

         State_(StatusLevel.Warning, F("Not yet - {0}.", string.Join("; ", problems)));
      }

      private void State_(StatusLevel level, string text)
      {
         stateText_.Level = level;
         stateText_.Text = text;
      }

      /// <summary>What the last run said, at the level of what it says.</summary>
      private void Summary_(StatusLevel level, string text)
      {
         summaryText_.Level = level;
         summaryText_.Text = text;
         summaryText_.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// The identity of a run, as a string. Apply is enabled only while this matches
      /// what the last preview was run with, so changing the domain, the disable
      /// switch or any selection locks Apply again without anyone having to remember
      /// to.
      /// </summary>
      private string CurrentOptions_()
      {
         // Read from the INI, deliberately, and not from the editors above.
         //
         // A run uses what is SAVED - the server reads its own [LDAP] section - so an
         // edit that has not been saved changes nothing about what Apply would do, and
         // locking Apply for one would be telling the administrator that something
         // changed when it did not. A save does invalidate the preview, because the
         // values below then differ; Save_ also invalidates explicitly, so it holds
         // even for a save that writes the same values back.
         return (domainChoice_.SelectedIndex <= 0 ? "" : (string)domainChoice_.SelectedItem)
                + "|" + (disableMissing_.IsChecked is true ? "1" : "0")
                + "|" + IniRead_("SyncFilter", "")
                + "|" + IniRead_("SyncMailAttribute", "mail")
                + "|" + IniRead_("SyncUsernameAttribute", "sAMAccountName")
                + "|" + IniRead_("SyncDisplayNameAttribute", "displayName")
                + "|" + IniRead_("SyncMaxUsers", "5000")
                + "|" + IniRead_("Enabled", "0")
                + "|" + IniRead_("SearchBase", "");
      }

      private void InvalidatePreview_()
      {
         if (loading_)
            return;

         previewedOptions_ = null;

         if (applyButton_ != null)
            applyButton_.IsEnabled = false;
      }

      private async Task RunAsync_(bool apply)
      {
         string domain = domainChoice_.SelectedIndex <= 0 ? "" : (string)domainChoice_.SelectedItem;
         bool disableMissing = disableMissing_.IsChecked is true;
         string options = CurrentOptions_();

         previewButton_.IsEnabled = false;
         applyButton_.IsEnabled = false;
         Summary_(StatusLevel.Information, apply ? L("Applying...") : L("Reading the directory..."));
         attentionText_.Text = "";
         rows_.Children.Clear();

         try
         {
            // Off the UI thread: a directory read is a network round trip per page, and
            // a large directory takes long enough that a frozen window would look like
            // a hang.
            Tuple<bool, string> outcome = await Task.Run(() => Invoke_(domain, apply, disableMissing));

            DirectorySyncReport report = DirectorySyncReport.Parse(outcome.Item1, outcome.Item2);

            Render_(report, apply);

            // Only a SUCCESSFUL preview unlocks Apply, and only for the options it was
            // run with. A refused preview tells the administrator nothing about what an
            // apply would do, so it must not be treated as one that was read.
            if (!apply && report.Succeeded)
            {
               previewedOptions_ = options;
               previewReport_ = report;
               applyButton_.IsEnabled = true;
            }
            else if (apply)
            {
               // The world has changed, so the preview that was read no longer
               // describes what another apply would do.
               previewedOptions_ = null;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Summary_(StatusLevel.Critical, L("The run failed: ") + ServerSession.DescribeComError(ex));
            previewedOptions_ = null;
         }
         finally
         {
            previewButton_.IsEnabled = true;
         }
      }

      private async Task ApplyAsync_()
      {
         // The gate. Not a courtesy check - the button is disabled outside this
         // condition too, and this catches the case where the options changed between
         // the click and the handler.
         if (previewedOptions_ == null || previewedOptions_ != CurrentOptions_())
         {
            Summary_(StatusLevel.Warning, L("Run a preview first: the options have changed since the last one, so it no longer describes what this would do."));
            applyButton_.IsEnabled = false;
            return;
         }

         // The question repeats what the preview just showed, the domain and the counts,
         // where it said only that mailboxes would be created and updated.
         DirectorySyncReport preview = previewReport_;
         string domain = domainChoice_.SelectedIndex <= 0 ? "" : (string)domainChoice_.SelectedItem;
         if (!MessageBox.Confirm(
            Confirmations.DirectorySyncApply(domain,
               preview?.Count(DirectorySyncRowKind.Create) ?? 0,
               preview?.Count(DirectorySyncRowKind.Update) ?? 0,
               preview?.Count(DirectorySyncRowKind.Disable) ?? 0,
               preview?.Count(DirectorySyncRowKind.Skip) ?? 0,
               disableMissing_.IsChecked is true),
            L("Apply directory synchronisation"),
            L("_Apply"),
            destructive: true))
            return;

         // Re-checked AFTER the dialog, not only before it.
         //
         // MessageBox.Show blocks this method but not the world: it pumps messages, so
         // while it is up the domain list can be reloaded, the checkbox can be toggled
         // by a keyboard accelerator, and - the case that actually matters - somebody
         // else can edit hMailServer.INI, which CurrentOptions_ reads. The gate above
         // ran against the options as they were when the button was clicked; this one
         // runs against the options the run is about to use. Without it, the whole
         // point of the gate is available to be raced by the confirmation intended to
         // make it safer.
         if (previewedOptions_ != CurrentOptions_())
         {
            Summary_(StatusLevel.Warning, L("Nothing was applied: the options changed while the confirmation was open, so the preview no longer describes what this would do. Run the preview again."));
            applyButton_.IsEnabled = false;
            previewedOptions_ = null;
            return;
         }

         await RunAsync_(true);
      }

      /// <summary>The COM call itself, on a background thread.</summary>
      private static Tuple<bool, string> Invoke_(string domain, bool apply, bool disableMissing)
      {
         dynamic settings = null;

         try
         {
            settings = ServerSession.Current.Application.Settings;

            // `text` is initialised rather than merely declared because the call is
            // dispatched dynamically: definite-assignment analysis cannot see through
            // a dynamic invocation, so an out parameter it fills does not count as
            // assigned at compile time.
            string text = string.Empty;

            bool ok = apply
               ? (bool)settings.ApplyDirectorySync(domain, disableMissing, out text)
               : (bool)settings.PreviewDirectorySync(domain, disableMissing, out text);

            return Tuple.Create(ok, text ?? string.Empty);
         }
         finally
         {
            ServerSession.Release((object)settings);
         }
      }

      private void Render_(DirectorySyncReport report, bool applied)
      {
         rows_.Children.Clear();

         Summary_(report.Succeeded ? StatusLevel.Information : StatusLevel.Critical, report.Summary);

         if (!report.Succeeded)
         {
            // "Nothing was changed" only when there is genuinely nothing to show. A
            // failed run normally means the run never started - LDAP off, no search
            // base, the directory unreachable - and then this is true and the row list
            // is empty anyway. But it is NOT safe to assert as a rule: a run can fail
            // partway with rows already carried out, and printing "nothing was changed"
            // over a list that says otherwise is the worst thing this page could do.
            // So the claim is made only when the list is empty, and the rows are drawn
            // either way.
            attentionText_.Text = report.Rows.Count == 0
               ? (applied ? L("Nothing was changed.") : L("Nothing was read."))
               : (applied
                  ? L("The run did not finish. The rows below say what had already been done before it stopped - read them before running it again.")
                  : L("The run did not finish. What was read before it stopped is below."));

            RenderRows_(report);
            return;
         }

         int attention = report.AttentionCount;

         if (applied)
         {
            int failed = 0;
            int done = 0;

            foreach (DirectorySyncRow row in report.Rows)
            {
               if (row.Outcome == DirectorySyncOutcome.Failed) failed++;
               else if (row.Outcome == DirectorySyncOutcome.Done) done++;
            }

            // Deliberately phrased around what FAILED rather than what succeeded. A run
            // reports success when it completed, not when every account was created -
            // Apply_ counts individual refusals in `failed` and leaves the run itself
            // successful - so "applied" on its own tells an administrator very little.
            attentionText_.Text = failed == 0
               ? Plural(done, "{0} change carried out, none failed.", "{0} changes carried out, none failed.")
               : PluralF(failed,
                  "{0} of {1} changes could not be carried out and is listed first, with the server's reason on it. The rest were applied.",
                  "{0} of {1} changes could not be carried out and are listed first, with the server's reason on each. The rest were applied.",
                  failed, failed + done);
         }
         else
         {
            attentionText_.Text = attention == 0
               ? L("Nothing here needs a decision: no mailbox is being skipped and none is being disabled.")
               : Plural(attention,
                  "{0} row needs reading before you apply - a skipped entry is a mailbox somebody expects and will not get, and a disabled one is a mailbox being taken away.",
                  "{0} rows need reading before you apply - a skipped entry is a mailbox somebody expects and will not get, and a disabled one is a mailbox being taken away.");
         }

         RenderRows_(report);
      }

      private void RenderRows_(DirectorySyncReport report)
      {
         // The rows that cost something first. Four hundred lines of equal weight is a
         // log; the six that matter have to be at the top or they are not read at all.
         foreach (DirectorySyncRow row in report.Rows.Where(r => r.NeedsAttention))
            rows_.Children.Add(Row_(row, true));

         foreach (DirectorySyncRow row in report.Rows.Where(r => !r.NeedsAttention))
            rows_.Children.Add(Row_(row, false));

         if (report.SkipBreakdown.Count == 0)
            return;

         // The aggregate, drawn as an aggregate. It used to arrive here as extra rows
         // headed "(no address)" - the same words the server uses for a real directory
         // entry with no mail attribute - so a count of skipped entries read as two more
         // unidentifiable mailboxes. It is also placed HERE, directly under the rows it
         // summarises, rather than trailing four hundred lines below them.
         var skipped = new TextBlock { Text = L("Skipped, by reason"), Margin = new Thickness(0, 16, 0, 4) };
         skipped.SetResourceReference(StyleProperty, "TextBodyStrong");
         System.Windows.Automation.AutomationProperties.SetHeadingLevel(skipped, System.Windows.Automation.AutomationHeadingLevel.Level2);
         rows_.Children.Add(skipped);

         foreach (string line in report.SkipBreakdown)
         {
            // The server writes "22\tthere is no domain of that name on this server".
            // Split so the count and the reason are two readable pieces rather than one
            // line with an unexplained gap in it.
            string[] parts = line.Split('\t');

            var skipLine = new TextBlock
            {
               Text = parts.Length >= 2
                  ? parts[0].Trim() + "  -  " + string.Join(" ", parts, 1, parts.Length - 1).Trim()
                  : line,
               TextWrapping = TextWrapping.Wrap,
               Margin = new Thickness(0, 0, 0, 2)
            };
            skipLine.SetResourceReference(StyleProperty, "TextCaption");
            rows_.Children.Add(skipLine);
         }
      }

      private static FrameworkElement Row_(DirectorySyncRow row, bool emphasise)
      {
         var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 8) };

         var head = new TextBlock
         {
            Text = (row.Address.Length == 0 ? L("(no address)") : row.Address) + "  -  " + row.Action,
            TextWrapping = TextWrapping.Wrap
         };
         head.SetResourceReference(StyleProperty, emphasise ? "TextBodyStrong" : "TextBody");
         panel.Children.Add(head);

         var detail = new TextBlock { Text = row.Detail, TextWrapping = TextWrapping.Wrap };
         detail.SetResourceReference(StyleProperty, "TextCaption");
         panel.Children.Add(detail);

         return panel;
      }
   }
}
