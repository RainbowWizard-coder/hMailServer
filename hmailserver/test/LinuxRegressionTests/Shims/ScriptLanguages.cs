// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace RegressionTests.Shared
{
   /// <summary>
   ///    The event-script languages the server under test runs, for a fixture that is
   ///    about one of them - API/EventsJavaScript.cs. Here it is what GET
   ///    /api/v1/status says under build.script_languages; the Windows project has a
   ///    file of the same name that asks the same route over its own listener.
   /// </summary>
   public static class ScriptLanguages
   {
      /// <summary>
      ///    Whether a server that does not run JavaScript fails the JavaScript
      ///    fixture rather than skipping it. Yes, here: this project is the Linux
      ///    builds' suite, and every Linux build since the row "Event scripts on
      ///    Linux" runs JavaScript. One that does not report it has lost its engine,
      ///    or its status field, and forty tests skipping on that would leave the job
      ///    green over a regression.
      /// </summary>
      public static bool JavaScriptRequired => true;

      /// <summary>Whether GET /api/v1/status named the languages itself, under build.script_languages.</summary>
      public static bool ListReported => ServerApi.ReportsScriptLanguages;

      /// <summary>GET /api/v1/status's build.script_engine.</summary>
      public static bool ScriptEngine => ServerApi.Capability("script_engine");

      public static bool ServerRuns(string language)
      {
         return ServerApi.RunsScriptLanguage(language);
      }

      public static string Describe()
      {
         var languages = ServerApi.ScriptLanguages;
         return languages.Count == 0 ? "none" : string.Join(", ", languages);
      }
   }

   /// <summary>
   ///    A backup, started for API/EventsJavaScript.cs's two backup events: POST
   ///    /api/v1/backup, the route the backup run is on this server. The Windows
   ///    project's file of the same name starts it through COM.
   ///
   ///    Not the Application shim's BackupManager, which still skips: made real, it
   ///    would start seven tests of ScheduledBackup and BackupVerifiedRestore on this
   ///    platform - compressed archives written with the system's 7-Zip, read back and
   ///    reconciled - which is a change to those fixtures and not this one's to make.
   ///    The two events need only the smallest backup and one that fails before it
   ///    starts.
   /// </summary>
   public static class EventScriptBackups
   {
      public static void Start(hMailServer.Application application)
      {
         ServerApi.Post("/api/v1/backup", "{}").Expect(202, "POST /api/v1/backup");
      }
   }
}
