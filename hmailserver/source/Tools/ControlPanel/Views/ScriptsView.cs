// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Editor for the server-side event script file (EventHandlers.vbs / .js).
   /// Loads the file referenced by Scripting.CurrentScriptFile, lets the admin
   /// edit and save it, then reloads the script engine and reports the compile
   /// result via Scripting.CheckSyntax.
   /// </summary>
   public class ScriptsView : UserControl, IPageLifecycle
   {
      private readonly TextBox editor_ = new()
      {
         FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily),
         FontSize = Typography.Body,
         AcceptsReturn = true,
         AcceptsTab = true,
         TextWrapping = TextWrapping.NoWrap,
         HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
         VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
         Background = System.Windows.Media.Brushes.Transparent
      };

      // The card around the editor carries the file's path as its description.
      private readonly Card card_ = new() { Padding = new Thickness(8) };

      // Both under the header, where they are read before the script is: what the
      // last save or check said, and whether the server runs these handlers at
      // all. The result was an Information box at the foot of the page, where an
      // ordinary load said "Loaded from disk.".
      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed, Margin = new Thickness(0, 0, 0, 12) };
      private readonly InlineNotice scriptingOff_ = new()
      {
         Level = StatusLevel.Warning,
         Visibility = Visibility.Collapsed,
         Margin = new Thickness(0, 0, 0, 12)
      };

      // The VBScript templates, offered only while the engine's language is
      // VBScript: they were appended to a JScript file as readily.
      private readonly ComboBox templates_ = new() { MinWidth = 240, VerticalAlignment = VerticalAlignment.Center };

      // Scripting.Enabled and Scripting.Language as last read; on, and VBScript,
      // until they are read - a read that failed is no reason to say the engine
      // is off.
      private bool scriptingOn_ = true;
      private bool vbscript_ = true;

      private string scriptPath_;

      // The script as it was last read from disk or written to it: what an unsaved
      // edit is measured from. Null while there is no script in the editor.
      private string loaded_;

      public ScriptsView() => Build();

      public void OnEnter() => LoadScript();
      public void OnLeave() { }

      /// <summary>
      /// Whether the editor holds something other than the file as it was last read
      /// or written. OnEnter reads the file again, so such an edit is gone when the
      /// page is next opened.
      /// </summary>
      public bool HasUnsavedChanges => editor_.IsEnabled && loaded_ != null && !string.Equals(editor_.Text, loaded_, StringComparison.Ordinal);

      /// <summary>Save &amp; reload; true when the file was written.</summary>
      public bool SaveChanges() => SaveScript();

      /// <summary>Reload from disk: asked about first while the script holds unsaved changes.</summary>
      private void ReloadFromDisk_()
      {
         if (HasUnsavedChanges &&
             !MessageBox.Confirm(UnsavedChanges.ReloadScriptQuestion, L("Control Panel"), L("_Discard"), destructive: true, cancelByDefault: true))
            return;

         LoadScript();
      }

      private void Build()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         // The editor is the only control in the Control Panel with AcceptsTab, so
         // it is the only one where Tab types a character instead of moving on.
         // WPF's own KeyboardNavigation makes Ctrl+Tab the way out, so this is not a
         // keyboard trap - but nothing said so, which for a keyboard-only user is
         // very nearly as bad. The name says it, and so does the tool tip.
         //
         // It also had no accessible name at all: a screen reader announced the
         // server's entire event-handler script as an unnamed "edit".
         System.Windows.Automation.AutomationProperties.SetName(editor_,
            L("Event handler script. Tab inserts a tab character; press Control and Tab together to move to the next control."));
         editor_.ToolTip = L("Tab indents. Use Ctrl+Tab to move focus out of the editor.");

         // A WrapPanel: in German and Japanese the three buttons and the template
         // list that sat here were wider than the header at the smallest window.
         var actions = new WrapPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };

         templates_.Items.Add(new ComboBoxItem { Content = L("Insert template\u2026"), Tag = "" });
         foreach ((string name, string body) in ScriptTemplates)
            templates_.Items.Add(new ComboBoxItem { Content = name, Tag = body });
         templates_.SelectedIndex = 0;
         System.Windows.Automation.AutomationProperties.SetName(templates_, L("Insert template\u2026"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(templates_, "scripts-templates");
         templates_.SelectionChanged += (s, e) =>
         {
            if (templates_.SelectedItem is ComboBoxItem cbi && cbi.Tag is string body && body.Length > 0)
            {
               InsertTemplate(body);
               templates_.SelectedIndex = 0;
            }
         };

         // Ctrl+S and F5 press these two, whose captions say more than the
         // pages' Save changes and Reload (Views/PageCommands).
         var save = MakeButton(L("_Save & reload"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => SaveScript());
         PageCommands.SetCommand(save, PageCommand.Save);
         actions.Children.Add(save);
         actions.Children.Add(MakeButton(L("_Check syntax"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => CheckSyntax()));
         var reload = MakeButton(L("_Reload from disk"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => ReloadFromDisk_());
         PageCommands.SetCommand(reload, PageCommand.Refresh);
         actions.Children.Add(reload);

         root.Children.Add(new PageHeader
         {
            Title = L("Event scripts"),
            Subtitle = L("Edit the server event-handler script (OnAcceptMessage, OnDeliveryStart, OnHELO, ...). Saving writes the file and reloads the scripting engine. The engine's on/off switch and its language are on the Advanced page."),
            Actions = actions
         });

         // The notices under the header, the card under them.
         var notices = new StackPanel();
         System.Windows.Automation.AutomationProperties.SetAutomationId(scriptingOff_, "scripts-scripting-off");
         scriptingOff_.Content = NoticeActions.OpenPage("advanced", "scripts-open-advanced");
         notices.Children.Add(scriptingOff_);
         notices.Children.Add(status_);
         Grid.SetRow(notices, 1);
         root.Children.Add(notices);

         // The template list is the editor's own tool, so it is on the editor's
         // card, above the text it inserts into.
         editor_.SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
         var editorArea = new Grid();
         editorArea.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         editorArea.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
         templates_.Margin = new Thickness(0, 0, 0, 8);
         templates_.HorizontalAlignment = HorizontalAlignment.Left;
         editorArea.Children.Add(templates_);
         Grid.SetRow(editor_, 1);
         editorArea.Children.Add(editor_);
         card_.Content = editorArea;
         Grid.SetRow(card_, 2);
         root.Children.Add(card_);

         Content = root;
      }

      private static Wpf.Ui.Controls.Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var b = new Wpf.Ui.Controls.Button { Content = text, Appearance = appearance, Margin = new Thickness(0, 0, 8, 4), MinWidth = 110 };
         b.Click += onClick;
         return b;
      }

      /// <summary>What the last load, save or check said, at its level.</summary>
      private void Status_(StatusLevel level, string text)
      {
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      // VBScript starter snippets for common integrations. Each is a complete
      // OnAcceptMessage handler; if the script already has one, the two must be
      // merged into a single Sub before saving.
      private static readonly (string Name, string Body)[] ScriptTemplates =
      {
         (L("OnAcceptMessage \u2192 external AV / DLP scanner"),
@"' --- Run an external AV / DLP scanner on the received message ----------------
' The message is already on disk at oMessage.Filename. A non-zero exit code is
' treated as ""infected/blocked"". Adjust the path and exit-code handling.
Sub OnAcceptMessage(oClient, oMessage)
   Dim oShell, sCmd, nResult
   sCmd = ""C:\Tools\scan.exe "" & Chr(34) & oMessage.Filename & Chr(34)
   Set oShell = CreateObject(""WScript.Shell"")
   nResult = oShell.Run(sCmd, 0, True)   ' 0 = hidden window, True = wait for exit
   If nResult <> 0 Then
      Result.Value = 2                   ' 0 = accept, 1 = move to queue, 2 = delete
      Result.Message = ""Rejected by external scanner (exit "" & nResult & "")""
   End If
End Sub
"),
         (L("OnAcceptMessage \u2192 webhook (SIEM / Slack / Teams)"),
@"' --- Notify a webhook about the received message ----------------------------
' Fire-and-forget POST; delivery is not blocked if the webhook is unavailable.
Sub OnAcceptMessage(oClient, oMessage)
   Dim oHttp, q, sUrl, sBody
   q = Chr(34)
   sUrl = ""https://example.com/webhook""
   sBody = ""{"" & q & ""from"" & q & "":"" & q & oMessage.FromAddress & q & ""}""
   On Error Resume Next
   Set oHttp = CreateObject(""MSXML2.ServerXMLHTTP.6.0"")
   oHttp.Open ""POST"", sUrl, False
   oHttp.setRequestHeader ""Content-Type"", ""application/json""
   oHttp.send sBody
   EventLog.Write ""Webhook notified, status "" & oHttp.Status
   On Error Goto 0
End Sub
"),
         (L("OnAcceptMessage \u2192 external HTTP API verdict"),
@"' --- Ask an external API whether to block the message -----------------------
' Calls a classification/threat API and deletes the message on a 'block' verdict.
Sub OnAcceptMessage(oClient, oMessage)
   Dim oHttp, sUrl
   sUrl = ""https://api.example.com/classify?from="" & oMessage.FromAddress
   Set oHttp = CreateObject(""MSXML2.ServerXMLHTTP.6.0"")
   oHttp.Open ""GET"", sUrl, False
   oHttp.send
   If oHttp.Status = 200 And InStr(oHttp.responseText, ""block"") > 0 Then
      Result.Value = 2
      Result.Message = ""Blocked by classification API""
   End If
End Sub
"),
      };

      private void InsertTemplate(string body)
      {
         if (!editor_.IsEnabled || !vbscript_)
            return;

         string separator = editor_.Text.Length > 0 ? "\r\n\r\n" : "";
         editor_.Text = editor_.Text + separator + body;
         editor_.CaretIndex = editor_.Text.Length;
         editor_.ScrollToEnd();
         Status_(StatusLevel.Information, L("Template appended. If you already have an OnAcceptMessage handler, merge the two into a single Sub before saving."));
      }

      /// <summary>
      /// Whether the server runs these handlers at all, and in which language:
      /// Scripting.Enabled and Scripting.Language, which the Advanced page sets.
      /// The page never said, so a script saved with the engine off was reported
      /// "Saved and reloaded" and ran nowhere. Said as a Warning under the header
      /// with the page that switches it on; and the VBScript templates are
      /// offered only while the language is VBScript.
      /// </summary>
      private void ReadScriptingState_()
      {
         dynamic scripting = null;
         try
         {
            scripting = ServerSession.Current.Application.Settings.Scripting;
            scriptingOn_ = (bool)scripting.Enabled;
            string language = ((string)scripting.Language ?? "").Trim();
            vbscript_ = language.Length == 0 || string.Equals(language, "VBScript", StringComparison.OrdinalIgnoreCase); // no-loc: the engine's name
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Not known: neither "off" nor "JScript" is said of a server that did
            // not answer.
            scriptingOn_ = true;
            vbscript_ = true;
         }
         finally
         {
            ServerSession.Release((object)scripting);
         }

         scriptingOff_.Text = scriptingOn_ ? null : L("Scripting is off - these handlers are not run. The engine's switch is on the Advanced page.");
         scriptingOff_.Visibility = scriptingOn_ ? Visibility.Collapsed : Visibility.Visible;
         templates_.Visibility = vbscript_ ? Visibility.Visible : Visibility.Collapsed;
      }

      private void LoadScript()
      {
         loaded_ = null;
         status_.Visibility = Visibility.Collapsed;
         ReadScriptingState_();
         scriptPath_ = ResolveScriptPath();
         if (scriptPath_ == null)
         {
            card_.Description = null;
            Status_(StatusLevel.Critical, L("Could not determine the event-script path."));
            editor_.IsEnabled = false;
            return;
         }

         card_.Description = scriptPath_;
         try
         {
            editor_.Text = File.Exists(scriptPath_) ? File.ReadAllText(scriptPath_) : "";

            // An ordinary load says nothing: the script is on screen.
            if (!File.Exists(scriptPath_))
               Status_(StatusLevel.Information, L("File does not exist yet; saving will create it."));
            editor_.IsEnabled = true;
            loaded_ = editor_.Text;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Keep the editor disabled: enabling it after a failed load would let a
            // save overwrite the file on disk with the empty text shown here.
            editor_.Text = "";
            editor_.IsEnabled = false;
            Status_(StatusLevel.Critical, F("Could not read the file: {0}", ex.Message));
         }
      }

      private string ResolveScriptPath()
      {
         dynamic scripting = ServerSession.Current.Application.Settings.Scripting;
         try
         {
            string file = (string)scripting.CurrentScriptFile;
            string dir = (string)scripting.Directory;
            if (!string.IsNullOrWhiteSpace(file) && Path.IsPathRooted(file))
               return file;
            if (!string.IsNullOrWhiteSpace(dir) && !string.IsNullOrWhiteSpace(file))
               return Path.Join(dir, file);
            if (!string.IsNullOrWhiteSpace(dir))
               return Path.Join(dir, "EventHandlers.vbs");
            return null;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
         finally
         {
            ServerSession.Release(scripting);
         }
      }

      /// <summary>Writes the file and reloads the scripting engine; true when the file was written.</summary>
      private bool SaveScript()
      {
         if (scriptPath_ == null || !editor_.IsEnabled)
            return false;
         try
         {
            // Keep the previous version next to the file: event handlers are
            // hand-written administrator code with no other undo.
            if (File.Exists(scriptPath_))
               File.Copy(scriptPath_, scriptPath_ + ".bak", true);

            File.WriteAllText(scriptPath_, editor_.Text);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not write the script file: {0}", ex.Message), L("Control Panel"));
            return false;
         }

         // On disk now, whatever the engine makes of it below.
         loaded_ = editor_.Text;

         dynamic scripting = ServerSession.Current.Application.Settings.Scripting;
         try
         {
            scripting.Reload();
            string result = "";
            try { result = (string)scripting.CheckSyntax(); } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
            // With the engine off the script was saved and checked, and runs
            // nowhere: "reloaded" would say otherwise.
            if (!string.IsNullOrWhiteSpace(result))
               Status_(StatusLevel.Warning, F("Saved. Compiler reported: {0}", result));
            else if (scriptingOn_)
               Status_(StatusLevel.Good, F("Saved and reloaded at {0}. No syntax errors.", DateTime.Now.ToLongTimeString()));
            else
               Status_(StatusLevel.Information, F("Saved at {0}. No syntax errors. Scripting is off, so the server does not run these handlers.", DateTime.Now.ToLongTimeString()));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, F("Saved, but reload failed: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(scripting);
         }

         return true;
      }

      private void CheckSyntax()
      {
         // The server checks the script file it has, not this editor: after an
         // edit, "No syntax errors reported." was said of the old code. Save &
         // reload checks what is on screen, and says so as it saves.
         if (HasUnsavedChanges)
         {
            Status_(StatusLevel.Information, L("Check syntax checks the script saved on the server, and the editor holds changes that are not saved yet. Save & reload to check what is on screen."));
            return;
         }

         dynamic scripting = ServerSession.Current.Application.Settings.Scripting;
         try
         {
            string result = (string)scripting.CheckSyntax();
            if (string.IsNullOrWhiteSpace(result))
               Status_(StatusLevel.Good, L("No syntax errors reported."));
            else
               Status_(StatusLevel.Warning, F("Compiler reported: {0}", result));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, F("Syntax check failed: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(scripting);
         }
      }
   }
}
