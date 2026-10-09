// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

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

      /// <summary>
      /// The sentence for a failure that is not the server's own words, or null for
      /// one that is - a refusal, whose message is the sentence. Two kinds: a member
      /// the server does not have, which is an older server than this Control
      /// Panel, and an answer this Control Panel could not read - a number that was
      /// not one, a document that was not JSON, a value of another type or none.
      /// Shown bare, each was the runtime's text, "The input string 'x' was not in
      /// a correct format." in bold on the Levels tab and a JSON parser's position
      /// as a statistic; that text is kept, in brackets, for whoever has to find
      /// out more (<paramref name="detail"/>, the innermost message).
      /// </summary>
      public static string NotTheServersWords(Exception ex, string detail)
      {
         if (ex == null)
            return null;

         if (IsMissingMember(ex))
            return F("the server has no such setting or action - it may be older than this Control Panel ({0})", detail ?? "");

         if (IsUnreadableAnswer(ex))
            return F("the server answered something this Control Panel could not read ({0})", detail ?? "");

         return null;
      }

      /// <summary>
      /// A member the server does not have: IDispatch's own codes for a name it does
      /// not know (DISP_E_UNKNOWNNAME) or an accessor it lacks (DISP_E_MEMBERNOTFOUND),
      /// reflection's missing member, or the binder saying there is no definition.
      /// </summary>
      public static bool IsMissingMember(Exception ex)
      {
         const int UnknownName = unchecked((int)0x80020006);     // DISP_E_UNKNOWNNAME
         const int MemberNotFound = unchecked((int)0x80020003);  // DISP_E_MEMBERNOTFOUND

         for (Exception e = ex; e != null; e = e.InnerException)
         {
            if (e is COMException com && (com.HResult == UnknownName || com.HResult == MemberNotFound))
               return true;
            if (e is MissingMemberException)
               return true;
            if (IsBinderFailure(e) && e.Message.Contains("does not contain a definition for", StringComparison.Ordinal)) // no-loc: the binder's own words
               return true;
         }

         return false;
      }

      /// <summary>
      /// An answer this Control Panel could not read: a number that was not one or
      /// did not fit, a value of another type than the one asked for or none at all
      /// (the binder's other failures), a key missing from a document, or a document
      /// that was not JSON - including the JSON reader's own InvalidOperationException
      /// for a value of the wrong kind.
      /// </summary>
      public static bool IsUnreadableAnswer(Exception ex)
      {
         for (Exception e = ex; e != null; e = e.InnerException)
         {
            if (e is FormatException || e is OverflowException || e is InvalidCastException
                || e is KeyNotFoundException || e is JsonException)
               return true;
            if (IsBinderFailure(e))
               return true;
            if (e is InvalidOperationException && (e.TargetSite?.DeclaringType?.Namespace ?? "").StartsWith("System.Text.Json", StringComparison.Ordinal)) // no-loc
               return true;
         }

         return false;
      }

      /// <summary>
      /// The runtime binder's exception, by name: the C# binder's assembly is not one
      /// this file should bind to for a type test.
      /// </summary>
      private static bool IsBinderFailure(Exception e)
         => e.GetType().FullName == "Microsoft.CSharp.RuntimeBinder.RuntimeBinderException"; // no-loc

      /// <summary>
      /// The exception as the runtime reports it - its type, its HRESULT and its
      /// message, innermost first - for a tool tip beside a sentence that says what
      /// it means, so that whoever has to look further has the code.
      /// </summary>
      public static string TechnicalDetail(Exception ex)
      {
         if (ex == null)
            return null;

         Exception real = ex;
         while (real.InnerException != null)
            real = real.InnerException;

         return string.Format(CultureInfo.InvariantCulture, "{0} 0x{1:X8}: {2}", real.GetType().Name, real.HResult, real.Message); // no-loc
      }
   }
}
