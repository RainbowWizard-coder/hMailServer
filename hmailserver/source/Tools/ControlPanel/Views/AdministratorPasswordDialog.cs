// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Changes the server's administrator password: the password in use now, the
   /// new one, and the new one again.
   ///
   /// It was one masked box on the Administrative access page, written by the page's
   /// Save with the mailbox password policy beside it - no second box, no current
   /// password, nothing said afterwards - so a typo locked the administrator out of
   /// this Control Panel, the REST API and every COM script, with an elevated prompt
   /// on the server the way back in (review finding F168). Each box says on itself
   /// what is wrong with it (Services/AdministratorPasswordChange); the current
   /// password is put to the server before the new one is written
   /// (ServerSession.CheckAdministratorPassword); what the server refuses - the
   /// breach check, a file the service may not write - is said in its words; and a
   /// session signed in as the administrator goes on with the new password.
   /// </summary>
   public class AdministratorPasswordDialog : FluentDialogWindow
   {
      private readonly PasswordField current_ = new();
      private readonly PasswordField new_ = new();
      private readonly PasswordField confirm_ = new();

      private readonly FieldRow currentRow_;
      private readonly FieldRow newRow_;
      private readonly FieldRow confirmRow_;
      private readonly InlineNotice notice_ = DialogFields.Notice();

      public AdministratorPasswordDialog(Window owner)
      {
         Owner = owner;
         Title = L("Set a new administrator password");

         System.Windows.Automation.AutomationProperties.SetAutomationId(current_, "admin-password-current");
         System.Windows.Automation.AutomationProperties.SetAutomationId(new_, "admin-password-new");
         System.Windows.Automation.AutomationProperties.SetAutomationId(confirm_, "admin-password-confirm");

         currentRow_ = Label(L("_Current administrator password"), current_);
         newRow_ = Label(L("_New password"), new_);
         confirmRow_ = Label(L("Con_firm the new password"), confirm_);

         var body = new StackPanel();
         body.Children.Add(notice_);
         body.Children.Add(currentRow_);
         body.Children.Add(newRow_);
         body.Children.Add(confirmRow_);
         confirmRow_.Margin = new Thickness(0);

         // Said on the box as it is typed in rather than only at the end: a
         // confirmation that does not match is the mistake this dialog exists for.
         new_.PasswordChanged += (s, e) => ClearErrors_();
         confirm_.PasswordChanged += (s, e) => ClearErrors_();
         current_.PasswordChanged += (s, e) => currentRow_.Error = null;

         var change = new Wpf.Ui.Controls.Button { Content = L("C_hange the password"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         change.Click += (s, e) => Change_();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         UseFrame(Title, body, change, cancel,
            L("The password this Control Panel, the REST API and every script that connects through the COM API sign in to the server with. Mailbox passwords are not changed."));

         // Room for a box's message: a press refuses one box, in one line.
         KeepRoomForFieldErrors();
      }

      /// <summary>Whether the server has the new password.</summary>
      public bool Changed { get; private set; }

      private static FieldRow Label(string caption, FrameworkElement editor) => DialogFields.Field(caption, editor);

      private void ClearErrors_()
      {
         newRow_.Error = null;
         confirmRow_.Error = null;
      }

      private void Change_()
      {
         notice_.Hide();
         DialogFields.ClearErrors(currentRow_, newRow_, confirmRow_);

         AdministratorPasswordChange.Problem problem = AdministratorPasswordChange.Check(current_.Password, new_.Password, confirm_.Password);
         if (problem != null)
         {
            DialogFields.ShowError(problem.Box switch
            {
               AdministratorPasswordChange.Box.Current => currentRow_,
               AdministratorPasswordChange.Box.New => newRow_,
               _ => confirmRow_
            }, problem.Sentence);
            return;
         }

         ServerSession session = ServerSession.Current;
         if (session == null)
         {
            notice_.Show(StatusLevel.Critical, L("Sign in to a server first."));
            return;
         }

         System.Windows.Input.Mouse.OverrideCursor = System.Windows.Input.Cursors.Wait;
         try
         {
            if (!session.CheckAdministratorPassword(current_.Password, AskForCode_, out bool refused, out string error))
            {
               if (refused)
                  DialogFields.ShowError(currentRow_, AdministratorPasswordChange.WrongCurrent);
               else
                  notice_.Show(StatusLevel.Critical, error);
               return;
            }

            string chosen = new_.Password;
            try
            {
               dynamic settings = session.Application.Settings;
               settings.SetAdministratorPassword(chosen);
               ServerSession.Release((object)settings);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               // The server's own sentence: the breach check's refusal, or the file
               // the service may not write and what to do about it.
               notice_.Show(StatusLevel.Critical, F("The administrator password was not changed: {0}", L(ServerSentences.MessageOf(ex))));
               return;
            }

            session.AdministratorPasswordChanged(chosen);
            Changed = true;
            DialogResult = true;
            Close();
         }
         finally
         {
            System.Windows.Input.Mouse.OverrideCursor = null;
         }
      }

      /// <summary>The administrator's one-time code, for the check of the password in use now; null when none was given.</summary>
      private string AskForCode_()
      {
         var prompt = new TotpPromptDialog(this);
         return prompt.ShowDialog() == true ? prompt.Code : null;
      }
   }
}
