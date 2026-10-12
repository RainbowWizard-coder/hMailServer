// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class ConnectView : UserControl
   {
      private readonly Action onConnected_;
      private bool connecting_;

      // Where the keyboard goes as the page opens: the password, or - for Connect
      // to another server - the server's box, its text chosen to be typed over.
      private bool focusServer_;

      public ConnectView(Action onConnected) : this(onConnected, null, StatusLevel.Normal)
      {
      }

      /// <summary>
      /// The sign-in page after a session ended (review finding F012): Sign out and
      /// Sign in again come back to the server and the account that were signed in,
      /// the keyboard in the password; Connect to another server to the server's
      /// box, ready to type another.
      /// </summary>
      public ConnectView(Action onConnected, string host, string userName, bool focusServer) : this(onConnected)
      {
         if (!string.IsNullOrEmpty(host))
            HostBox.Text = host;
         if (!string.IsNullOrEmpty(userName))
            UserBox.Text = userName;
         focusServer_ = focusServer;
      }

      /// <summary>
      /// The sign-in page with a notice above the fields: after an administrator's
      /// install of an update, when the Control Panel that opened again could not
      /// sign in by itself (B-signin), the outcome's first sentence.
      /// </summary>
      public ConnectView(Action onConnected, string notice, StatusLevel level)
      {
         InitializeComponent();
         // An example of what the box takes, marked as one, where the placeholder was
         // the box's caption again with the default, "Server (localhost)"
         // (docs/ControlPanelDesign.md: a placeholder is never the field's name).
         HostBox.PlaceholderText = ExamplePlaceholder.Of("mail.example.com"); // no-loc: an example host
         onConnected_ = onConnected;
         BuildLanguageBox_();
         LanguageBox.KeyDown += Language_KeyDown;

         // The server and the user name of the last sign-in that worked, where
         // every launch opened on localhost and Administrator (review finding F033).
         // The XAML's literals are the fallback SignInMemory gives as well.
         (string host, string user) = SignInMemory.Read();
         HostBox.Text = host;
         UserBox.Text = user;

         // Enter on a field signs in through the default button, where a KeyDown
         // handler on the whole page started a sign-in (F030). The default button takes
         // every Enter that the control with the keyboard leaves unhandled, the closed
         // Language box's too: the box opens its list on Enter instead
         // (Language_KeyDown). Collapsed while a sign-in runs, the button presses
         // nothing then; Connect_Click's guard stays.
         ConnectButton.IsDefault = true;

         if (!string.IsNullOrEmpty(notice))
         {
            ErrorNotice.Level = level;
            ErrorNotice.Text = notice;
            ErrorNotice.Visibility = Visibility.Visible;
         }

         Loaded += (s, e) =>
         {
            if (!focusServer_)
            {
               PasswordBox.Focus();
               return;
            }

            HostBox.Focus();
            HostBox.SelectAll();
         };
      }

      private async void Connect_Click(object sender, RoutedEventArgs e)
      {
         // Enter on a field that is already connecting would start a second
         // attempt on top of the first.
         if (connecting_)
            return;

         string host = HostBox.Text.Trim();
         string user = UserBox.Text.Trim();
         string password = PasswordBox.Password;

         SetBusy_(true, F("Contacting {0}…", host.Length == 0 ? "localhost" : host));

         try
         {
            // Is the machine even there? This runs off the UI thread and answers
            // in seconds, where the DCOM activation below would sit on the UI
            // thread for RPC's own timeout - not redrawing, not moving, and
            // wearing "(Not Responding)" in the title bar. See HostReachability
            // for why the activation itself cannot simply be moved with it.
            HostReachability.Result reach =
               await HostReachability.CheckAsync(host, HostReachability.DefaultTimeout);

            if (!reach.Reachable)
            {
               Fail_(reach.Error, StatusLevel.Critical);
               return;
            }

            BusyText.Text = L("Signing in…");
            Announce.Request(BusyText);

            // Back on the UI thread deliberately: the COM proxy belongs to the
            // apartment that creates it, and this is the apartment that has to
            // keep using it for the rest of the session. Yield once first so the
            // busy state above actually paints before the blocking calls start.
            await Task.Delay(50);

            var session = new ServerSession();
            bool cancelled = false;

            // session.Connect tries the password. On a server that enforces a second
            // factor on the administrator it fails with SecondFactorRequired set, so
            // TryWithCode_ prompts once and retries with the code. Prompting only once
            // is deliberate: a wrong password sets the same flag, and a re-prompt loop
            // would hide that the password was the problem. The `&&` short-circuits, so
            // TryWithCode_ (which shows a dialog) runs only when a code is actually wanted.
            if (!session.Connect(host, user, password, out string error)
                && (!session.SecondFactorRequired || !TryWithCode_(session, host, user, password, out cancelled, out error)))
            {
               // A password or a code the server refused, or the prompt cancelled, is a
               // Warning; a server that could not be asked is Critical (F029).
               Fail_(error ?? L("Connection failed."), SignInMemory.FailureLevel(session.CredentialRefused, cancelled));
               return;
            }

            ServerSession.SetCurrent(session);
            onConnected_();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Connect reports its own failures through the out parameter, so
            // reaching here means something further out went wrong. Showing it
            // beats an unhandled exception dialog over the sign-in card.
            Fail_(ex.Message, StatusLevel.Critical);
         }
      }

      // Prompts for the administrator's one-time code and retries the connect with
      // it. Returns false (with a reason in error) when the user cancels or the
      // code is refused; the caller then fails the sign-in as normal.
      private bool TryWithCode_(ServerSession session, string host, string user, string password, out bool cancelled, out string error)
      {
         var prompt = new TotpPromptDialog(Window.GetWindow(this));

         cancelled = prompt.ShowDialog() != true;
         if (cancelled)
         {
            error = L("Sign-in cancelled.");
            return false;
         }

         BusyText.Text = L("Signing in…");
         Announce.Request(BusyText);
         return session.Connect(host, user, password, prompt.Code, out error);
      }

      private bool languageReady_;

      private void BuildLanguageBox_()
      {
         foreach (Loc.Language language in Loc.Languages)
         {
            var item = new ComboBoxItem { Content = L(language.NativeName), Tag = language.Tag };
            LanguageBox.Items.Add(item);
            if (string.Equals(language.Tag, LanguageChoice.Stored, StringComparison.OrdinalIgnoreCase))
               LanguageBox.SelectedItem = item;
         }

         if (LanguageBox.SelectedIndex < 0)
            LanguageBox.SelectedIndex = 0;

         languageReady_ = true;
      }

      // A choice made from the open list is applied when the list closes; one
      // made with the arrow keys on the closed box is applied at once. Either
      // way LanguageChoice asks first, because the answer restarts the program.
      private void Language_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         if (languageReady_ && !LanguageBox.IsDropDownOpen)
            ApplyLanguage_();
      }

      private void Language_DropDownClosed(object sender, EventArgs e) => ApplyLanguage_();

      // Enter on the closed box opens its list, as F4 and Alt+Down do, where the
      // page's default button took it and started a sign-in (review finding F030):
      // a closed ComboBox leaves Enter unhandled. In the open list the box's own
      // handling chooses the item and closes it, and this handler is not reached.
      private void Language_KeyDown(object sender, System.Windows.Input.KeyEventArgs e)
      {
         if (e.Key != System.Windows.Input.Key.Enter || LanguageBox.IsDropDownOpen)
            return;

         LanguageBox.IsDropDownOpen = true;
         e.Handled = true;
      }

      private void ApplyLanguage_()
      {
         if (!languageReady_ || LanguageBox.SelectedItem is not ComboBoxItem item)
            return;

         string tag = (string)item.Tag;
         if (string.Equals(tag, LanguageChoice.Stored, StringComparison.OrdinalIgnoreCase) || LanguageChoice.Offer(tag))
            return;

         // Declined: back to the language in force, without offering again.
         languageReady_ = false;
         foreach (ComboBoxItem candidate in LanguageBox.Items)
         {
            if (string.Equals((string)candidate.Tag, LanguageChoice.Stored, StringComparison.OrdinalIgnoreCase))
               LanguageBox.SelectedItem = candidate;
         }
         languageReady_ = true;
      }

      /// <summary>
      /// Says why the sign-in failed, at <paramref name="level"/>: Warning for what
      /// the reader can type again, Critical for a server that could not be asked
      /// (SignInMemory.FailureLevel). The notice was Critical for every failure.
      /// </summary>
      private void Fail_(string message, StatusLevel level)
      {
         ErrorNotice.Level = level;
         ErrorNotice.Text = message;
         ErrorNotice.Visibility = Visibility.Visible;
         // Under the fold in a window shorter than the card: the page scrolls
         // the notice into view, so that a failed sign-in is seen as well as
         // announced.
         ErrorNotice.BringIntoView();
         SetBusy_(false, null);
         PasswordBox.Focus();
      }

      /// <summary>
      /// Swaps the Sign in button for a progress ring and locks the fields, so
      /// the credentials cannot be edited underneath an attempt that is using
      /// them. The button is hidden rather than merely disabled: a greyed-out
      /// button is how this screen used to say "working", and it is
      /// indistinguishable from how every screen says "you cannot do that".
      /// </summary>
      private void SetBusy_(bool busy, string message)
      {
         connecting_ = busy;

         if (busy)
         {
            BusyText.Text = message ?? L("Signing in…");
            ErrorNotice.Visibility = Visibility.Collapsed;

            // The busy line is a polite live region: a screen reader hears
            // "Contacting localhost…" and then "Signing in…", where it heard
            // nothing until the failure or the next page (review finding F031).
            Announce.Request(BusyText);
         }

         ConnectButton.Visibility = busy ? Visibility.Collapsed : Visibility.Visible;
         BusyPanel.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;

         HostBox.IsEnabled = !busy;
         UserBox.IsEnabled = !busy;
         PasswordBox.IsEnabled = !busy;
      }
   }
}
