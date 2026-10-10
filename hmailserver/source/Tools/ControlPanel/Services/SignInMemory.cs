// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using Microsoft.Win32;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the sign-in page and the window keep of a sign-in, without the window, so
   /// that SignInMemoryTests can hold it (review findings F029 and F033).
   ///
   /// Every launch showed localhost and Administrator, although the window already
   /// kept its bounds, theme, sidebar, help and palette history under HKCU: the
   /// server and the user name of the last sign-in that worked are kept beside them
   /// now, as LastHost and LastUser, and the sign-in page opens with them - never
   /// the password, which is not kept anywhere. And once signed in the window named
   /// its server only in the pill's tool tip, so that two Control Panels open on two
   /// servers looked the same in the taskbar and in Alt+Tab: the window's title
   /// names the server it manages.
   /// </summary>
   public static class SignInMemory
   {
      /// <summary>The window's own key, where its bounds, theme and palette history are.</summary>
      public const string RegistryPath = @"Software\hMailServer\ControlPanel"; // no-loc: a registry key

      /// <summary>The server of the last sign-in that worked.</summary>
      public const string HostValue = "LastHost"; // no-loc: a registry value

      /// <summary>The user name of the last sign-in that worked.</summary>
      public const string UserValue = "LastUser"; // no-loc: a registry value

      /// <summary>What the page opens with where nothing is kept: the server on this computer.</summary>
      public const string DefaultHost = "localhost"; // no-loc: a host name

      /// <summary>
      /// What the page opens with where nothing is kept: the server's administrator,
      /// whose name is Administrator in every language (issue #156).
      /// </summary>
      public const string DefaultUser = "Administrator"; // no-loc: the account's name

      /// <summary>
      /// The server and the user name the sign-in page opens with, from the values
      /// kept in the registry: each as it was kept, or localhost and Administrator
      /// where nothing usable is - no value, an empty one, or one of another type.
      /// </summary>
      public static (string Host, string User) Prefill(object keptHost, object keptUser)
         => (Usable_(keptHost) ?? DefaultHost, Usable_(keptUser) ?? DefaultUser);

      /// <summary>
      /// The server and the user name kept by the last sign-in that worked, or
      /// localhost and Administrator. A registry that cannot be read costs the
      /// prefill and nothing else.
      /// </summary>
      public static (string Host, string User) Read()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            return Prefill(key?.GetValue(HostValue), key?.GetValue(UserValue));
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return Prefill(null, null);
         }
      }

      /// <summary>
      /// Keeps the server and the user name of a sign-in that worked, for the next
      /// launch. There is no password here to keep: the method is given none. Best
      /// effort - a sign-in that worked is not undone because the registry refused.
      /// </summary>
      public static void Remember(string host, string user)
      {
         string keptHost = Usable_(host);
         string keptUser = Usable_(user);
         if (keptHost == null || keptUser == null)
            return;

         try
         {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath);
            key?.SetValue(HostValue, keptHost);
            key?.SetValue(UserValue, keptUser);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: the next launch opens on localhost and Administrator.
         }
      }

      /// <summary>
      /// The window's title: the server it manages first, which is the part that
      /// tells two windows apart in the taskbar, then the product; the product alone
      /// before sign-in.
      /// </summary>
      public static string WindowTitle(string host)
      {
         string server = Usable_(host);
         return server == null ? L("hMailServer Control Panel") : F("{0} - hMailServer Control Panel", server);
      }

      /// <summary>
      /// The level a failed sign-in is said at. A password or a one-time code the
      /// server refused, and a second-factor prompt cancelled, are the reader's to try
      /// again: a Warning. A server that could not be reached or asked - no reply, no
      /// COM registration, a call that failed - is Critical. Every failure was
      /// Critical, so that a typo got the strongest word and colour there are, and
      /// the first word the page's assertive notice said (review finding F029).
      /// </summary>
      public static StatusLevel FailureLevel(bool refused, bool cancelled)
         => refused || cancelled ? StatusLevel.Warning : StatusLevel.Critical;

      private static string Usable_(object value)
         => value is string text && text.Trim().Length > 0 ? text.Trim() : null;
   }
}
