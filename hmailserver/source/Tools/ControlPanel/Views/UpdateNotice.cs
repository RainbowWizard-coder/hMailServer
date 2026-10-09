// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// "hMailServer 6.3.7 is available." in one line with Open Updates, on Server
   /// status, the Dashboard and About, while a newer release is waiting - and
   /// nothing while none is. The release is checked for, downloaded and installed
   /// on the Updates page (FeatureSettingsView.Updates.cs); About showed the
   /// version and not that a newer one was waiting (review finding F272).
   ///
   /// Read once as the page opens, not with the Dashboard's two-second refresh:
   /// the check runs once a day. Shown only to a credential that may open the
   /// Updates page, and a read that fails says nothing here - the page that owns
   /// the read says it.
   /// </summary>
   internal static class UpdateNotice
   {
      /// <summary>Puts the notice in <paramref name="host"/>, or empties and hides it.</summary>
      public static void ShowIn(Panel host, string automationId)
      {
         if (host == null)
            return;

         host.Children.Clear();
         string text = Read_();
         if (text == null)
         {
            host.Visibility = Visibility.Collapsed;
            return;
         }

         var notice = new InlineNotice
         {
            Level = StatusLevel.Information,
            Text = text,
            Content = NoticeActions.OpenPage("updates", automationId),
            Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(notice, automationId + "-notice");
         host.Children.Add(notice);
         host.Visibility = Visibility.Visible;
      }

      private static string Read_()
      {
         Func<string, bool> mayUse = PaletteSearch.MayUse;
         NavNode updates = NavigationMap.Find("updates");
         if (updates == null || (mayUse != null && !mayUse(updates.Needs)))
            return null;

         dynamic app = ServerSession.Current?.Application;
         if (app == null)
            return null;

         try
         {
            dynamic status = app.Status;
            try
            {
               return UpdateStatusText.Waiting((int)status.UpdateState, (string)status.AvailableVersion);
            }
            finally
            {
               ServerSession.Release(status);
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Said by the page that owns the read: Server status under its header,
            // the Updates page in its card.
            return null;
         }
      }
   }
}
