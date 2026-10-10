// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Editor for one TCP/IP port binding, including the SSL certificate to use
   /// (which the inline add-form on the page cannot set) and the client
   /// certificate (mutual TLS) policy, which nothing else in the interface
   /// could set at all. On the standard frame: a port number that will not do
   /// is said on the port field, a client-certificate combination the server
   /// refuses is said in a notice while it is being chosen, and a save the
   /// server refuses is said in a notice above the fields.
   /// </summary>
   public class TcpIpPortDialog : FluentDialogWindow
   {
      private readonly int portId_;

      private readonly ComboBox protocol_ = new();
      private readonly TextBox address_ = new();
      private readonly TextBox port_ = new();
      private readonly FieldRow portRow_;
      private readonly ComboBox security_ = new();

      // The certificate's row, which says a TLS port without one (review finding
      // F222): the dialog saved such a port with "(none)", and it did not start.
      private readonly FieldRow certificateRow_;
      private int certificateCount_;
      private readonly ComboBox certificate_ = new();
      private readonly ComboBox clientCertPolicy_ = new();
      private readonly ComboBox smtpAuth_ = new();
      private readonly TextBox clientCertCaFile_ = new();
      private readonly InlineNotice clientCertNotice_ = DialogFields.Notice();
      private readonly InlineNotice notice_ = DialogFields.Notice();

      // The rows that apply only sometimes (review finding F223): SMTP
      // authentication to an SMTP port, the CA bundle and the policy's hint to a
      // policy that is not Off. The dialog was 880 pixels tall at 520, taller
      // than the smallest window, with SMTP's five-line hint on a POP3 or IMAP
      // port where its box was disabled, and the CA bundle under a policy of Off.
      private readonly FieldRow smtpAuthRow_;
      private readonly FieldRow clientCertPolicyRow_;
      private readonly FieldRow caFieldRow_;

      // The frame, whose heading names the port once it is read.
      private readonly DialogFrame frame_;

      public TcpIpPortDialog(Window owner, int portId)
      {
         portId_ = portId;
         Owner = owner;
         // Named for the port once it is read (Load): "TCP/IP port" over "Port
         // binding", opened by Properties, never said which port it was.
         Title = L("TCP/IP port");

         var body = new StackPanel();
         body.Children.Add(notice_);

         protocol_.Items.Add(DialogFields.Combo("SMTP", ServerSession.SessionSmtp));
         protocol_.Items.Add(DialogFields.Combo("POP3", ServerSession.SessionPop3));
         protocol_.Items.Add(DialogFields.Combo("IMAP", ServerSession.SessionImap));
         body.Children.Add(Label(L("_Protocol"), protocol_));

         body.Children.Add(Label(L("B_ind address"), address_));
         portRow_ = Label(L("P_ort"), port_);
         body.Children.Add(portRow_);

         security_.Items.Add(DialogFields.Combo(L("None"), 0));
         security_.Items.Add(DialogFields.Combo(L("SSL/TLS"), 1));
         security_.Items.Add(DialogFields.Combo(L("STARTTLS (optional)"), 2));
         security_.Items.Add(DialogFields.Combo(L("STARTTLS (required)"), 3));
         body.Children.Add(Label(L("_Connection security"), security_));

         certificateRow_ = Label(L("SSL c_ertificate"), certificate_).WithHint(L("Required for SSL/TLS and STARTTLS."));
         body.Children.Add(certificateRow_);
         security_.SelectionChanged += (s, e) => UpdateCertificate_();
         certificate_.SelectionChanged += (s, e) => certificateRow_.Error = null;

         // What an SMTP port does with AUTH (GitLab work item #54): the three values
         // of SMTPAuthenticationMode in SocketConstants.h, spelled out as what each
         // does to a client that sends AUTH. A POP3 or IMAP port has no such choice,
         // so for one the box shows On and cannot be changed.
         smtpAuth_.Items.Add(DialogFields.Combo(L("On - offer AUTH and check the password"), 0));
         smtpAuth_.Items.Add(DialogFields.Combo(L("Off - do not offer AUTH; refuse it and ban the address"), 1));
         smtpAuth_.Items.Add(DialogFields.Combo(L("Honeypot - offer AUTH; refuse it and ban the address"), 2));
         // One sentence; the rest - the 502, the ban's length and who is never
         // banned - is docs/SmtpAuthOnDeliveryPorts.md's.
         smtpAuthRow_ = Label(L("S_MTP authentication"), smtpAuth_)
            .WithHint(L("Off and Honeypot refuse AUTH and ban the address that sends it, Honeypot going on offering it as bait; mail clients sign in on 587, not on port 25."));
         body.Children.Add(smtpAuthRow_);
         protocol_.SelectionChanged += (s, e) => UpdateSmtpAuthentication();

         // Client certificates (mutual TLS), per port. The three options are the
         // three values of ClientCertificatePolicy in SocketConstants.h, spelled
         // out as what each one does to a connection. The sentence that was the
         // combo's help text is the row's hint now, read and heard alike.
         clientCertPolicy_.Items.Add(DialogFields.Combo(L("Off"), 0));
         clientCertPolicy_.Items.Add(DialogFields.Combo(L("Request (verify and log, never refuse)"), 1));
         clientCertPolicy_.Items.Add(DialogFields.Combo(L("Require (refuse a connection without a trusted certificate)"), 2));
         clientCertPolicyRow_ = Label(L("Client certificate polic_y (mutual TLS)"), clientCertPolicy_);
         body.Children.Add(clientCertPolicyRow_);

         var caRow = new Grid();
         caRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         caRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         Grid.SetColumn(clientCertCaFile_, 0);
         caRow.Children.Add(clientCertCaFile_);
         var caBrowse = new Wpf.Ui.Controls.Button { Content = L("_Browse…"), Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0), VerticalAlignment = VerticalAlignment.Top };
         AutomationProperties.SetAutomationId(caBrowse, "ClientCertCaBrowse");
         ControlHelp.Explain(caBrowse, L("Browse for a CA certificate bundle file"));
         caBrowse.Click += (s, e) =>
         {
            string file = PathPicker.PickFile(clientCertCaFile_.Text,
               "PEM/certificate files (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*");
            if (file != null)
               clientCertCaFile_.Text = file;
         };
         Grid.SetColumn(caBrowse, 1);
         caRow.Children.Add(caBrowse);

         // The note is the kind of thing an administrator has to read before they
         // go looking for a "generate" button that does not exist. The row shows
         // it under the box; the box itself is told it as help text, because the
         // row only sees the grid the box and its browse button share.
         string caNote = L("hMailServer does not create or manage client certificates. The certificate authority and the client certificates themselves must be produced outside hMailServer (for example with OpenSSL or an internal PKI) - the server only trusts the CA bundle it is given here.");
         caFieldRow_ = Label(L("C_A certificate bundle (PEM) that client certificates must chain to"), caRow).WithHint(caNote);
         AutomationProperties.SetHelpText(clientCertCaFile_, caNote);
         body.Children.Add(caFieldRow_);

         // Inline validation for the combinations the server refuses to save, so
         // the dialog says so while the user is still choosing rather than
         // relaying a COM error after Save. A notice carries the state in colour,
         // shape and word, and is a polite live region, so a screen reader hears
         // it when it appears.
         body.Children.Add(clientCertNotice_);

         security_.SelectionChanged += (s, e) => UpdateClientCertificateValidation();
         clientCertPolicy_.SelectionChanged += (s, e) => { UpdateClientCertificateRows(); UpdateClientCertificateValidation(); };
         clientCertCaFile_.TextChanged += (s, e) => UpdateClientCertificateValidation();

         // Enter saves and Escape cancels.
         var save = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         save.Click += (s, e) => Save();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         UpdateClientCertificateRows();
         frame_ = UseFrame(L("TCP/IP port"), body, save, cancel, width: 520);

         // Room for the port's or the certificate's message, where the dialog is
         // not at its cap; a press refuses one of them at a time.
         KeepRoomForFieldErrors(2, portRow_, certificateRow_);
         Loaded += (s, e) => Load();
      }

      private void LoadCertificates(int selectedId)
      {
         certificate_.Items.Add(DialogFields.Combo(L("(none)"), 0));
         dynamic certs = ServerSession.Current.Application.Settings.SSLCertificates;
         try
         {
            int count = (int)certs.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic c = certs.Item[i];
               certificate_.Items.Add(DialogFields.Combo((string)c.Name, (int)c.ID));
               ServerSession.Release(c);
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
         finally { ServerSession.Release(certs); }
         DialogFields.SelectCombo(certificate_, selectedId, orFirst: true);
         certificateCount_ = certificate_.Items.Count - 1;
      }

      /// <summary>The certificate box follows the security: enabled while it is not None, its message gone with a change of either.</summary>
      private void UpdateCertificate_()
      {
         certificate_.IsEnabled = DialogFields.ComboValue(security_) != 0;
         certificateRow_.Error = null;
      }

      private dynamic FindPort(dynamic ports)
      {
         int count = (int)ports.Count;
         for (int i = 0; i < count; i++)
         {
            dynamic p = ports.Item[i];
            if ((int)p.ID == portId_)
               return p;
            ServerSession.Release(p);
         }
         return null;
      }

      private void Load()
      {
         dynamic ports = ServerSession.Current.Application.Settings.TCPIPPorts;
         try
         {
            dynamic p = FindPort(ports);
            if (p == null) { Close(); return; }
            DialogFields.SelectCombo(protocol_, (int)p.Protocol, orFirst: true);
            address_.Text = (string)p.Address ?? "";
            port_.Text = ((int)p.PortNumber).ToString();
            DialogFields.SelectCombo(security_, (int)p.ConnectionSecurity, orFirst: true);
            LoadCertificates((int)p.SSLCertificateID);
            DialogFields.SelectCombo(clientCertPolicy_, (int)p.ClientCertificatePolicy, orFirst: true);
            clientCertCaFile_.Text = (string)p.ClientCertificateCAFile ?? "";
            DialogFields.SelectCombo(smtpAuth_, SmtpAuthentication((object)p), orFirst: true);
            UpdateSmtpAuthentication();
            UpdateCertificate_();
            UpdateClientCertificateRows();
            UpdateClientCertificateValidation();
            // The window and its heading say which port this is, as it was read.
            Title = PortName(DialogFields.ComboValue(protocol_), (int)p.PortNumber);
            frame_.Heading = Title;
            ServerSession.Release(p);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not load the port: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            Close();
         }
         finally
         {
            ServerSession.Release(ports);
         }
      }

      /// <summary>True once the port was saved, so the page can offer to apply it.</summary>
      public bool Saved { get; private set; }

      private void Save()
      {
         notice_.Hide();
         DialogFields.ClearErrors(portRow_, certificateRow_);

         if (!int.TryParse(port_.Text.Trim(), out int portNumber) || portNumber <= 0 || portNumber > 65535)
         {
            DialogFields.ShowError(portRow_, L("Enter a valid port number."));
            return;
         }

         // A port with connection security presents a certificate: saved with
         // "(none)", it did not start (review finding F222).
         string certificateProblem = PortChanges.CertificateProblem(DialogFields.ComboValue(security_), DialogFields.ComboValue(certificate_), certificateCount_);
         if (certificateProblem != null)
         {
            DialogFields.ShowError(certificateRow_, certificateProblem);
            return;
         }

         // The server refuses these combinations too (PersistentTCPIPPort), so
         // this is a courtesy, not the enforcement: a COM save error is still
         // handled below in case the dialog and the server ever disagree. The
         // notice is already on screen; the keyboard goes to the policy.
         if (ClientCertificateValidationError() != null)
         {
            UpdateClientCertificateValidation();
            clientCertPolicy_.Focus();
            return;
         }

         dynamic ports = ServerSession.Current.Application.Settings.TCPIPPorts;
         try
         {
            dynamic p = FindPort(ports);
            if (p == null) { Close(); return; }
            p.Protocol = DialogFields.ComboValue(protocol_);
            p.Address = address_.Text.Trim();
            p.PortNumber = portNumber;
            p.ConnectionSecurity = DialogFields.ComboValue(security_);
            p.SSLCertificateID = DialogFields.ComboValue(certificate_);
            p.ClientCertificatePolicy = DialogFields.ComboValue(clientCertPolicy_);
            p.ClientCertificateCAFile = clientCertCaFile_.Text.Trim();
            // Written only when the server has the property and something other
            // than On is chosen or was there: a server older than 6.3.6 has no
            // SMTPAuthentication, and its every port is On already.
            int smtpAuthentication = DialogFields.ComboValue(smtpAuth_);
            if (smtpAuthentication != 0 || SmtpAuthentication((object)p) != 0)
               p.SMTPAuthentication = smtpAuthentication;
            p.Save();
            ServerSession.Release(p);
            Saved = true;
            Close();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not save the port: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(ports);
         }
      }

      // ---- SMTP authentication ----

      /// <summary>
      /// The port's SMTPAuthentication, or 0 (On) from a server older than 6.3.6,
      /// which does not have the property and on which every port takes AUTH.
      /// </summary>
      private static int SmtpAuthentication(object port)
      {
         try
         {
            return (int)((dynamic)port).SMTPAuthentication;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return 0;
         }
      }

      /// <summary>
      /// AUTH is a question for an SMTP port only; the server refuses Off and
      /// Honeypot on any other (PersistentTCPIPPort), so the box is On, and its
      /// row is not shown, while the protocol is POP3 or IMAP: a disabled box
      /// under a hint about port 25 said nothing about the port being edited.
      /// </summary>
      private void UpdateSmtpAuthentication()
      {
         bool smtp = DialogFields.ComboValue(protocol_) == ServerSession.SessionSmtp;
         if (!smtp)
            DialogFields.SelectCombo(smtpAuth_, 0, orFirst: true);
         smtpAuth_.IsEnabled = smtp;
         smtpAuthRow_.Visibility = smtp ? Visibility.Visible : Visibility.Collapsed;
      }

      /// <summary>
      /// The port's name as the dialog's title and heading say it: "SMTP port
      /// 587" (F("{0} port {1}")), the protocol by its own name.
      /// </summary>
      internal static string PortName(int protocol, int number)
      {
         string name = protocol == ServerSession.SessionPop3 ? "POP3" : protocol == ServerSession.SessionImap ? "IMAP" : "SMTP"; // no-loc: the protocols' names
         return F("{0} port {1}", name, number.ToString(System.Globalization.CultureInfo.InvariantCulture));
      }

      // ---- client certificate rows ----

      /// <summary>
      /// What Request and Require do, and the CA bundle they verify against, while
      /// the policy is one of them; neither while it is Off, when no certificate
      /// is asked for and no bundle is read.
      /// </summary>
      private void UpdateClientCertificateRows()
      {
         bool asked = DialogFields.ComboValue(clientCertPolicy_) != 0;
         if (asked)
            clientCertPolicyRow_.Hint = L("Request asks every client for a certificate, verifies and logs one if it is offered, and never refuses the connection - use it to inventory which clients would survive Require before enforcing it. Require refuses the connection unless the client presents a certificate that chains to the CA bundle below.");
         else
            clientCertPolicyRow_.Hint = null;
         caFieldRow_.Visibility = asked ? Visibility.Visible : Visibility.Collapsed;
      }

      // ---- client certificate validation ----

      /// <summary>
      /// The combinations PersistentTCPIPPort::SaveObject refuses, checked while
      /// the user is still choosing. The wording explains the refusal rather than
      /// just naming it, because each of these is a configuration that LOOKS like
      /// security and enforces nothing. Must stay in step with that file.
      /// </summary>
      private string ClientCertificateValidationError()
      {
         int policy = DialogFields.ComboValue(clientCertPolicy_);
         if (policy == 0)
            return null;

         int security = DialogFields.ComboValue(security_);

         // A client certificate is only ever exchanged during a TLS handshake,
         // so on a plaintext port the policy could never run.
         if (security == 0)
            return L("Client certificates can only be requested or required on a port that uses SSL/TLS or STARTTLS. On a port with no connection security there is no TLS handshake, so no client would ever be asked for a certificate.");

         // Without trust anchors, Require rejects every client (nothing chains
         // to an empty CA set) and Request verifies nothing while looking
         // configured.
         if (clientCertCaFile_.Text.Trim().Length == 0)
            return L("A CA certificate bundle file must be specified when client certificates are requested or required. Without one, Require would reject every client and Request would verify nothing.");

         // On an optional-STARTTLS port a client that never issues STARTTLS is
         // never asked for a certificate at all, so Require there is a lock on
         // an open door.
         if (policy == 2 && security == 2)
            return L("Require cannot be combined with STARTTLS (optional): a client that simply never issues STARTTLS is never asked for a certificate, so the requirement would not be enforced. Use SSL/TLS or STARTTLS (required).");

         return null;
      }

      private void UpdateClientCertificateValidation()
      {
         string error = ClientCertificateValidationError();
         if (error == null)
            clientCertNotice_.Hide();
         else
            clientCertNotice_.Show(StatusLevel.Critical, F("Cannot save: {0}", error));
      }

      /// <summary>
      /// A caption and its editor as one row, which names the editor to UI
      /// Automation. This dialog binds a network port and once announced itself
      /// to a screen reader as "combo box, edit, edit, combo box, combo box".
      /// </summary>
      private static FieldRow Label(string text, FrameworkElement editor) => DialogFields.Field(text, editor);
   }
}
