// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What a failed call to the server was, apart from the session that made it.
   ///
   /// A lost link is one event, and the Control Panel shows it at one level: the
   /// shell's pill calls it Critical (ConnectionStatus), so every page that says a
   /// read failed because the link went says so at Critical too, and a page whose
   /// server answered but could not give some of its values says so as a Warning.
   /// Before, the same lost link was a Warning on the setup checklist and on DNS
   /// records, Critical on Setting profiles and a grey caption on the Dashboard.
   ///
   /// WPF-free and free of ServerSession, so that ControlPanel.Tests can hold the
   /// classification and the level to the codes without a server: ServerSession
   /// asks here, and adds the one thing a test cannot have, the repair of its link.
   /// </summary>
   public static class ComErrorText
   {
      /// <summary>
      /// True when the exception means the COM link itself has gone - the
      /// service was restarted, crashed, or the network dropped - rather than
      /// the server rejecting the call.
      /// </summary>
      public static bool IsTransportFailure(Exception ex)
      {
         for (Exception e = ex; e != null; e = e.InnerException)
         {
            if (e is InvalidComObjectException)
               return true;

            if (e is COMException com)
            {
               switch ((uint)com.ErrorCode)
               {
                  case 0x800706BA: // RPC_S_SERVER_UNAVAILABLE
                  case 0x800706BE: // RPC_S_CALL_FAILED
                  case 0x800706BF: // RPC_S_CALL_FAILED_DNE
                  case 0x800706B5: // RPC_S_UNKNOWN_IF
                  case 0x80010108: // RPC_E_DISCONNECTED
                  case 0x80010105: // RPC_E_SERVERFAULT
                  case 0x800401FD: // CO_E_OBJNOTCONNECTED
                  case 0x80080005: // CO_E_SERVER_EXEC_FAILURE
                     return true;
               }
            }
         }

         return false;
      }

      /// <summary>
      /// The level a page shows a failed read at, from its cause: Critical when the
      /// link to the service has gone, as the shell's pill shows it, and Warning when
      /// the server answered and some values could not be read from it.
      /// </summary>
      public static StatusLevel LevelFor(Exception ex) => LevelFor(IsTransportFailure(ex));

      /// <summary>The same, for a page that has counted its failures and remembered whether the link went.</summary>
      public static StatusLevel LevelFor(bool linkLost) => linkLost ? StatusLevel.Critical : StatusLevel.Warning;
   }
}
