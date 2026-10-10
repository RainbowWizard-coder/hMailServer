// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using Microsoft.Win32;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// "hMailServer 6.3.7 is available." in one line with Open Updates, on Server
   /// status, the Dashboard and About, while a newer release is waiting - and
   /// nothing while none is. The release is checked for, downloaded and installed
   /// on the Updates page (FeatureSettingsView.Updates.cs); About showed the
   /// version and not that a newer one was waiting (review finding F272).
   ///
   /// The same line says, at Warning, an update step that failed and the last
   /// apply's outcome, which Server status's Update status row said before the
   /// steps moved: a rolled-back unattended apply was otherwise on a Maintenance
   /// page nobody had reason to open. The outcome is said until the Updates page
   /// has shown it to this reader, in its Update status row - the line leads there,
   /// and the service keeps the outcome until it stops, so the line would otherwise
   /// stay for months (<see cref="UpdateStatusText.WaitingOrFailed"/>).
   ///
   /// Read once as the page opens, not with the Dashboard's two-second refresh:
   /// the check runs once a day. Shown only to a credential that may open the
   /// Updates page, and a read that fails says nothing here - the page that owns
   /// the read says it.
   /// </summary>
   internal static class UpdateNotice
   {
      private const string RegistryPath = @"Software\hMailServer\ControlPanel"; // no-loc: a registry key
      private const string SeenValue = "UpdateOutcomeSeen"; // no-loc: a registry value

      /// <summary>Puts the notice in <paramref name="host"/>, or empties and hides it.</summary>
      public static void ShowIn(Panel host, string automationId)
      {
         if (host == null)
            return;

         host.Children.Clear();
         (StatusLevel level, string text) = Read_();
         if (text == null)
         {
            host.Visibility = Visibility.Collapsed;
            return;
         }

         var notice = new InlineNotice
         {
            Level = level,
            Text = text,
            Content = NoticeActions.OpenPage("updates", automationId),
            Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(notice, automationId + "-notice");
         host.Children.Add(notice);
         host.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// The Updates page has shown <paramref name="outcome"/>, the last apply's, to this
      /// reader: the line no longer says it. Kept per Windows user, and per server, beside
      /// the Control Panel's other preferences; nothing for an empty outcome.
      /// </summary>
      public static void MarkSeen(string outcome)
      {
         if (string.IsNullOrWhiteSpace(outcome))
            return;

         try
         {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath);
            key?.SetValue(SeenValue, Host_() + "\n" + outcome.Trim());
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only - the line then says the outcome again.
         }
      }

      // The outcome the Updates page last showed this reader, for the server signed in to.
      private static string SeenOutcome_()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            string value = key?.GetValue(SeenValue) as string;
            int split = value?.IndexOf('\n') ?? -1;
            return split >= 0 && string.Equals(value.Substring(0, split), Host_(), StringComparison.OrdinalIgnoreCase) ? value.Substring(split + 1) : null;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
      }

      private static string Host_() => ServerSession.Current?.Host ?? "";

      private static (StatusLevel Level, string Text) Read_()
      {
         Func<string, bool> mayUse = PaletteSearch.MayUse;
         NavNode updates = NavigationMap.Find("updates");
         if (updates == null || (mayUse != null && !mayUse(updates.Needs)))
            return (StatusLevel.Normal, null);

         dynamic app = ServerSession.Current?.Application;
         if (app == null)
            return (StatusLevel.Normal, null);

         try
         {
            dynamic status = app.Status;
            try
            {
               int state = (int)status.UpdateState;
               string error = state == 5 ? (string)status.UpdateLastError : null;
               return UpdateStatusText.WaitingOrFailed(state, (string)status.AvailableVersion, error, ApplyOutcome_(status), SeenOutcome_());
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
            return (StatusLevel.Normal, null);
         }
      }

      // An older server has no apply's outcome to say: the line says the rest.
      private static string ApplyOutcome_(dynamic status)
      {
         try
         {
            return (string)status.UpdateApplyOutcome;
         }
         catch (Exception older) when (!ExceptionPolicy.IsFatal(older))
         {
            return null;
         }
      }
   }
}
