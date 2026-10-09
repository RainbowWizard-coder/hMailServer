// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Net;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Matching a configured port row against the machine's open ports.
   ///
   /// Both ways of getting this wrong cost something real. Reporting a live port as
   /// down sends an administrator hunting for a conflict that does not exist;
   /// reporting a dead port as up hides the single commonest cause of "mail stopped
   /// arriving and nothing looks wrong". The rules are small and entirely
   /// mechanical, which is exactly why they are pinned here rather than trusted.
   /// </summary>
   public class ListenerProbeTests
   {
      private static IReadOnlyList<IPEndPoint> Listening(params string[] endpoints)
      {
         var list = new List<IPEndPoint>();
         foreach (string endpoint in endpoints)
         {
            int colon = endpoint.LastIndexOf(':');
            list.Add(new IPEndPoint(IPAddress.Parse(endpoint.Substring(0, colon)),
               int.Parse(endpoint.Substring(colon + 1))));
         }
         return list;
      }

      // ---- the ordinary cases ----------------------------------------------

      [Fact]
      public void IsBound_ExactAddressAndPort()
      {
         Assert.True(ListenerProbe.IsBound("192.168.1.10", 25, Listening("192.168.1.10:25")));
      }

      [Fact]
      public void IsBound_WrongPortIsNotAMatch()
      {
         Assert.False(ListenerProbe.IsBound("192.168.1.10", 25, Listening("192.168.1.10:587")));
      }

      [Fact]
      public void IsBound_WrongAddressIsNotAMatch()
      {
         Assert.False(ListenerProbe.IsBound("192.168.1.10", 25, Listening("192.168.1.11:25")));
      }

      /// <summary>
      /// hMailServer stores a wildcard as "0.0.0.0" - IPAddress default-constructs
      /// to IPv4 any and TCPIPPort::GetAddressString returns its ToString - so this
      /// is the shape of nearly every real row.
      ///
      /// A wildcard row is matched by a WILDCARD listener, not by any listener on
      /// the port. Windows reports a successful wildcard bind literally as 0.0.0.0
      /// or [::], so the narrow test is the accurate one; the loose form could read
      /// green when hMailServer's own wildcard listener was down and some other
      /// program happened to hold that port on one specific address.
      /// </summary>
      [Theory]
      [InlineData("0.0.0.0")]
      [InlineData("")]
      [InlineData("   ")]
      [InlineData(null)]
      [InlineData("::")]
      [InlineData("[::]")]
      public void IsBound_AWildcardRowNeedsAWildcardListener(string configured)
      {
         Assert.True(ListenerProbe.IsBound(configured, 143, Listening("0.0.0.0:143")));
         Assert.True(ListenerProbe.IsBound(configured, 143, Listening(":::143")));

         // A listener on one specific address does not serve the wildcard row.
         Assert.False(ListenerProbe.IsBound(configured, 143, Listening("192.168.1.10:143")));

         // Right address shape, wrong port.
         Assert.False(ListenerProbe.IsBound(configured, 143, Listening("0.0.0.0:993")));
      }

      /// <summary>
      /// The other direction, and the one that would otherwise produce a false
      /// alarm on every properly configured server: a listener on the wildcard is
      /// serving the named address too.
      /// </summary>
      [Fact]
      public void IsBound_AWildcardListenerCoversANamedAddress()
      {
         Assert.True(ListenerProbe.IsBound("192.168.1.10", 25, Listening("0.0.0.0:25")));
         Assert.True(ListenerProbe.IsBound("fe80::1", 25, Listening(":::25")));
      }

      /// <summary>
      /// Families are kept apart on purpose. A socket bound to :: covers IPv4 when
      /// dual-stack is on and does not when it is off, and nothing here can tell
      /// which - so it claims neither. The cost is a v4 row reported as down on a
      /// dual-stack-only server; the alternative is reporting a port as up when
      /// nothing answers on it, which is the worse error to make here.
      /// </summary>
      [Fact]
      public void IsBound_DoesNotAssumeDualStack()
      {
         Assert.False(ListenerProbe.IsBound("192.168.1.10", 25, Listening(":::25")));
         Assert.False(ListenerProbe.IsBound("fe80::1", 25, Listening("0.0.0.0:25")));
      }

      // ---- the degenerate cases --------------------------------------------

      [Fact]
      public void IsBound_NoListenersIsNotAMatch()
      {
         Assert.False(ListenerProbe.IsBound("0.0.0.0", 25, Listening()));
         Assert.False(ListenerProbe.IsBound("0.0.0.0", 25, null));
      }

      /// <summary>
      /// A row whose address does not parse cannot be matched against anything.
      /// Saying "not listening" would be a claim about a configuration that is
      /// already broken in a different way, and the port dialog is where that gets
      /// fixed.
      /// </summary>
      [Fact]
      public void IsBound_AnUnparseableAddressMatchesNothing()
      {
         Assert.False(ListenerProbe.IsBound("mail.example.com", 25, Listening("0.0.0.0:25")));
      }

      [Fact]
      public void IsBound_APortOfZeroMatchesNothing()
      {
         Assert.False(ListenerProbe.IsBound("0.0.0.0", 0, Listening("0.0.0.0:0")));
      }

      // ---- the state an administrator actually reads -------------------------

      [Fact]
      public void StateOf_BoundIsListening()
      {
         Assert.Equal(ListenerState.Listening,
            ListenerProbe.StateOf("0.0.0.0", 25, true, Listening("0.0.0.0:25"), localServer: true));
      }

      /// <summary>
      /// A disabled protocol explains an unbound port completely, so it must be
      /// reported as that and not as a fault - otherwise every server that has
      /// deliberately switched POP3 off shows a warning for it forever.
      /// </summary>
      [Fact]
      public void StateOf_ADisabledProtocolIsNotAFault()
      {
         Assert.Equal(ListenerState.ProtocolDisabled,
            ListenerProbe.StateOf("0.0.0.0", 110, false, Listening(), localServer: true));
      }

      /// <summary>
      /// The case the column exists for: enabled, configured, and nothing is
      /// listening.
      /// </summary>
      [Fact]
      public void StateOf_EnabledAndUnboundIsAWarning()
      {
         Assert.Equal(ListenerState.NotListening,
            ListenerProbe.StateOf("0.0.0.0", 25, true, Listening("0.0.0.0:587"), localServer: true));

         Assert.Equal(StatusLevel.Warning, ListenerProbe.LevelFor(ListenerState.NotListening));
      }

      /// <summary>
      /// A port that IS bound is reported as listening even with the protocol
      /// switch off or unreadable - because something really is listening there,
      /// and that is worth knowing. It is very likely another program.
      /// </summary>
      [Fact]
      public void StateOf_BoundWinsOverTheProtocolSwitch()
      {
         Assert.Equal(ListenerState.Listening,
            ListenerProbe.StateOf("0.0.0.0", 25, false, Listening("0.0.0.0:25"), localServer: true));
      }

      [Fact]
      public void StateOf_AnUnreadableSwitchIsUnknownRatherThanBroken()
      {
         Assert.Equal(ListenerState.Unknown,
            ListenerProbe.StateOf("0.0.0.0", 25, null, Listening(), localServer: true));
      }

      /// <summary>
      /// Connected to another host, this machine's open ports describe the wrong
      /// computer, so nothing is claimed at all.
      /// </summary>
      [Fact]
      public void StateOf_ARemoteSessionCannotTell()
      {
         Assert.Equal(ListenerState.Unknown,
            ListenerProbe.StateOf("0.0.0.0", 25, true, Listening("0.0.0.0:25"), localServer: false));
      }

      // ---- what is shown ----------------------------------------------------

      /// <summary>
      /// Every state carries a word, so none of them depends on colour alone.
      /// </summary>
      [Theory]
      [InlineData(ListenerState.Listening, "Listening")]
      [InlineData(ListenerState.NotListening, "Not listening")]
      [InlineData(ListenerState.ProtocolDisabled, "Server off")]
      [InlineData(ListenerState.Unknown, "Unknown")]
      public void WordFor_IsAlwaysPresent(ListenerState state, string expected)
      {
         Assert.Equal(expected, ListenerProbe.WordFor(state));
      }

      /// <summary>
      /// The explanation must name the protocol it is about, and must not be a
      /// stub: an administrator reading "Not listening" needs to be told what to
      /// look for, and this is the only place they are told.
      /// </summary>
      [Fact]
      public void ExplainFor_NamesTheProtocolAndSaysWhatToLookFor()
      {
         string down = ListenerProbe.ExplainFor(ListenerState.NotListening, "SMTP");
         Assert.Contains("SMTP", down);
         Assert.Contains("another program", down);

         string off = ListenerProbe.ExplainFor(ListenerState.ProtocolDisabled, "POP3");
         Assert.Contains("POP3", off);
         Assert.Contains("Protocols page", off);
      }

      /// <summary>
      /// "Listening" must not be read as "hMailServer is listening". This check
      /// reads the operating system's port table, which cannot tell one program's
      /// listener from another's, and the wording has to admit that - the case
      /// where it matters is a port held by a different mail server.
      /// </summary>
      [Fact]
      public void ExplainFor_DoesNotClaimTheListenerIsHMailServers()
      {
         string up = ListenerProbe.ExplainFor(ListenerState.Listening, "IMAP");
         Assert.Contains("cannot tell", up);
      }

      /// <summary>
      /// The live query has to survive a machine that will not answer it: it runs
      /// while the page is being built, and an exception there would take the page
      /// down rather than degrade to "unknown".
      /// </summary>
      [Fact]
      public void ActiveListeners_NeverThrows()
      {
         Assert.NotNull(ListenerProbe.ActiveListeners());
      }

      /// <summary>
      /// An endpoint as it is written: an IPv6 address in brackets, so that the
      /// summary under the ports list no longer printed IMAP on :: as ":::993".
      /// </summary>
      [Theory]
      [InlineData("::", 993, "[::]:993")]
      [InlineData("2001:db8::25", 25, "[2001:db8::25]:25")]
      [InlineData("[::1]", 143, "[::1]:143")]
      [InlineData("0.0.0.0", 25, "0.0.0.0:25")]
      [InlineData("192.168.1.10", 587, "192.168.1.10:587")]
      public void Endpoint_BracketsAnIPv6Address(string address, int port, string expected)
      {
         Assert.Equal(expected, ListenerProbe.Endpoint(address, port));
      }

      /// <summary>
      /// The summary under the list leads with the commonest cause - a port added
      /// or changed since the server started is not bound until it reinitialises -
      /// as each row's tool tip does, and is one whole sentence for one port and
      /// one for several; it led with a port conflict and an error-log entry that
      /// a port never bound does not have.
      /// </summary>
      [Fact]
      public void NotListeningSummary_LeadsWithReinitialising_AndNamesThePorts()
      {
         string one = ListenerProbe.NotListeningSummary(new[] { "IMAP [::]:993" });
         Assert.StartsWith("One configured port is not being listened on: IMAP [::]:993. If you have just added or changed it, reinitialise the server", one);

         string two = ListenerProbe.NotListeningSummary(new[] { "SMTP 0.0.0.0:587", "IMAP [::]:993" });
         Assert.StartsWith("2 configured ports are not being listened on: SMTP 0.0.0.0:587, IMAP [::]:993. If you have just added or changed one, reinitialise the server", two);

         foreach (string text in new[] { one, two })
         {
            Assert.True(text.IndexOf("reinitialise", System.StringComparison.Ordinal) < text.IndexOf("another program", System.StringComparison.Ordinal),
               "The summary names a port conflict before the commonest cause.");
            Assert.EndsWith("the error log says which.", text);
         }
      }

      /// <summary>
      /// A server that is not running listens on none of its ports, and the page
      /// says why at the Information level - paused, in maintenance mode, starting
      /// or stopping - where its warning blamed a port just added and offered
      /// Reinitialise now, which ends maintenance mode and resumes a paused
      /// server. A running server, or one whose state could not be read, leaves
      /// the warning to say what it says.
      /// </summary>
      [Fact]
      public void WhyNoPortIsListenedOn_SaysTheServerIsNotRunning_AndNothingWhileItIs()
      {
         Assert.Equal(1, ListenerProbe.ServerStopped);
         Assert.Equal(3, ListenerProbe.ServerRunning);
         Assert.Equal(5, ListenerProbe.ServerDraining);

         Assert.StartsWith("The server is paused, so none of its ports is being listened on.", ListenerProbe.WhyNoPortIsListenedOn(ListenerProbe.ServerStopped));
         Assert.StartsWith("The server is in maintenance mode, so its listeners are closed", ListenerProbe.WhyNoPortIsListenedOn(ListenerProbe.ServerDraining));
         Assert.StartsWith("The server is starting or stopping", ListenerProbe.WhyNoPortIsListenedOn(ListenerProbe.ServerStarting));
         Assert.StartsWith("The server is starting or stopping", ListenerProbe.WhyNoPortIsListenedOn(ListenerProbe.ServerStopping));
         Assert.Null(ListenerProbe.WhyNoPortIsListenedOn(ListenerProbe.ServerRunning));
         Assert.Null(ListenerProbe.WhyNoPortIsListenedOn(null));
      }

      /// <summary>
      /// A reinitialise of a server in maintenance mode asks the question Apply now
      /// asks, which says that it ends maintenance mode, and of a paused one says
      /// that it starts it again; of a running one, or one whose state could not be
      /// read, it asks nothing more.
      /// </summary>
      [Fact]
      public void ReinitialiseQuestionWhenNotRunning_SaysWhatAReinitialiseEnds()
      {
         Assert.Equal(SettingApplies.ApplyNowQuestion(draining: true), ListenerProbe.ReinitialiseQuestionWhenNotRunning(ListenerProbe.ServerDraining));
         Assert.Contains("maintenance mode", ListenerProbe.ReinitialiseQuestionWhenNotRunning(ListenerProbe.ServerDraining));
         Assert.StartsWith("The server is paused. A reinitialise starts it again", ListenerProbe.ReinitialiseQuestionWhenNotRunning(ListenerProbe.ServerStopped));
         Assert.Null(ListenerProbe.ReinitialiseQuestionWhenNotRunning(ListenerProbe.ServerRunning));
         Assert.Null(ListenerProbe.ReinitialiseQuestionWhenNotRunning(null));
      }
   }
}
