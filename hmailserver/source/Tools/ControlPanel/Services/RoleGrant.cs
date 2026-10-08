// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Which roles hand out the whole server, and the question asked before one is
   /// given.
   ///
   /// The Administrator roles page opened its role box on the first role the server
   /// lists, serveradmin - "Everything, in every domain" - and Give the role handed
   /// it to whatever address had been typed, without a question (review finding
   /// F055). The box opens empty now, and a role that reaches everything is asked
   /// about first: serveradmin by its name, and any role whose permissions, as the
   /// server lists them, include administer_server - the permission the server
   /// reads as every permission there is (ServerSession.May). Today that is
   /// serveradmin alone: the server refuses a custom role that holds
   /// administer_server (RoleManager::SaveRole), so the second test is for a server
   /// that lists one anyway, newer or older than this program. Held by
   /// RoleGrantTests.
   /// </summary>
   public static class RoleGrant
   {
      /// <summary>The built-in role that holds everything (RoleStore on the server).</summary>
      public const string ServerAdmin = "serveradmin"; // no-loc: a role's name

      /// <summary>
      /// Whether giving <paramref name="role"/> gives the whole server: serveradmin,
      /// or a role whose permissions - as the server lists them, separated by commas -
      /// include administer_server.
      /// </summary>
      public static bool ReachesEverything(string role, string permissions)
      {
         if (string.Equals((role ?? "").Trim(), ServerAdmin, StringComparison.OrdinalIgnoreCase))
            return true;

         return (permissions ?? "")
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Any(permission => string.Equals(permission, NavigationMap.ServerAdministration, StringComparison.Ordinal));
      }

      /// <summary>The question asked before such a role is given.</summary>
      public static string Question(string address, string role)
         => F("Give {0} the role {1}, which holds everything, in every domain?", address, role);
   }
}
