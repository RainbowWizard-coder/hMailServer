// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Text;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the About page's Copy version details puts on the clipboard: the lines a
   /// report needs, in English whatever the Control Panel's language, so that
   /// whoever reads the report can read them (review finding F314). The server's
   /// line is there whenever the host is known - the host is the session's own and
   /// is what a report needs first - with the version said not read when the server
   /// did not give it; it was left out, host and all, whenever the version could
   /// not be read.
   /// </summary>
   public static class VersionDetailsReport
   {
      /// <summary>The report, a line each: the Control Panel, the server, the database, .NET and Windows.</summary>
      public static string Text(string controlPanel, string serverVersion, string host, string database, string dotnet, string windows)
      {
         var text = new StringBuilder();
         text.Append("hMailServer Control Panel ").AppendLine(controlPanel ?? "");   // no-loc: a report's lines
         string server = ServerLine(serverVersion, host);
         if (server != null)
            text.AppendLine(server);
         if (!string.IsNullOrEmpty(database))
            text.Append("Database: ").AppendLine(database);   // no-loc
         text.Append(".NET ").AppendLine(dotnet ?? "");   // no-loc
         text.Append("Windows ").AppendLine(windows ?? "");   // no-loc
         return text.ToString();
      }

      /// <summary>
      /// "Server: hMailServer 6.3.6 on mail.example.com", "Server: hMailServer
      /// (version not read) on mail.example.com" when the server did not give its
      /// version, or null when neither is known.
      /// </summary>
      public static string ServerLine(string version, string host)
      {
         bool hasVersion = !string.IsNullOrEmpty(version);
         bool hasHost = !string.IsNullOrEmpty(host);
         if (!hasVersion && !hasHost)
            return null;

         string line = "Server: hMailServer " + (hasVersion ? version : "(version not read)");   // no-loc
         return hasHost ? line + " on " + host : line;   // no-loc
      }
   }
}
