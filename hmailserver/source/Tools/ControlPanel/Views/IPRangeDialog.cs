// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Full tabbed editor for one IP security range — the complete set of
   /// IInterfaceSecurityRange options (connections, delivery with each direction's
   /// SMTP authentication, anti-spam/anti-virus and expiry), of which the page's
   /// card holds the first two. On the standard frame: four tabs sized to the tallest of them
   /// so the window does not jump on every click of the strip, every field a
   /// <see cref="FieldRow"/>, a priority or an expiry that will not parse said on
   /// the field itself - which also brings its tab to the front - and a save the
   /// server refuses said in a notice above the tabs.
   /// </summary>
   public class IPRangeDialog : FluentDialogWindow
   {
      /// <summary>
      /// The width this dialog asks the frame for where its four headers fit in it;
      /// wider where they do not (DialogFields.WidthForTabs). The tabs are measured
      /// against the body inside the width it is given.
      /// </summary>
      private const double DialogWidth = 560;

      private readonly int rangeId_;

      /// <summary>Whether the range was saved before the dialog closed.</summary>
      internal bool Saved { get; private set; }

      // General
      private readonly TextBox name_ = new();
      private readonly TextBox lower_ = new();
      private readonly TextBox upper_ = new();
      private readonly TextBox priority_ = new();
      private FieldRow priorityRow_;

      // Connections, and the TLS a sign-in needs, in the page's words (review finding F229).
      private readonly CheckBox smtp_ = new() { Content = L("Allow SM_TP connections") };
      private readonly CheckBox imap_ = new() { Content = L("Allow _IMAP connections") };
      private readonly CheckBox pop3_ = new() { Content = L("Allow _POP3 connections") };
      private readonly CheckBox tlsAuth_ = new() { Content = L("Require TLS _before authentication") };

      // Delivery: each direction allowed, and only after SMTP authentication, on one
      // row, as the page's permissions card shows them, where they were two tabs
      // apart - Relaying and Require auth - each in its own words (review finding
      // F229). The boxes are named by their row and column (DeliveryRow).
      private readonly CheckBox ll_ = new();
      private readonly CheckBox lr_ = new();
      private readonly CheckBox rl_ = new();
      private readonly CheckBox rr_ = new();
      private readonly CheckBox authLL_ = new();
      private readonly CheckBox authLE_ = new();
      private readonly CheckBox authEL_ = new();
      private readonly CheckBox authEE_ = new();

      // What relaying without authentication means for this range (Services/RelayExposure).
      private readonly InlineNotice relayNotice_ = DialogFields.Notice();

      // Protection + expiry
      private readonly CheckBox spam_ = new() { Content = L("Enable _anti-spam for this range") };
      private readonly CheckBox virus_ = new() { Content = L("Enable anti-_virus for this range") };
      private readonly CheckBox expires_ = new() { Content = L("This range _expires") };

      // The expiry as a date and a time of day, enabled only while the range expires
      // (review finding F197): it was one box of "YYYY-MM-DD HH:MM:SS" that showed a
      // stored time - "2026-10-08 04:13:09" - with "This range expires" unticked.
      private readonly DatePicker expiresDate_ = new() { MinWidth = 180 };
      private readonly Wpf.Ui.Controls.TextBox expiresTime_ = new() { Width = 100, PlaceholderText = "00:00", Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0) };
      private FieldRow expiresRow_;

      private readonly InlineNotice notice_ = DialogFields.Notice();

      public IPRangeDialog(Window owner, int rangeId)
      {
         rangeId_ = rangeId;
         Owner = owner;
         Title = L("IP range");

         var tabs = new TabControl { Background = System.Windows.Media.Brushes.Transparent, BorderThickness = new Thickness(0) };
         tabs.Items.Add(new TabItem { Header = L("General"), Content = BuildGeneral() });
         tabs.Items.Add(new TabItem { Header = L("Connections"), Content = BuildConnections() });
         tabs.Items.Add(new TabItem { Header = L("Delivery"), Content = BuildDelivery() });
         tabs.Items.Add(new TabItem { Header = L("Protection"), Content = BuildProtection() });

         // One height for every tab, taken from the tallest: a window sized to
         // its content would otherwise grow and shrink with each click on the
         // strip, which is the tabbed dialog's version of a jumping layout.
         // As wide as the headers need in the language in force, so that none is
         // cut behind a chevron: in German "Authentifizierung" and "Schutz" were
         // (review finding F193).
         double width = DialogFields.WidthForTabs(tabs, DialogWidth);
         DialogFields.FitTabs(tabs, DialogFields.BodyWidth(width));

         var body = new StackPanel();
         body.Children.Add(notice_);
         body.Children.Add(tabs);

         // Enter saves, Escape cancels; the frame wires both.
         var save = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         save.Click += (s, e) => Save();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         UseFrame(L("IP range"), body, save, cancel, width: width);
         Loaded += (s, e) => Load();
      }

      private ScrollViewer BuildGeneral()
      {
         var p = DialogFields.TabPanel();
         p.Children.Add(Label(L("_Name"), name_));
         p.Children.Add(Label(L("_Lower IP address"), lower_));
         p.Children.Add(Label(L("_Upper IP address"), upper_));
         priorityRow_ = Label(L("_Priority (higher wins when ranges overlap)"), priority_);
         p.Children.Add(priorityRow_);
         return DialogFields.Scroll(p);
      }

      private ScrollViewer BuildConnections()
      {
         var p = DialogFields.TabPanel();
         p.Children.Add(DialogFields.Check(smtp_));
         p.Children.Add(DialogFields.Check(imap_));
         p.Children.Add(DialogFields.Check(pop3_));
         p.Children.Add(DialogFields.Separator());
         p.Children.Add(DialogFields.Check(tlsAuth_));
         return DialogFields.Scroll(p);
      }

      private ScrollViewer BuildDelivery()
      {
         var p = DialogFields.TabPanel();
         p.Children.Add(DeliveryRow.BuildGrid(new[]
         {
            (L("_Local to local"), ll_, authLL_),
            (L("Lo_cal to external"), lr_, authLE_),
            (L("E_xternal to local"), rl_, authEL_),
            (L("External to external (rela_y)"), rr_, authEE_)
         }));

         relayNotice_.Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0);
         p.Children.Add(relayNotice_);
         foreach (CheckBox box in new[] { rr_, authEE_ })
         {
            box.Checked += (s, e) => ShowRelayExposure_();
            box.Unchecked += (s, e) => ShowRelayExposure_();
         }

         lower_.TextChanged += (s, e) => ShowRelayExposure_();
         upper_.TextChanged += (s, e) => ShowRelayExposure_();
         return DialogFields.Scroll(p);
      }

      /// <summary>The Delivery tab's word on relaying without authentication, as the page's card says it.</summary>
      private void ShowRelayExposure_()
      {
         RelayExposure.Reach reach = RelayExposure.Of(lower_.Text, upper_.Text, rr_.IsChecked is true, authEE_.IsChecked is true);
         string notice = RelayExposure.Notice(reach);
         if (notice == null)
            relayNotice_.Hide();
         else
            relayNotice_.Show(RelayExposure.LevelOf(reach), notice);
      }

      private ScrollViewer BuildProtection()
      {
         var p = DialogFields.TabPanel();
         p.Children.Add(DialogFields.Check(spam_));
         p.Children.Add(DialogFields.Check(virus_));
         p.Children.Add(DialogFields.Separator());
         p.Children.Add(DialogFields.Check(expires_));
         // The day and the time of day on one line, under one caption: the tab is the
         // dialog's tallest, and a message under a second row ran out of it.
         var when = new StackPanel { Orientation = Orientation.Horizontal };
         when.Children.Add(expiresDate_);
         when.Children.Add(expiresTime_);
         // The caption names the row and its key reaches the date; the time box is named for itself.
         System.Windows.Automation.AutomationProperties.SetName(expiresTime_, L("Expiry time (HH:MM; empty = 00:00)"));
         expiresRow_ = Label(L("Expires _on (server time)"), when);
         p.Children.Add(expiresRow_);
         DialogFields.EnabledBy(expires_, expiresRow_);

         // An error goes when what it is about is edited.
         expiresDate_.SelectedDateChanged += (s, e) => expiresRow_.Error = null;
         expiresTime_.TextChanged += (s, e) => expiresRow_.Error = null;
         return DialogFields.Scroll(p);
      }

      private dynamic FindRange(dynamic ranges)
      {
         int count = (int)ranges.Count;
         for (int i = 0; i < count; i++)
         {
            dynamic r = ranges.Item[i];
            if ((int)r.ID == rangeId_)
               return r;
            ServerSession.Release(r);
         }
         return null;
      }

      private void Load()
      {
         dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
         try
         {
            dynamic r = FindRange(ranges);
            if (r == null) { Close(); return; }

            name_.Text = (string)r.Name ?? "";
            lower_.Text = (string)r.LowerIP ?? "";
            upper_.Text = (string)r.UpperIP ?? "";
            priority_.Text = ((int)r.Priority).ToString();

            smtp_.IsChecked = (bool)r.AllowSMTPConnections;
            imap_.IsChecked = (bool)r.AllowIMAPConnections;
            pop3_.IsChecked = (bool)r.AllowPOP3Connections;

            ll_.IsChecked = (bool)r.AllowDeliveryFromLocalToLocal;
            lr_.IsChecked = (bool)r.AllowDeliveryFromLocalToRemote;
            rl_.IsChecked = (bool)r.AllowDeliveryFromRemoteToLocal;
            rr_.IsChecked = (bool)r.AllowDeliveryFromRemoteToRemote;

            authLL_.IsChecked = (bool)r.RequireSMTPAuthLocalToLocal;
            authLE_.IsChecked = (bool)r.RequireSMTPAuthLocalToExternal;
            authEL_.IsChecked = (bool)r.RequireSMTPAuthExternalToLocal;
            authEE_.IsChecked = (bool)r.RequireSMTPAuthExternalToExternal;
            tlsAuth_.IsChecked = (bool)r.RequireSSLTLSForAuth;

            spam_.IsChecked = (bool)r.EnableSpamProtection;
            virus_.IsChecked = (bool)r.EnableAntiVirus;
            expires_.IsChecked = (bool)r.Expires;
            // A VT_DATE, the server's local time (DateTime::GetVariant). A range that never
            // expired holds no date worth showing: the picker stays empty for one to be chosen.
            try
            {
               DateTime when = (DateTime)r.ExpiresTime;
               if (when.Year >= 1970)
               {
                  expiresDate_.SelectedDate = when.Date;
                  expiresTime_.Text = TimeOfDayText.Format(when);
               }
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               expiresDate_.SelectedDate = null;
               expiresTime_.Text = "";
            }

            ServerSession.Release(r);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not load the range: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            Close();
         }
         finally
         {
            ServerSession.Release(ranges);
         }
      }

      private void Save()
      {
         notice_.Hide();
         DialogFields.ClearErrors(priorityRow_, expiresRow_);

         // A priority that will not parse used to be dropped on the floor: the
         // dialog closed, the range kept its old priority and nothing said so.
         // It is a field that is wrong, so the field says so.
         int? priority = null;
         string priorityText = priority_.Text.Trim();
         if (priorityText.Length > 0)
         {
            if (!int.TryParse(priorityText, out int parsed))
            {
               DialogFields.ShowError(priorityRow_, L("Enter a whole number."));
               return;
            }

            priority = parsed;
         }

         // A range set to expire needs the day it expires, and a time of day that is one.
         DateTime? expiry = null;
         if (expires_.IsChecked is true)
         {
            if (expiresDate_.SelectedDate is not DateTime day)
            {
               DialogFields.ShowError(expiresRow_, L("Choose the day the range expires."));
               return;
            }

            if (!TimeOfDayText.TryParse(expiresTime_.Text, out TimeSpan time))
            {
               DialogFields.ShowError(expiresRow_, L("Enter the time as HH:MM, such as 18:30."));
               // The row's first editor is the date: the keyboard goes on to the time.
               Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Input, new Action(() => expiresTime_.Focus()));
               return;
            }

            expiry = day.Date + time;
         }

         // Permissions that make an open relay are asked about first (review finding F229).
         if (RelayExposure.Of(lower_.Text, upper_.Text, rr_.IsChecked is true, authEE_.IsChecked is true) == RelayExposure.Reach.Internet
             && !MessageBox.Confirm(RelayExposure.SaveQuestion(name_.Text.Trim()), L("Control Panel"), L("_Save"), destructive: true))
            return;

         dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
         try
         {
            dynamic r = FindRange(ranges);
            if (r == null) { Close(); return; }

            r.Name = name_.Text.Trim();
            if (lower_.Text.Trim().Length > 0) r.LowerIP = lower_.Text.Trim();
            if (upper_.Text.Trim().Length > 0) r.UpperIP = upper_.Text.Trim();
            if (priority.HasValue) r.Priority = priority.Value;

            r.AllowSMTPConnections = smtp_.IsChecked is true;
            r.AllowIMAPConnections = imap_.IsChecked is true;
            r.AllowPOP3Connections = pop3_.IsChecked is true;

            r.AllowDeliveryFromLocalToLocal = ll_.IsChecked is true;
            r.AllowDeliveryFromLocalToRemote = lr_.IsChecked is true;
            r.AllowDeliveryFromRemoteToLocal = rl_.IsChecked is true;
            r.AllowDeliveryFromRemoteToRemote = rr_.IsChecked is true;

            r.RequireSMTPAuthLocalToLocal = authLL_.IsChecked is true;
            r.RequireSMTPAuthLocalToExternal = authLE_.IsChecked is true;
            r.RequireSMTPAuthExternalToLocal = authEL_.IsChecked is true;
            r.RequireSMTPAuthExternalToExternal = authEE_.IsChecked is true;
            r.RequireSSLTLSForAuth = tlsAuth_.IsChecked is true;

            r.EnableSpamProtection = spam_.IsChecked is true;
            r.EnableAntiVirus = virus_.IsChecked is true;
            r.Expires = expires_.IsChecked is true;
            // As a date, which the server takes as its own local time; a range that does
            // not expire keeps the time it has.
            if (expiry.HasValue)
               r.ExpiresTime = expiry.Value;

            r.Save();
            ServerSession.Release(r);
            Saved = true;
            Close();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not save the range: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(ranges);
         }
      }

      /// <summary>
      /// A caption and its editor as one row, which names the editor to UI
      /// Automation. A TextBlock above a control tells UI Automation nothing, so
      /// the four boxes on the General tab announced themselves as "edit, edit,
      /// edit, edit" on a dialog where two of them are the ends of an IP range.
      /// The check boxes are named by their own Content, which is why the group
      /// captions here are <see cref="DialogFields.Caption"/> and not rows.
      /// </summary>
      private static FieldRow Label(string text, FrameworkElement editor) => DialogFields.Field(text, editor);
   }
}
