// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    The event-script languages the server under test runs, for a fixture that is
   ///    about one of them - API/EventsJavaScript.cs, whose handlers are JavaScript and
   ///    which the Windows builds refuse (their engines are VBScript and JScript).
   ///
   ///    Asked of the server rather than assumed from the test host: GET /api/v1/status
   ///    reports build.script_languages, and this starts the REST listener on a port of
   ///    its own to read it, once per run, then puts the listener back as it was. The
   ///    Linux project has a file of the same name that asks the same route through the
   ///    listener it already talks to.
   /// </summary>
   public static class ScriptLanguages
   {
      // EventsJavaScript's own port, of the row's 10070-10079.
      private const int ProbePort = 10070;

      private const string AdminPassword = "testar";

      private static List<string> _languages;
      private static bool _listReported;
      private static bool _scriptEngine;

      /// <summary>
      ///    Whether a server that does not run JavaScript fails the JavaScript
      ///    fixture rather than skipping it. Not here: this project is the Windows
      ///    suite, and the Windows builds run VBScript and JScript. The Linux
      ///    project's file of the same name says yes.
      /// </summary>
      public static bool JavaScriptRequired => false;

      /// <summary>Whether GET /api/v1/status named the languages itself, under build.script_languages.</summary>
      public static bool ListReported
      {
         get
         {
            Languages();
            return _listReported;
         }
      }

      /// <summary>GET /api/v1/status's build.script_engine.</summary>
      public static bool ScriptEngine
      {
         get
         {
            Languages();
            return _scriptEngine;
         }
      }

      public static bool ServerRuns(string language)
      {
         foreach (var runs in Languages())
         {
            if (string.Equals(runs, language, StringComparison.OrdinalIgnoreCase))
               return true;
         }

         return false;
      }

      public static string Describe()
      {
         var languages = Languages();
         return languages.Count == 0 ? "none" : string.Join(", ", languages);
      }

      private static List<string> Languages()
      {
         if (_languages != null)
            return _languages;

         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         var languages = new List<string>();

         int port = RestListener.Start(ProbePort);
         application.Reinitialize();

         try
         {
            string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
            (int status, string body) = RestRequest.Send("127.0.0.1", port, "GET", "/api/v1/status", "Basic " + credentials, null);

            if (status == 200)
            {
               Match list = Regex.Match(body, "\"script_languages\"\\s*:\\s*\\[([^\\]]*)\\]");

               _listReported = list.Success;
               _scriptEngine = Regex.IsMatch(body, "\"script_engine\"\\s*:\\s*true");

               if (list.Success)
               {
                  foreach (Match language in Regex.Matches(list.Groups[1].Value, "\"([^\"]+)\""))
                     languages.Add(language.Groups[1].Value);
               }
               else if (Regex.IsMatch(body, "\"script_engine\"\\s*:\\s*true"))
               {
                  // A server from before the list: its engine was Active Scripting.
                  languages.Add("VBScript");
                  languages.Add("JScript");
               }
            }
         }
         finally
         {
            RestListener.Stop();
            application.Reinitialize();
         }

         _languages = languages;
         return _languages;
      }
   }

   /// <summary>
   ///    A backup, started for API/EventsJavaScript.cs's two backup events the way
   ///    this project starts one: through Application.BackupManager over COM. The
   ///    Linux project's file of the same name starts it with POST /api/v1/backup.
   /// </summary>
   public static class EventScriptBackups
   {
      public static void Start(hMailServer.Application application)
      {
         application.BackupManager.StartBackup();
      }
   }
}
