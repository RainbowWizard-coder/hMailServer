// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Threading;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   public partial class App : Application
   {
      protected override void OnStartup(StartupEventArgs e)
      {
         base.OnStartup(e);

         // The language first: every caption from here on is looked up in the
         // culture this puts in force, and the first window is created when this
         // method returns.
         LanguageChoice.Apply();

         DispatcherUnhandledException += OnDispatcherUnhandledException;
         AppDomain.CurrentDomain.UnhandledException += OnDomainUnhandledException;

         // Register the semantic colour tokens before any window loads so that
         // every page resolves on-brand, theme-aware state colours.
         Services.ThemeTokens.Initialize();

         // Every page's text and values can be selected and copied (issue #48),
         // every plain list's selected rows, and the captions of check boxes,
         // buttons, drop-downs, tabs and tree items; class handlers, so registered
         // before the first window has any text, list or control.
         SelectableText.Install();
         SelectableList.Install();
         SelectableCaption.Install();

         // The words WPF shows by itself - a text box's Copy, Cut and Paste, a scroll
         // bar's menu - in the language chosen above, where the catalogue has it.
         FrameworkWords.Install();

         // A sign-in handoff an administrator's install of an update left and nobody
         // read in its thirty minutes goes at every start (AfterUpdateHandoff).
         AfterUpdateHandoff.SweepStale();
      }

      protected override void OnExit(ExitEventArgs e)
      {
         // Text copied a moment before the Control Panel was closed still pastes
         // after it has gone: the clipboard is given its own copy now if it was not
         // yet (ClipboardWriter).
         ClipboardWriter.FlushBeforeExit();
         base.OnExit(e);
      }

      private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs args)
      {
         string logPath = LogException(args.Exception);

         string message = F(
            "An unexpected error occurred:\n\n{0}\n\nThe full details were written to:\n{1}\n\nRestart the Control Panel now? Choose No to try to keep working.",
            args.Exception.Message, logPath);

         // Keep the app alive so the user decides; never silently swallow.
         args.Handled = true;

         if (MessageBox.Show(message, L("hMailServer Control Panel"),
                MessageBoxButton.YesNo, MessageBoxImage.Error) == MessageBoxResult.Yes)
         {
            Restart();
         }
      }

      private void OnDomainUnhandledException(object sender, UnhandledExceptionEventArgs args)
      {
         // Non-UI-thread failures can't be recovered from; at least record them.
         if (args.ExceptionObject is Exception ex)
            LogException(ex);
      }

      /// <summary>
      ///    Writes an exception to the Control Panel's own log and returns the path.
      ///
      ///    Internal rather than private because ThemeTokens needs it: it invokes theme
      ///    subscribers one at a time so that a chart card which throws cannot cost the
      ///    others their restyle, and a swallowed exception with nowhere to go is how a
      ///    card silently keeps the wrong palette. This is the only log the Control
      ///    Panel has, so widening it by one assembly is better than a second one.
      /// </summary>
      internal static string LogException(Exception ex)
      {
         try
         {
            string dir = Path.Join(
               Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
               "hMailServer", "ControlPanel");
            Directory.CreateDirectory(dir);

            string path = Path.Join(dir, "control-panel-errors.log");
            File.AppendAllText(path,
               string.Format("[{0:yyyy-MM-dd HH:mm:ss}] {1}{2}{2}", DateTime.Now, ex, Environment.NewLine)); // no-loc
            return path;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return L("(the log file could not be written)");
         }
      }

      /// <summary>
      /// True once the Control Panel has started a copy of itself and is closing
      /// for it - a change of language, or the restart offered after a fault. The
      /// window asks about a page's unsaved changes before it closes, and does not
      /// then: a change of language asked before it got here, and a closing that
      /// Application.Shutdown makes cannot be stopped by a Stay.
      /// </summary>
      internal static bool Restarting { get; private set; }

      internal void Restart()
      {
         Restarting = true;

         try
         {
            string exe = Process.GetCurrentProcess().MainModule?.FileName;
            if (!string.IsNullOrEmpty(exe))
            {
               // Preserve the original command-line arguments (e.g. /connect ...).
               var psi = new ProcessStartInfo(exe);
               string[] args = Environment.GetCommandLineArgs();
               for (int i = 1; i < args.Length; i++)
                  psi.ArgumentList.Add(args[i]);
               Process.Start(psi);
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }

         Shutdown();
      }
   }
}
