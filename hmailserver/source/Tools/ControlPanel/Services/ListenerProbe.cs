// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>What the machine says about one configured port.</summary>
   public enum ListenerState
   {
      /// <summary>Something is listening on that address and port.</summary>
      Listening,

      /// <summary>Nothing is listening, and the protocol is switched on - so it should be.</summary>
      NotListening,

      /// <summary>Nothing is listening because the protocol server is switched off.</summary>
      ProtocolDisabled,

      /// <summary>The question cannot be answered from here.</summary>
      Unknown
   }

   /// <summary>
   /// Whether the ports configured in hMailServer are actually being listened on.
   ///
   /// A TCP/IP port row is a request, not a state. TCPServer::InitAcceptor can fail
   /// to bind - another program already has the port, or the address does not exist
   /// on this machine - and when it does the server reports error 4316 to a log
   /// nobody has open and carries on running every other listener. The port page
   /// went on showing the row exactly as before. The commonest form of "mail stopped
   /// arriving and nothing looks wrong" is a port that never came up.
   ///
   /// TLS ports have two more ways to fail before the bind is even attempted, both
   /// in TCPServer::Run: an SSL context that will not initialise (a missing,
   /// unreadable or passphrase-protected certificate) and a client-certificate
   /// policy set to "require" on a port with no TLS. Neither starts the listener,
   /// and neither changes anything this page could previously show.
   ///
   /// The check is the operating system's own list of listening endpoints, so it
   /// answers "is this port open" and not "is it open because of hMailServer" - a
   /// port held by a different program looks the same from here. That distinction
   /// is stated where the result is shown rather than papered over, because the
   /// other program IS the usual cause when a configured port is not hMailServer's.
   /// </summary>
   public static class ListenerProbe
   {
      /// <summary>
      /// Every TCP endpoint this machine is listening on, or an empty list when the
      /// question cannot be answered. Never throws: it is called while a page is
      /// being built, and a machine with an unusual network stack must degrade to
      /// "cannot tell" rather than take the page down.
      /// </summary>
      public static IReadOnlyList<IPEndPoint> ActiveListeners()
      {
         try
         {
            return IPGlobalProperties.GetIPGlobalProperties().GetActiveTcpListeners();
         }
         catch (NetworkInformationException)
         {
            return Array.Empty<IPEndPoint>();
         }
         catch (PlatformNotSupportedException)
         {
            return Array.Empty<IPEndPoint>();
         }
      }

      /// <summary>
      /// True when one of <paramref name="listeners"/> covers the configured
      /// address and port.
      ///
      /// Pure, so the matching rules can be tested without a network stack. Two
      /// things make it more than string equality:
      ///
      ///  - hMailServer stores a wildcard as "0.0.0.0" (IPAddress default-
      ///    constructs to IPv4 any, and TCPIPPort::GetAddressString returns its
      ///    ToString), so a wildcard row must match a listener on any address.
      ///  - A listener on the wildcard covers every address of its own family, so
      ///    a row naming 192.168.1.10 is served by a listener on 0.0.0.0.
      ///
      /// Families are kept apart deliberately. A socket bound to :: covers IPv4 too
      /// when dual-stack is on and does not when it is off, and this cannot tell
      /// which - so it does not claim either. The cost is a v4 row reported as not
      /// listening on a dual-stack-only server; the alternative is reporting a port
      /// as up when nothing answers on it, which is the worse error here.
      /// </summary>
      public static bool IsBound(string configuredAddress, int port, IReadOnlyList<IPEndPoint> listeners)
      {
         if (listeners == null || port <= 0)
            return false;

         string address = (configuredAddress ?? "").Trim();

         bool wildcardRow = address.Length == 0
                            || address == "0.0.0.0"
                            || address == "::"
                            || address == "[::]";

         IPAddress configured = null;
         if (!wildcardRow && !IPAddress.TryParse(address, out configured))
         {
            // A row whose address does not parse cannot be matched to anything, and
            // saying "not listening" would be a claim about a configuration that is
            // already broken in a different way.
            return false;
         }

         foreach (IPEndPoint listener in listeners.Where(l => l != null && l.Port == port))
         {
            // A wildcard row is served by a wildcard listener, and Windows reports a
            // successful wildcard bind literally as 0.0.0.0 or [::] - so matching
            // any address on the port would be looser than the question asked.
            // Narrowed after an adversarial check of this file: the loose form could
            // read green when hMailServer's own wildcard listener was down (a TLS
            // context failure, the service stopped) while some other program held
            // that port on one specific address. That conjunction is narrow, but the
            // tighter test costs nothing and cannot be wrong in that direction.
            if (wildcardRow)
            {
               if (listener.Address.Equals(IPAddress.Any) || listener.Address.Equals(IPAddress.IPv6Any))
                  return true;

               continue;
            }

            if (listener.Address.Equals(configured))
               return true;

            // The wildcard of the configured address's own family covers it.
            if (configured.AddressFamily == AddressFamily.InterNetwork &&
                listener.Address.Equals(IPAddress.Any))
            {
               return true;
            }

            if (configured.AddressFamily == AddressFamily.InterNetworkV6 &&
                listener.Address.Equals(IPAddress.IPv6Any))
            {
               return true;
            }
         }

         return false;
      }

      /// <summary>
      /// The state of one configured port, given whether its protocol server is
      /// switched on and whether the Control Panel is looking at this machine.
      /// </summary>
      /// <param name="protocolEnabled">
      /// ServiceSMTP / ServiceIMAP / ServicePOP3 for this row's protocol, or null
      /// when that could not be read - in which case a port that is not bound is
      /// reported as unknown rather than as broken.
      /// </param>
      public static ListenerState StateOf(string configuredAddress, int port, bool? protocolEnabled,
         IReadOnlyList<IPEndPoint> listeners, bool localServer)
      {
         if (!localServer)
            return ListenerState.Unknown;

         if (IsBound(configuredAddress, port, listeners))
            return ListenerState.Listening;

         // Order matters: a disabled protocol explains an unbound port completely,
         // and reporting it as a fault would send an administrator hunting for a
         // port conflict that does not exist.
         if (protocolEnabled == false)
            return ListenerState.ProtocolDisabled;

         if (protocolEnabled == null)
            return ListenerState.Unknown;

         return ListenerState.NotListening;
      }

      /// <summary>
      /// A listener's address and port as an endpoint is written: an IPv6 address
      /// in brackets, "[::]:993", where address, colon and port read ":::993",
      /// which is no address at all; an IPv4 address or a name as it is,
      /// "0.0.0.0:25".
      /// </summary>
      public static string Endpoint(string address, int port)
      {
         string host = (address ?? "").Trim();
         return host.Contains(':') && !host.StartsWith('[')
            ? "[" + host + "]:" + port.ToString(System.Globalization.CultureInfo.InvariantCulture)
            : host + ":" + port.ToString(System.Globalization.CultureInfo.InvariantCulture);
      }

      /// <summary>
      /// The sentence under the list when configured ports are not being
      /// listened on, naming them. It leads with the commonest cause, as each
      /// row's tool tip does (<see cref="ExplainFor"/>): a port added or changed
      /// since the server started is not bound until the server reinitialises,
      /// since it reads its port list at start-up. It led with a port conflict and
      /// sent the reader to an error-log entry that a port never bound does not
      /// have. One whole sentence for one port and one for several, so that a
      /// language can agree its words with the count.
      /// </summary>
      public static string NotListeningSummary(IReadOnlyList<string> endpoints)
      {
         string names = string.Join(", ", endpoints ?? Array.Empty<string>());
         return endpoints == null || endpoints.Count <= 1
            ? F("One configured port is not being listened on: {0}. If you have just added or changed it, reinitialise the server: it reads its port list at start-up. Otherwise another program holds the port, the address does not exist on this machine, or a TLS port's certificate could not be loaded - the error log says which.", names)
            : PluralF(endpoints.Count,
               "{0} configured port is not being listened on: {1}. If you have just added or changed one, reinitialise the server: it reads its port list at start-up. Otherwise another program holds the port, the address does not exist on this machine, or a TLS port's certificate could not be loaded - the error log says which.",
               "{0} configured ports are not being listened on: {1}. If you have just added or changed one, reinitialise the server: it reads its port list at start-up. Otherwise another program holds the port, the address does not exist on this machine, or a TLS port's certificate could not be loaded - the error log says which.",
               endpoints.Count, names);
      }

      /// <summary>ServerStatus's states, as the COM enum numbers them (Application.ServerState).</summary>
      public const int ServerStopped = 1, ServerStarting = 2, ServerRunning = 3, ServerStopping = 4, ServerDraining = 5;

      /// <summary>
      /// Why no configured port is being listened on while the server is not
      /// running, said at the Information level in place of the warning: paused
      /// (Application.Stop stops the servers and closes every listener), in
      /// maintenance mode (EnterDrain closes every acceptor until it is left), or
      /// starting or stopping. Null when it is running, or its state could not
      /// be read, and a port not listened on is a fault of its own.
      ///
      /// The warning blamed a port just added and offered Reinitialise now, which
      /// stops the servers and starts them again - so it ended maintenance mode or
      /// resumed the paused server, under a question that said only that clients
      /// are disconnected for a moment.
      /// </summary>
      public static string WhyNoPortIsListenedOn(int? serverState) => serverState switch
      {
         ServerStopped => L("The server is paused, so none of its ports is being listened on. Resuming it opens them again."),
         ServerDraining => L("The server is in maintenance mode, so its listeners are closed and none of its ports is being listened on. They open again when maintenance mode ends."),
         ServerStarting or ServerStopping => L("The server is starting or stopping, so whether its ports are being listened on cannot be said yet. Open this page again in a moment."),
         _ => null
      };

      /// <summary>
      /// The question a reinitialise asks before it is done to a server that is
      /// not running, whichever question came first: in maintenance mode the one
      /// Apply now asks (SettingApplies.ApplyNowQuestion), and while paused one
      /// that says it starts the server again. A reinitialise stops the servers and
      /// starts them again, which ends maintenance mode and resumes a paused
      /// server. Null when the server is running, starting or stopping, or its
      /// state could not be read.
      /// </summary>
      public static string ReinitialiseQuestionWhenNotRunning(int? serverState) => serverState switch
      {
         ServerDraining => SettingApplies.ApplyNowQuestion(draining: true),
         ServerStopped => L("The server is paused. A reinitialise starts it again: the listeners open and delivery resumes. Reinitialise the server now?"),
         _ => null
      };

      /// <summary>The word shown beside the badge. Always present, never colour alone.</summary>
      public static string WordFor(ListenerState state) => state switch
      {
         ListenerState.Listening => L("Listening"),
         ListenerState.NotListening => L("Not listening"),
         ListenerState.ProtocolDisabled => L("Server off"),
         _ => L("Unknown")
      };

      /// <summary>
      /// <see cref="WordFor(ListenerState)"/> with the server's state, as
      /// Application.ServerState numbers it: while the server is starting or
      /// stopping, a port not listened on is Unknown, since the line under the
      /// list says that whether its ports are being listened on cannot be said yet
      /// (<see cref="WhyNoPortIsListenedOn"/>), and each row's tool tip and
      /// accessible name read "Not listening" before that very sentence. Paused or
      /// in maintenance mode it is Not listening, which the line's sentence
      /// explains; every other state keeps its word.
      /// </summary>
      public static string WordFor(ListenerState state, int? serverState)
         => WordFor(NotYetKnown_(state, serverState) ? ListenerState.Unknown : state);

      /// <summary>
      /// What a row's tool tip and accessible name say: its word, then what that
      /// means for the port (<see cref="ExplainFor(ListenerState, string, int?)"/>),
      /// both with the server's state.
      /// </summary>
      public static string TipFor(ListenerState state, string protocol, int? serverState)
         => WordFor(state, serverState) + ". " + ExplainFor(state, protocol, serverState);

      /// <summary>
      /// <see cref="LevelFor(ListenerState)"/> with the server's state, as
      /// Application.ServerState numbers it: while the server is paused or in
      /// maintenance mode a port not listened on is Information, not a fault of
      /// its own, at the level of the line under the list that says why
      /// (<see cref="WhyNoPortIsListenedOn"/>); each row was a Warning beside that
      /// Information line. While it is starting or stopping the port's state
      /// cannot be said yet, and it is at the level of Unknown, the word it reads
      /// (<see cref="WordFor(ListenerState, int?)"/>). Every other state keeps its
      /// level.
      /// </summary>
      public static StatusLevel LevelFor(ListenerState state, int? serverState)
         => NotYetKnown_(state, serverState)
            ? LevelFor(ListenerState.Unknown)
            : state == ListenerState.NotListening && WhyNoPortIsListenedOn(serverState) != null
               ? StatusLevel.Information
               : LevelFor(state);

      /// <summary>
      /// A port not listened on while the server is starting or stopping: whether
      /// it will be, or still is, listened on cannot be said yet.
      /// </summary>
      private static bool NotYetKnown_(ListenerState state, int? serverState)
         => state == ListenerState.NotListening && serverState is ServerStarting or ServerStopping;

      /// <summary>
      /// <see cref="ExplainFor(ListenerState, string)"/> with the server's state:
      /// while the server is not running, a port not listened on is explained by
      /// that, in the line's own sentence, where each row's tool tip and accessible
      /// name blamed a port just added or another program. Every other state keeps
      /// its sentence.
      /// </summary>
      public static string ExplainFor(ListenerState state, string protocol, int? serverState)
         => state == ListenerState.NotListening && WhyNoPortIsListenedOn(serverState) is string why
            ? why
            : ExplainFor(state, protocol);

      public static StatusLevel LevelFor(ListenerState state) => state switch
      {
         ListenerState.Listening => StatusLevel.Good,
         ListenerState.NotListening => StatusLevel.Warning,
         ListenerState.ProtocolDisabled => StatusLevel.Information,
         _ => StatusLevel.Normal
      };

      /// <summary>
      /// The sentence under the list: what the state means and, where it is a
      /// fault, what to do about it.
      /// </summary>
      public static string ExplainFor(ListenerState state, string protocol) => state switch
      {
         ListenerState.Listening =>
            L("Something on this machine is listening on that address and port. This check reads the operating system's list of open ports, so it cannot tell hMailServer's listener from another program's - if connections are being refused or answered by the wrong service, that is what to look for."),

         // The first cause listed used to be a port conflict, and the sentence sent
         // the reader to an error-log entry. Both were wrong for the commonest case
         // by far: IOService reads the port list ONCE per server start and creates
         // every listener in that pass, so a port added or changed since then is
         // simply not bound - no bind was attempted, so there is no 4316 to find.
         // The ports page adds rows and reloads immediately, which means every port
         // an administrator creates here appeared at once as a yellow row blaming a
         // conflict that does not exist.
         ListenerState.NotListening =>
            F("The {0} server is switched on but nothing is listening here, so every connection to this port is refused. If you have just added or changed this port, that is the reason: the server reads its port list once when it starts, so a new row does nothing until the service is restarted. Otherwise the causes are another program already holding the port, an address that does not exist on this machine, or - on a TLS port - a certificate the server could not load; the server records which, once, in the error log at start-up.", protocol),

         ListenerState.ProtocolDisabled =>
            F("The {0} server is switched off on the Protocols page, so this port is not listened on. The row is kept. Switching the protocol back on does not start the listener on its own - the port list and the enabled protocols are both read once, when the service starts - so restart the service after enabling it.", protocol),

         _ =>
            L("Whether this port is being listened on could not be determined from here. Open the Control Panel on the server itself to see it.")
      };
   }
}
