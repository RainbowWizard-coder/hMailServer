// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Input;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// A paused mail server, said under every page (review finding F019). Server
   /// status's Pause stops the server's engine - no connection is accepted and
   /// nothing is delivered - while the Windows service, and so this Control Panel's
   /// link, keeps running; the shell's pill went on saying Connected in green on
   /// every page, since it is fed by the link alone, and nothing outside Server
   /// status read the server's state, so a pause that was forgotten was a server
   /// that took no mail with no sign of it anywhere. While the server reports
   /// Stopped the window shows a Warning bar at its foot, "The mail server is paused
   /// - nothing is accepted or delivered.", with Resume the server, and the pill
   /// says Paused at the Warning level. The state is asked as each page is entered
   /// (EnterPage), on the Dashboard's two-second refresh, and by Server status
   /// whenever it reads it; a state that cannot be read shows no bar, which claims
   /// nothing.
   /// </summary>
   public partial class MainWindow
   {
      // ServerStatus.StateStopped, as the COM enum numbers it.
      private const int ServerStateStopped_ = 1;

      // Whether the server last said it was paused; the pill and the bar show it.
      private bool serverPaused_;

      /// <summary>Asks the server whether its engine is paused, and shows the answer.</summary>
      internal void RefreshServerPaused()
      {
         ServerSession session = ServerSession.Current;
         if (!connected_ || session == null || !session.IsConnected)
         {
            ShowServerPaused_(false);
            return;
         }

         try
         {
            ShowServerState((int)session.Application.ServerState);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Cannot tell: no bar, which says nothing it does not know.
            ShowServerPaused_(false);
         }
      }

      /// <summary>
      /// The server's state as a page read it (ServerStatus: 1 stopped, 3 running,
      /// 5 draining): the bar and the pill follow it, so that Server status's Pause
      /// and Resume are seen at once on the rest of the window.
      /// </summary>
      public void ShowServerState(int state) => ShowServerPaused_(connected_ && state == ServerStateStopped_);

      private void ShowServerPaused_(bool paused)
      {
         bool shown = PausedBar.Visibility == Visibility.Visible;
         if (paused == serverPaused_ && paused == shown)
            return;

         serverPaused_ = paused;
         if (!paused && (PausedBar.IsKeyboardFocusWithin || ResumeServerButton.IsKeyboardFocused))
            KeyboardToPage_();
         PausedBar.Visibility = paused ? Visibility.Visible : Visibility.Collapsed;
         ResumeServerButton.IsEnabled = true;

         // The pill says Paused while the link is up and the engine is not.
         ShowLinkState_(linkState_);
      }

      /// <summary>
      /// Resume the server, from the bar: Application.Start(), as Server
      /// status's Resume, which asks nothing first - starting the engine closes
      /// nothing. Then the state is asked again and the page entered again, unless
      /// it holds unsaved changes, so that Server status says Running.
      /// </summary>
      private void ResumePausedServer_Click(object sender, RoutedEventArgs e)
      {
         ServerSession session = ServerSession.Current;
         if (session == null)
            return;

         ResumeServerButton.IsEnabled = false;
         Mouse.OverrideCursor = Cursors.Wait;
         try
         {
            session.Application.Start();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Services.Toast.Failure(F("The server state could not be changed: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            Mouse.OverrideCursor = null;
            ResumeServerButton.IsEnabled = true;
         }

         if (CurrentPageHasUnsavedChanges_())
            RefreshServerPaused();
         else
            EnterPage(ContentHost.Content);
      }
   }
}
