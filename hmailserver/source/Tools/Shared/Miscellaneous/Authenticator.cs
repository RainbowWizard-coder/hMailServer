// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows.Forms;
using System.Linq;

namespace hMailServer.Shared
{
   public static class Authenticator
   {
      // The environment variable the installer hands the administrator password in,
      // as it does to hMailServer.exe --set-admin-password, to DBSetupQuick and to
      // DBSetup, and that DBSetupQuick hands it on to DBUpdater in. Never a command
      // line: anybody listing processes reads one, and one is split again by the C
      // runtime's rules on the way in, so a space, a double quote or backslashes
      // before one changed the password.
      public const string PasswordVariable = "HM_ADMIN_PASSWORD";

      // The administrator password this process was given: password:VALUE on its
      // command line - how an administrator running a tool by hand gives it - or
      // else HM_ADMIN_PASSWORD, how the installer gives it, the live update's token
      // ("token:<hex>") included. Null when neither is there.
      public static string GetGivenPassword()
      {
         if (CommandLineParser.ContainsArgument("password"))
            return CommandLineParser.GetArgument("password");

         string fromEnvironment = Environment.GetEnvironmentVariable(PasswordVariable);
         return string.IsNullOrEmpty(fromEnvironment) ? null : fromEnvironment;
      }

      public static bool AuthenticateUser(hMailServer.Application application, string password)
      {
         hMailServer.Account account = application.Authenticate("Administrator", password);

         if (account != null)
            return true;

         return false;
      }

      public static bool AuthenticateUser(hMailServer.Application application)
      {
         // First try to authenticate using an empty password.
         if (AuthenticateUser(application, ""))
            return true;

         // Then the password this process was given, if it was given one: in
         // HM_ADMIN_PASSWORD, from the installer - to DBSetup directly, or through
         // DBSetupQuick to DBUpdater, the path an unattended UPGRADE takes - or as
         // "password:VALUE" by an administrator.
         // The raw argument sweep below cannot serve either: the raw argument is
         // the whole "password:VALUE" string, never the value on its own.
         string given = GetGivenPassword();
         if (given != null && AuthenticateUser(application, given))
            return true;

         // Try to authenticate using password on command line...
         string[] args = Environment.GetCommandLineArgs();
         if (args.Any(password => AuthenticateUser(application, password)))
            return true;

         // Under /silent there is nobody at the keyboard, so a modal dialog is not a
         // prompt - it is a hang, and it hangs the installer waiting on this process
         // rather than this process alone. Failing here lets the caller return an exit
         // code the installer reports out loud. A visible error beats an invisible wait.
         if (CommandLineParser.IsSilent())
            return false;

         while (true)
         {
            formEnterPassword passwordDlg = new formEnterPassword();

            if (passwordDlg.ShowDialog() != System.Windows.Forms.DialogResult.OK)
               return false;

            string password = passwordDlg.Password;

            if (AuthenticateUser(application, password))
               return true;

            MessageBox.Show("Invalid user name or password.", "hMailServer");
         }
      }
   }
}
