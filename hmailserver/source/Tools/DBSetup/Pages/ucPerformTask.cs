// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using hMailServer.Shared;

namespace DBSetup.Pages
{
   public partial class ucPerformTask : UserControl, IWizardPage
   {
      private Dictionary<string, string> _state;

      public ucPerformTask()
      {
         InitializeComponent();
      }

      public string Title
      {
         get
         {
            return "Finish";
         }
      }

      public void OnShowPage(Dictionary<string, string> state)
      {
         textLog.Text = "";

         _state = state;
      }

      private void AddToLog(string text)
      {
         textLog.AppendText(text + Environment.NewLine);
         textLog.SelectionStart = textLog.Text.Length;
         textLog.ScrollToCaret();
         textLog.Refresh();

         Application.DoEvents();
      }

      public bool OnLeavePage(bool next)
      {
         if (!next)
            return true;
         try
         {
            AddToLog("Starting task...");


            // Perform the task...
            hMailServer.eDBtype dbType = Globals.GetDatabaseType(_state["ServerType"]);
            string serverName = _state["ServerAddress"];
            string portString = _state["ServerPort"];
            int port = 0;
            int.TryParse(portString, out port);
            string databaseName = _state["DatabaseName"];

            string userName = _state["Username"];
            string passWord = _state["Password"];

            string serviceDependency = _state["ServiceDependency"];
            SetDependency(serviceDependency);

            if (_state["Authentication"] == "Windows")
            {
               userName = "";
               passWord = "";
            }

            hMailServer.Database database = Globals.GetApp().Database;

            if (_state["CreateNew"] == "Yes")
            {
               AddToLog("Please wait while creating database...");
               database.CreateExternalDatabase(dbType, serverName, port, databaseName, userName, passWord);
               AddToLog("Database created.");
            }
            else
            {
               AddToLog("Please wait while updating database settings...");
               database.SetDefaultDatabase(dbType, serverName, port, databaseName, userName, passWord);
               AddToLog("Settings updated.");
            }

            AddToLog("Restarting server...");
            Globals.GetApp().Reinitialize();
            AddToLog("Server restarted.");
            AddToLog("");
            AddToLog("Task completed.");


         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            AddToLog(ex.Message);
            return false;
         }

         return true;
      }

      private void SetDependency(string service)
      {
         try
         {
            if (service.Length == 0)
               return;

            int paranthesisStart = service.LastIndexOf("(") + 1;
            int paranthesisEnd = service.IndexOf(")", paranthesisStart);
            int len = paranthesisEnd - paranthesisStart;

            string serviceName = service.Substring(paranthesisStart, len);

            try
            {
               Globals.GetApp().Utilities.MakeDependent(serviceName);
            }
            catch (Exception serverRefused) when (!ExceptionPolicy.IsFatal(serverRefused))
            {
               // The server changes its own service entry only while it runs as
               // LocalSystem - the installer sets the database up then. Under its
               // own account it may not, by design (docs/ServiceAccount.md), so
               // the entry is changed from here instead, by sc.exe, as an
               // administrator: this program already is one when the installer
               // runs it, and Windows asks otherwise.
               string why = SetDependencyAsAdministrator(serviceName);
               if (why != null)
                  MessageBox.Show("The set-up failed to set the service dependency: " + serverRefused.Message + "\r\n\r\n" + why, "hMailServer");
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            MessageBox.Show("The set-up failed to set the service dependency: " + fatalCheck.Message, "hMailServer");
         }

      }

      /// <summary>
      /// sc.exe config hMailServer depend= RPCSS/name, run as an administrator:
      /// the same two dependencies ServiceManager::MakeDependentOn sets. Null when
      /// it was set, else why not.
      /// </summary>
      private static string SetDependencyAsAdministrator(string serviceName)
      {
         string arguments = "config hMailServer depend= \"RPCSS/" + serviceName + "\"";
         try
         {
            var start = new System.Diagnostics.ProcessStartInfo
            {
               FileName = System.IO.Path.Combine(Environment.SystemDirectory, "sc.exe"),
               Arguments = arguments,
               UseShellExecute = true,
               Verb = "runas",
               WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden
            };

            using (var process = System.Diagnostics.Process.Start(start))
            {
               if (process == null)
                  return "sc.exe could not be started. From an elevated prompt: sc.exe " + arguments;
               process.WaitForExit();
               return process.ExitCode == 0 ? null : "sc.exe " + arguments + " returned " + process.ExitCode + "; run it from an elevated prompt.";
            }
         }
         catch (Win32Exception cancelled) when (cancelled.NativeErrorCode == 1223)
         {
            return "The elevation prompt was cancelled. From an elevated prompt: sc.exe " + arguments;
         }
         catch (Exception other) when (!ExceptionPolicy.IsFatal(other))
         {
            return other.Message + " From an elevated prompt: sc.exe " + arguments;
         }
      }
   }
}
