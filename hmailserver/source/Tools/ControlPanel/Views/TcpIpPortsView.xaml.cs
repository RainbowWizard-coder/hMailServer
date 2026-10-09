// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class TcpIpPortsView : UserControl, IPageLifecycle
   {
      public class PortRow
      {
         public int Id { get; set; }
         public string Protocol { get; set; }
         public string Address { get; set; }
         public int Port { get; set; }
         public string Security { get; set; }
         public string Certificate { get; set; }

         /// <summary>What an SMTP port does with AUTH - On, Off or Honeypot - and empty for POP3 and IMAP.</summary>
         public string Auth { get; set; }

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString()
            => string.Join(", ", new[] { Protocol + " " + Address + ":" + Port, Security, string.IsNullOrEmpty(Auth) ? "" : F("AUTH {0}", Auth), ListeningWord, Certificate }.Where(s => !string.IsNullOrWhiteSpace(s)));

         /// <summary>"Listening", "Not listening", "Server off" or "Unknown".</summary>
         public string ListeningWord { get; set; }

         /// <summary>The full explanation, as the cell's tooltip and its accessible name.</summary>
         public string ListeningTip { get; set; }

         /// <summary>The badge, already shaped and coloured by ShapeMarkVisuals.</summary>
         public FrameworkElement ListeningMark { get; set; }
      }

      private static readonly string[] SecurityNames = { L("None"), L("SSL/TLS"), L("STARTTLS (optional)"), L("STARTTLS (required)") };

      // SMTPAuthenticationMode's three values (GitLab work item #54), as the port
      // dialog's box names them first.
      private static readonly string[] SmtpAuthNames = { L("On"), L("Off"), L("Honeypot") };

      /// <summary>
      /// The AUTH cell: empty for POP3 and IMAP, which have no such choice; On for
      /// a server older than 6.3.6, which has no SMTPAuthentication and on which
      /// every port takes AUTH.
      /// </summary>
      private static string SmtpAuthName(dynamic port, int protocol)
      {
         if (protocol != ServerSession.SessionSmtp)
            return "";

         int mode;
         try
         {
            mode = (int)port.SMTPAuthentication;
         }
         catch (System.Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            mode = 0;
         }

         return mode >= 0 && mode < SmtpAuthNames.Length ? SmtpAuthNames[mode] : "#" + mode;
      }

      /// <summary>The listening mark's box, and the gap between it and its word (the cell template's Margin).</summary>
      private const double ListeningMarkWidth = 10;
      private const double ListeningMarkGap = 6;

      public TcpIpPortsView()
      {
         InitializeComponent();
         hMailServer.ControlPanel.Services.SelectionGate.Bind(PortGrid, PropertiesButton, DeleteButton);
         FloorColumns_();
      }

      /// <summary>
      /// No column of the grid is narrower than its own header or than the widest
      /// of its words in the current language - the security and authentication
      /// names, the listening word with the mark before it, the widest IPv4
      /// address, the highest port - so that a narrow window scrolls the grid
      /// sideways rather than cutting a word, as the Suppression list floors its
      /// columns. Once the grid is narrower than its columns and the certificate
      /// column has given up all it can, WPF takes the rest from the fixed columns
      /// down to their MinWidth, which was 20, and a cell's words are cut mid-glyph
      /// with nothing to say so: the 12 pixels the page keeps clear of its scroll
      /// bar took the last glyph from 待ち受けていません (Not listening) in a
      /// 1280-pixel window. The floors leave the grid narrower than a 1280-pixel
      /// window in every language, so the words are whole without a scroll bar
      /// there; the smallest window scrolls.
      /// </summary>
      private void FloorColumns_()
      {
         foreach (DataGridColumn column in PortGrid.Columns)
            Floor_(column, GridStyles.HeaderWidth(column.Header as string));

         Floor_(AddressColumn, GridStyles.CellWidth("255.255.255.255")); // no-loc: the widest IPv4 address
         Floor_(PortColumn, GridStyles.CellWidth("65535")); // no-loc: the highest port
         foreach (string name in SecurityNames)
            Floor_(SecurityColumn, GridStyles.CellWidth(name));
         foreach (string name in SmtpAuthNames)
            Floor_(AuthColumn, GridStyles.CellWidth(name));
         foreach (ListenerState state in Enum.GetValues<ListenerState>())
            Floor_(ListeningColumn, GridStyles.CellWidth(ListenerProbe.WordFor(state), ListeningMarkWidth + ListeningMarkGap));
      }

      /// <summary>Raises a column's floor to <paramref name="width"/> where that is more.</summary>
      private static void Floor_(DataGridColumn column, double width) => column.MinWidth = Math.Max(column.MinWidth, width);

      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      private static string ProtocolName(int sessionType) => sessionType switch
      {
         ServerSession.SessionSmtp => "SMTP",
         ServerSession.SessionPop3 => "POP3",
         ServerSession.SessionImap => "IMAP",
         _ => "#" + sessionType
      };

      private void Reload()
      {
         try
         {
            Read_();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "No ports configured." is a
            // fact this page has now: both go, with the listener summary that
            // was read with them, the notice says what happened, with Try again,
            // and Add and Restore defaults wait for a read that works
            // (Services/ReadFailure).
            ReadFailure.Show(Notice, EmptyStatus, PortGrid, L("Could not read the ports: "), ex, Reload,
               AddPortButton, RestoreDefaultsButton);
            ListenerSummary.Visibility = Visibility.Collapsed;
         }
      }

      private void Read_()
      {
         var rows = new List<PortRow>();
         var certNames = LoadCertNames();

         // Read once per reload, not per row: GetActiveTcpListeners walks the whole
         // TCP table, and a server with twenty port rows would otherwise walk it
         // twenty times for the same answer.
         bool local = ServerSession.IsLocalSession;
         IReadOnlyList<System.Net.IPEndPoint> listeners = local
            ? ListenerProbe.ActiveListeners()
            : System.Array.Empty<System.Net.IPEndPoint>();
         Dictionary<string, bool?> protocolEnabled = LoadProtocolSwitches();

         dynamic ports = ServerSession.Current.Application.Settings.TCPIPPorts;
         try
         {
            int count = (int)ports.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic port = ports.Item[i];
               int security = (int)port.ConnectionSecurity;
               int certId = (int)port.SSLCertificateID;
               int protocolNumber = (int)port.Protocol;
               string protocol = ProtocolName(protocolNumber);
               string auth = SmtpAuthName((object)port, protocolNumber);
               string address = (string)port.Address;
               int portNumber = (int)port.PortNumber;

               protocolEnabled.TryGetValue(protocol, out bool? enabled);

               ListenerState state = ListenerProbe.StateOf(address, portNumber, enabled, listeners, local);
               StatusPresentation presentation = StatusSemantics.For(ListenerProbe.LevelFor(state));

               rows.Add(new PortRow
               {
                  Id = (int)port.ID,
                  Protocol = protocol,
                  Address = address,
                  Port = portNumber,
                  Security = security >= 0 && security < SecurityNames.Length ? SecurityNames[security] : "#" + security,
                  Certificate = certId > 0 && certNames.TryGetValue(certId, out string n) ? n : "",
                  Auth = auth,
                  ListeningWord = ListenerProbe.WordFor(state),
                  ListeningTip = ListenerProbe.WordFor(state) + ". " + ListenerProbe.ExplainFor(state, protocol),
                  ListeningMark = MakeListeningMark(presentation)
               });
               ServerSession.Release(port);
            }
         }
         finally
         {
            ServerSession.Release(ports);
         }

         ReadFailure.Clear(Notice, AddPortButton, RestoreDefaultsButton);
         PortGrid.ItemsSource = rows;
         ListSearch.Apply(PortGrid, SearchBar.SearchText);
         StatusText.Show(EmptyStatus, null, rows.Count, null, L("No ports configured."));

         ShowListenerSummary(rows, local);
      }

      /// <summary>
      /// One row's badge.
      ///
      /// The colour goes on as a resource reference rather than a resolved brush,
      /// because ThemeTokens republishes the status brushes on every theme change
      /// and a badge handed a brush would stay painted for the previous theme.
      /// ApplyMark is also what knows that a stroke-only mark has to be stroked
      /// rather than filled - a filled cross renders as nothing at all.
      /// </summary>
      private static FrameworkElement MakeListeningMark(StatusPresentation presentation)
      {
         var path = new System.Windows.Shapes.Path
         {
            Width = ListeningMarkWidth,
            Height = ListeningMarkWidth,
            Stretch = System.Windows.Media.Stretch.Fill,
            VerticalAlignment = VerticalAlignment.Center
         };

         ShapeMarkVisuals.ApplyMark(path, presentation.Shape, presentation.BrushKey);
         return path;
      }

      /// <summary>
      /// The one line under the list, shown only when it has something to say.
      ///
      /// A per-row badge answers "is this port up"; it does not answer "why has mail
      /// stopped", which is the question somebody opens this page with. A port that
      /// is configured, enabled and not bound is the single most likely cause and
      /// the least visible, so it gets said in words at the bottom of the page as
      /// well as shown in a column.
      /// </summary>
      private void ShowListenerSummary(List<PortRow> rows, bool local)
      {
         if (!local)
         {
            ShowSummary_(StatusLevel.Information,
               L("Connected to another host, so whether these ports are being listened on cannot be read from here - open the Control Panel on the server itself to see it."));
            return;
         }

         var down = rows.FindAll(r => r.ListeningWord == ListenerProbe.WordFor(ListenerState.NotListening));

         if (down.Count == 0)
         {
            ListenerSummary.Visibility = Visibility.Collapsed;
            return;
         }

         var names = new List<string>();
         foreach (PortRow row in down)
            names.Add(row.Protocol + " " + row.Address + ":" + row.Port);

         // The first sentence says which, the second why, written whole with the
         // first placed in it.
         string which = down.Count == 1
            ? F("One configured port is not being listened on: {0}.", string.Join(", ", names))
            : PluralF(down.Count, "{0} configured port is not being listened on: {1}.", "{0} configured ports are not being listened on: {1}.",
               down.Count, string.Join(", ", names));
         ShowSummary_(StatusLevel.Warning,
            F("{0} Connections to a port that is not listened on are refused. The usual causes are another program already holding the port, a bind address that does not exist on this machine, or - on a TLS port - a certificate the server could not load; the server records which, once, in the error log at start-up.", which));
      }

      /// <summary>The summary as a notice at its level: a warning for a dead port,
      /// information when the answer cannot be read from this machine.</summary>
      private void ShowSummary_(StatusLevel level, string text)
      {
         ListenerSummary.Level = level;
         ListenerSummary.Text = text;
         ListenerSummary.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// ServiceSMTP / ServicePOP3 / ServiceIMAP, keyed by the same protocol names
      /// the rows carry. A value of null means the switch could not be read, which
      /// makes an unbound port "unknown" rather than "broken" - the difference
      /// between asking somebody to investigate and telling them nothing useful.
      /// </summary>
      private static Dictionary<string, bool?> LoadProtocolSwitches()
      {
         var map = new Dictionary<string, bool?>
         {
            ["SMTP"] = null,
            ["POP3"] = null,
            ["IMAP"] = null
         };

         dynamic settings = ServerSession.Current.Application.Settings;
         try
         {
            map["SMTP"] = (bool)settings.ServiceSMTP;
            map["POP3"] = (bool)settings.ServicePOP3;
            map["IMAP"] = (bool)settings.ServiceIMAP;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
         finally
         {
            ServerSession.Release((object)settings);
         }

         return map;
      }

      private void Search_TextChanged(object sender, EventArgs e)
         => ListSearch.Apply(PortGrid, SearchBar.SearchText);

      private static Dictionary<int, string> LoadCertNames()
      {
         var map = new Dictionary<int, string>();
         dynamic certs = ServerSession.Current.Application.Settings.SSLCertificates;
         try
         {
            int count = (int)certs.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic c = certs.Item[i];
               map[(int)c.ID] = (string)c.Name;
               ServerSession.Release(c);
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
         finally
         {
            ServerSession.Release(certs);
         }
         return map;
      }

      private void Edit_Click(object sender, RoutedEventArgs e)
      {
         if (PortGrid.SelectedItem is not PortRow row)
         {
            MessageBox.Show(L("Select a port first."), L("Control Panel"));
            return;
         }

         var dialog = new TcpIpPortDialog(Window.GetWindow(this), row.Id);
         dialog.ShowDialog();
         Reload();
         if (dialog.Saved)
            OfferReinitialize_();
      }

      /// <summary>
      /// A saved port is not what the server listens on until its services are
      /// started again (Application.Reinitialize stops and starts them in place,
      /// reading the ports afresh). Nothing said so but the page's subtitle, so a
      /// first setup that gave the submission port TLS went on serving it in the
      /// clear with every sign of being done. Offered, not done: it disconnects
      /// whoever is connected.
      /// </summary>
      private void OfferReinitialize_()
      {
         // Later takes Enter: a reinitialise disconnects whoever is connected.
         if (!MessageBox.Confirm(
                L("Saved. The server goes on listening as it did until its services are reinitialised - stopped and started again in place, which disconnects connected clients for a moment. Reinitialise the server now?"),
                L("Control Panel"), L("_Reinitialise now"), cancelByDefault: true, decline: L("_Later")))
            return;

         Mouse.OverrideCursor = Cursors.Wait;
         string failure = null;
         try
         {
            ServerSession.Current.Application.Reinitialize();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failure = ServerSession.DescribeComError(ex);
         }
         finally
         {
            Mouse.OverrideCursor = null;
         }

         if (failure == null)
            Services.Toast.Success(L("Reinitialised - the server now listens on the ports as this page lists them."));
         else
            Services.Toast.Failure(F("Could not reinitialise: {0}", failure), L("Control Panel"));

         Reload();
      }

      private void Add_Click(object sender, RoutedEventArgs e)
      {
         if (!int.TryParse(NewPort.Text.Trim(), out int portNumber) || portNumber <= 0 || portNumber > 65535)
         {
            MessageBox.Show(L("Enter a valid port number."), L("Control Panel"));
            return;
         }

         int protocol = NewProtocol.SelectedIndex switch
         {
            0 => ServerSession.SessionSmtp,
            1 => ServerSession.SessionPop3,
            _ => ServerSession.SessionImap
         };

         dynamic ports = ServerSession.Current.Application.Settings.TCPIPPorts;
         try
         {
            dynamic port = ports.Add();
            port.Protocol = protocol;
            port.Address = NewAddress.Text.Trim();
            port.PortNumber = portNumber;
            port.ConnectionSecurity = NewSecurity.SelectedIndex;
            port.Save();
            ServerSession.Release(port);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not add the port: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(ports);
         }

         NewPort.Text = "";
         Reload();
         OfferReinitialize_();
      }

      /// <summary>
      /// Settings.TCPIPPorts.SetDefault() over COM: every configured port is replaced
      /// with SMTP 25 + 587, POP3 110 and IMAP 143, no connection security. The COM
      /// call exists since the classic Administrator; this button went missing when
      /// that tool was retired. The failure text from the server is shown verbatim,
      /// after a prefix that says what failed - it is written for exactly this
      /// situation (the old ports are gone even when writing the new ones failed).
      /// </summary>
      private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
      {
         if (!MessageBox.Confirm(
                L("Replace ALL configured ports with the defaults?\n\nSMTP 25 and 587, POP3 110, IMAP 143 - without connection security, and with SMTP authentication On on both SMTP ports, so a port 25 that is a honeypot takes AUTH again. TLS ports and custom bindings will be removed. The change takes effect when the server is reinitialised."),
                L("Control Panel"), L("_Replace all ports"), destructive: true))
            return;

         dynamic ports = ServerSession.Current.Application.Settings.TCPIPPorts;
         bool restored = false;
         try
         {
            ports.SetDefault();
            restored = true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not restore the default TCP/IP ports: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Error);
         }
         finally
         {
            ServerSession.Release(ports);
         }

         Reload();
         if (restored)
            OfferReinitialize_();
      }

      private void Delete_Click(object sender, RoutedEventArgs e)
      {
         if (PortGrid.SelectedItem is not PortRow row)
            return;

         if (!MessageBox.Confirm(F("Delete the {0} port {1} binding?", row.Protocol, row.Port), L("Control Panel"), L("_Delete port"), destructive: true))
            return;

         bool deleted = false;
         dynamic ports = ServerSession.Current.Application.Settings.TCPIPPorts;
         try
         {
            int count = (int)ports.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic port = ports.Item[i];
               if (ProtocolName((int)port.Protocol) == row.Protocol &&
                   (int)port.PortNumber == row.Port &&
                   (string)port.Address == row.Address)
               {
                  port.Delete();
                  ServerSession.Release(port);
                  deleted = true;
                  break;
               }
               ServerSession.Release(port);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the port: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            deleted = false;
         }
         finally
         {
            ServerSession.Release(ports);
         }

         Reload();
         if (deleted)
            OfferReinitialize_();
      }
   }
}
