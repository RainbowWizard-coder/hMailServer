// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using TextBox = Wpf.Ui.Controls.TextBox;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The parts the Monitoring &amp; alerts page and the Protocols page's ManageSieve tab
   /// brought with them from the INI-only view (FeatureSettingsView) when review finding
   /// F273's cards moved (the owner's decision of 12 October 2026): a whole number held
   /// to the server's range as typed, a signpost to the page that now holds a setting,
   /// the metrics listener's five warnings and ManageSieve's TLS state - each as that
   /// view had it, so that nothing a reader was told there goes unsaid here.
   /// </summary>
   public partial class ServerSettingsView
   {
      /// <summary>
      /// A whole number kept under a [Settings] key as the INI-only view writes one
      /// (FeatureSettingsView.NumberSetting; review findings F220 and F139): a text box
      /// 180 pixels wide whose text stays as typed until a save asks whether it is a
      /// whole number in the range the server reads the key in, and says on the field
      /// when it is not (<see cref="Save_"/> asks every one before it writes any) - where
      /// a NumberBox rounded "1.5", put back the value before for "30s" and pulled a value
      /// past its bound to the bound without a word. Asked of the server when there is a
      /// session (IniFeatureStore.Read), of the file only when there is none.
      /// </summary>
      private sealed class IniRangeNumber : ComSetting, IIniSetting
      {
         public IniFeatureStore IniStore;
         public string Default = "0";
         public string Placeholder = "";

         /// <summary>The lowest and the highest value the server reads the key in.</summary>
         public long Minimum;
         public long Maximum = int.MaxValue;

         private TextBox box_;
         private string loaded_;

         public override bool WantsInitialValue => false;

         /// <summary>The setting's box, once the page has built it.</summary>
         public TextBox Box => box_;

         protected override FrameworkElement BuildEditor(object value)
         {
            string raw = ((IniStore != null && IniStore.SettingsReachable ? IniStore.ReadNumber(Path, Default) : Default) ?? "").Trim();

            box_ = TextEditor(raw, ExamplePlaceholder.Shown(Placeholder));
            box_.Width = NumberWidth;
            box_.HorizontalAlignment = HorizontalAlignment.Left;

            // No input method: with a Japanese or Chinese one on, a digit typed arrives
            // as a full-width one, which the server does not read as a digit.
            System.Windows.Input.InputMethod.SetIsInputMethodEnabled(box_, false);

            loaded_ = SettingNumberRange.TryParse(raw, out long current) ? current.ToString(CultureInfo.InvariantCulture) : raw;
            box_.TextChanged += (s, e) =>
            {
               if (Field != null)
                  Field.Error = null;
            };
            return box_;
         }

         /// <summary>The number as the store spells it, read from the text as typed, or the text itself when it is no whole number.</summary>
         public string LiveValue
         {
            get
            {
               if (box_ == null)
                  return null;

               string text = (box_.Text ?? "").Trim();
               return SettingNumberRange.TryParse(text, out long number) ? number.ToString(CultureInfo.InvariantCulture) : text;
            }
         }

         public override object ReadEditor() => LiveValue ?? "";

         /// <summary>The row the page put round the box, where a refusal is said.</summary>
         public Scaffold.FieldRow Field => box_?.Parent as Scaffold.FieldRow;

         /// <summary>
         /// Why a save would refuse the row as it stands, or null: an edited value that is
         /// no whole number in the range. A value the store holds is shown as it is and,
         /// unchanged, neither refused nor written.
         /// </summary>
         public string Refusal
            => box_ == null || string.Equals(LiveValue, loaded_, StringComparison.Ordinal)
               ? null
               : SettingNumberRange.Error(LiveValue, Minimum, Maximum, false);

         public bool SaveToIni()
         {
            if (WithheldEntry != null)
               return SaveWithheld(IniStore, Path);

            if (IniStore == null || box_ == null)
               return false;

            string value = LiveValue;
            if (string.Equals(value, loaded_, StringComparison.Ordinal))
               return false;

            IniStore.Write(Path, value);
            loaded_ = value;
            return true;
         }
      }

      /// <summary>
      /// A signpost among a card's rows: what is on another page, where - its group and
      /// title, and its tab when the page has one - and a button that opens it there, as
      /// the INI-only view's ElsewhereSetting draws one. Writes nothing.
      /// </summary>
      private sealed class ElsewhereRow : ComSetting, INotPersisted
      {
         /// <summary>The navigation key of the page that holds the setting.</summary>
         public string Page;

         /// <summary>The English header of the page's tab that holds it, marked with N(), or null.</summary>
         public string Tab;

         public override bool WantsInitialValue => false;

         protected override bool CaptionOnRow => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            string destination = NavigationMap.LocationOf(Page);
            if (string.IsNullOrEmpty(destination))
               destination = NavigationMap.TitleOf(Page);
            if (!string.IsNullOrEmpty(Tab))
               destination += " > " + L(Tab);

            var row = new StackPanel { Orientation = Orientation.Horizontal };

            var text = new TextBlock
            {
               Text = Label + "  →  " + destination,
               VerticalAlignment = VerticalAlignment.Center,
               TextWrapping = TextWrapping.Wrap,
               MaxWidth = 420
            };
            text.SetResourceReference(StyleProperty, "TextBody");
            row.Children.Add(text);

            // The button names the page it opens, with no ellipsis: it opens a page and
            // asks nothing (review finding F305).
            string pageTitle = L(NavigationMap.TitleOf(Page));
            var button = new Wpf.Ui.Controls.Button   // per setting: no access key, the rows are reached with the arrow keys
            {
               Content = F("Open {0}", pageTitle),
               Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
               VerticalAlignment = VerticalAlignment.Center,
               ToolTip = F("Open {0}", destination)
            };
            ButtonStyles.CompactPageLink(button);
            AutomationProperties.SetName(button, F("Open {0}, which now has {1}", pageTitle, Label));
            SetAid(button, "elsewhere-" + Page);
            string page = Page, tab = Tab;
            button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateToTab(page, tab);
            row.Children.Add(button);
            return row;
         }

         public override object ReadEditor() => null;

         public override void Write(object owner, string property)
         {
         }
      }

      // ---- the metrics listener's warnings -----------------------------------------

      /// <summary>The rows the metrics listener's warnings read, and the five notices they fill.</summary>
      private sealed class MetricsRows
      {
         public IniRangeNumber Port;
         public IniText Bind, User, Certificate, PrivateKey;
         public IniSecret Token, Password;
         public FollowingNoticeRow BindInvalid, NeedsCredential, HalfBasic, HalfTls, ClearTextCredential;
      }

      /// <summary>
      /// Follows the metrics listener's fields with the five warnings the server's
      /// MetricsServer::Start gives cause for (verified there): an invalid bind address
      /// is refused outright; a non-loopback bind without a credential serves 503 on
      /// /metrics; half an HTTP Basic pair and half a TLS pair are each discarded with a
      /// log line; and a credential without TLS crosses the network in clear text. Each
      /// notice shows while its condition holds, as the fields are edited.
      /// </summary>
      private void WireMetricsWarnings_(MetricsRows rows)
      {
         var notices = new[] { rows.BindInvalid, rows.NeedsCredential, rows.HalfBasic, rows.HalfTls, rows.ClearTextCredential };
         if (notices.Any(notice => notice?.Notice == null) || rows.Port?.Box == null)
         {
            foreach (FollowingNoticeRow notice in notices.Where(notice => notice?.Notice != null))
            {
               Shown_(notice).Visibility = Visibility.Collapsed;
               TrimCardEnd_(notice);
            }
            return;
         }

         string[] ids = { "MetricsBindAddressInvalidWarning", "MetricsNeedsCredentialWarning", "MetricsHalfBasicWarning", "MetricsHalfTlsWarning", "MetricsClearTextCredentialWarning" };
         for (int i = 0; i < notices.Length; i++)
            AutomationProperties.SetAutomationId(notices[i].Notice, ids[i]);

         static string Text_(IniText row, string fallback) => row?.Box == null ? fallback : (row.Box.Text ?? "").Trim();

         void Refresh()
         {
            bool on = int.TryParse(rows.Port.LiveValue, NumberStyles.Integer, CultureInfo.InvariantCulture, out int port) && port > 0;
            string bind = Text_(rows.Bind, "127.0.0.1");
            bool bindParsed = TryParseIpv4_(bind, out byte firstOctet);
            bool loopback = bindParsed && firstOctet == 127;
            bool tokenSet = rows.Token?.IsConfigured == true;
            bool userSet = Text_(rows.User, "").Length > 0;
            bool passSet = rows.Password?.IsConfigured == true;
            bool basicSet = userSet && passSet;
            bool certSet = Text_(rows.Certificate, "").Length > 0;
            bool keySet = Text_(rows.PrivateKey, "").Length > 0;

            Say_(rows.BindInvalid, on && !bindParsed,
               L("The metrics bind address is not a plain IPv4 address (host names and IPv6 are not accepted here), so the metrics listener will not start at all."));
            Say_(rows.NeedsCredential, on && bindParsed && !loopback && !tokenSet && !basicSet,
               L("The bind address is not a loopback address and no credential is set, so /metrics answers 503 (Service Unavailable) to every scrape. Set a bearer token, or both HTTP Basic fields, or bind to 127.0.0.1. The health probes keep answering either way."));
            Say_(rows.HalfBasic, on && userSet != passSet,
               F("HTTP Basic needs both the user name and the password. Only {0} is set, so Basic authentication is not enabled - the server logs this and behaves as if neither were set.",
                  userSet ? L("the user name") : L("the password")));
            Say_(rows.HalfTls, on && certSet != keySet,
               F("TLS for the metrics listener needs both the certificate and the private key. Only {0} is set, so TLS is not enabled and scrapes stay plain HTTP.",
                  certSet ? L("the certificate") : L("the private key")));
            Say_(rows.ClearTextCredential, on && bindParsed && !loopback && (tokenSet || basicSet) && !(certSet && keySet),
               L("A credential is set but the listener has no TLS, so the credential crosses the network in clear text on every scrape and can be replayed by anyone on the path. Set the certificate and key files, or bind to 127.0.0.1. The server still starts - it logs this same warning."));
            TrimCardEnd_(rows.ClearTextCredential);
         }

         rows.Port.Box.TextChanged += (s, e) => Refresh();
         foreach (IniText text in new[] { rows.Bind, rows.User, rows.Certificate, rows.PrivateKey }.Where(row => row?.Box != null))
            text.Box.TextChanged += (s, e) => Refresh();
         foreach (IniSecret secret in new[] { rows.Token, rows.Password }.Where(row => row?.Box != null))
         {
            secret.Box.PasswordChanged += (s, e) => Refresh();
            secret.RemoveBox.Checked += (s, e) => Refresh();
            secret.RemoveBox.Unchecked += (s, e) => Refresh();
         }

         Refresh();
      }

      /// <summary>The row a following notice stands in: its FieldRow, or the notice where it has none.</summary>
      private static FrameworkElement Shown_(FollowingNoticeRow notice)
         => notice.Notice.Parent as Scaffold.FieldRow ?? (FrameworkElement)notice.Notice;

      /// <summary>
      /// The card's last row on screen gives up its gap, as BuildUi gives the last row
      /// its own: the warnings at the foot of a card come and go as the fields are
      /// edited, and the field above a collapsed one kept its gap inside the card's
      /// padding, which made the card taller than its fields by a field's gap.
      /// </summary>
      private static void TrimCardEnd_(FollowingNoticeRow notice)
      {
         if (Shown_(notice).Parent is not Panel fields)
            return;

         FrameworkElement last = fields.Children.OfType<FrameworkElement>().LastOrDefault(row => row.Visibility == Visibility.Visible);
         foreach (Scaffold.FieldRow row in fields.Children.OfType<Scaffold.FieldRow>())
         {
            if (ReferenceEquals(row, last))
               row.Margin = new Thickness(0);
            else
               row.ClearValue(MarginProperty);
         }
      }

      /// <summary>Shows a warning's notice with its words while it holds, and collapses its row while it does not.</summary>
      private static void Say_(FollowingNoticeRow notice, bool holds, string text, StatusLevel level = StatusLevel.Warning)
      {
         notice.Notice.Level = level;
         notice.Notice.Text = holds ? text : null;
         Shown_(notice).Visibility = holds ? Visibility.Visible : Visibility.Collapsed;
      }

      /// <summary>
      /// Strict dotted-quad parse, mirroring the inet_pton call in MetricsServer::Start -
      /// IPAddress.TryParse would accept "127.1" and IPv6, both of which the listener
      /// itself refuses.
      /// </summary>
      private static bool TryParseIpv4_(string text, out byte firstOctet)
      {
         firstOctet = 0;
         if (string.IsNullOrWhiteSpace(text))
            return false;

         string[] parts = text.Trim().Split('.');
         if (parts.Length != 4)
            return false;

         var octets = new byte[4];
         for (int i = 0; i < 4; i++)
         {
            if (parts[i].Length == 0 || parts[i].Length > 3 || !parts[i].All(char.IsDigit))
               return false;
            if (!byte.TryParse(parts[i], out octets[i]))
               return false;
         }

         firstOctet = octets[0];
         return true;
      }

      // ---- ManageSieve's TLS state ----------------------------------------------------

      /// <summary>
      /// Follows the ManageSieve tab's port and bind address with whether the listener
      /// will offer STARTTLS (<see cref="ManageSieveTlsState_"/>), on the card under them.
      /// </summary>
      private void WireManageSieveTlsState_(IniRangeNumber port, IniText bind, FollowingNoticeRow notice)
      {
         if (notice?.Notice == null)
            return;

         AutomationProperties.SetAutomationId(notice.Notice, "ManageSieveTlsState");
         if (port?.Box == null)
         {
            Shown_(notice).Visibility = Visibility.Collapsed;
            TrimCardEnd_(notice);
            return;
         }

         void Refresh()
         {
            bool on = int.TryParse(port.LiveValue, NumberStyles.Integer, CultureInfo.InvariantCulture, out int number) && number > 0;
            string address = bind?.Box == null ? "127.0.0.1" : (bind.Box.Text ?? "").Trim();
            (StatusLevel level, string text) = on ? ManageSieveTlsState_(address) : (StatusLevel.Information, null);
            Say_(notice, text != null, text, level);
            TrimCardEnd_(notice);
         }

         port.Box.TextChanged += (s, e) => Refresh();
         if (bind?.Box != null)
            bind.Box.TextChanged += (s, e) => Refresh();
         Refresh();
      }

      /// <summary>
      /// Whether the ManageSieve listener will actually offer STARTTLS, for a listener
      /// that is on and bound to <paramref name="bind"/>.
      ///
      /// It has no certificate setting of its own: ManageSieveServer::FindTlsCertificate_
      /// borrows one from a TLS-capable IMAP, POP3 or SMTP port, in that order, and when
      /// there is none it starts anyway and offers plain text only, saying so once in the
      /// application log. So an administrator can configure the port, save, restart,
      /// connect - and hand their mailbox password across the network in the clear, with
      /// nothing anywhere in the GUI having suggested otherwise.
      /// </summary>
      private (StatusLevel Level, string Text) ManageSieveTlsState_(string bind)
      {
         bool loopbackOnly = bind.StartsWith("127.", StringComparison.Ordinal) || bind == "::1";

         // Read once per build of the page, including when the answer is "could not
         // tell": two COM collection walks on every keystroke would retry for as long
         // as the session stayed broken.
         if (!manageSieveCertificateRead_)
         {
            manageSieveCertificate_ = FindManageSieveCertificateName_();
            manageSieveCertificateRead_ = true;
         }

         string certificate = manageSieveCertificate_;

         if (certificate == null)
            return (StatusLevel.Information, L("Whether STARTTLS will be offered could not be determined: the TCP/IP ports and certificates could not be read. This listener borrows its certificate from a TLS-capable IMAP, POP3 or SMTP port and has none of its own."));

         // Deliberately "expected to be", not "will be". Everything checkable from here
         // has been checked - the certificate row exists and names both files - but the
         // listener only truly gets STARTTLS if SslContextInitializer::InitServer can LOAD
         // them at start-up, and a file that has been moved, is unreadable by the service
         // account, or carries an encrypted key with no passphrase fails there.
         if (certificate.Length > 0)
            return (StatusLevel.Good, F("STARTTLS is expected to be offered, using the certificate '{0}' borrowed from a TLS-capable mailbox port - this listener has no certificate setting of its own, by design: the client editing Sieve filters is the client reading the mailbox, so the certificate it already trusts is the right one to present. Whether the files actually load is decided when the service starts; if they do not, the listener runs in plain text and says so in the application log, and the TLS certificates page checks the files themselves.", certificate));

         string missing = L("No TLS-capable IMAP, POP3 or SMTP port has a certificate assigned, so this listener will offer plain text only - ManageSieve authenticates with the mailbox password, which would then cross the network in the clear.");
         return loopbackOnly
            ? (StatusLevel.Information, Sentences(missing, F("It is bound to {0}, which keeps it on this machine - that is the supported way to run it without TLS. Assign a certificate to a mailbox port to offer STARTTLS.", bind)))
            : (StatusLevel.Warning, Sentences(missing, F("It is bound to {0}, so those passwords cross the network. Either assign a certificate to a TLS-capable mailbox port on the TCP/IP ports page, bind this listener to 127.0.0.1, or put it behind a TLS terminator.", bind)));
      }

      /// <summary>
      /// The name of the certificate ManageSieve would borrow: "" when no TLS-capable
      /// mailbox port has one, or null when the question could not be asked. Read once
      /// per build of the page (BuildUi clears it): two COM collection walks, and the
      /// state follows every keystroke.
      /// </summary>
      private string manageSieveCertificate_;
      private bool manageSieveCertificateRead_;

      private static string FindManageSieveCertificateName_()
      {
         // Usable, not merely referenced. FindTlsCertificate_ skips a port whose
         // SSLCertificateID resolves to no certificate row, and one whose certificate has
         // an empty certificate or private-key path - in both cases it KEEPS LOOKING. A
         // dangling id is the ordinary result of deleting a certificate that was in use
         // (the delete never clears portsslcertificateid), so matching on the id alone
         // once reported a certificate that no longer existed.
         var usableCertificateNames = new Dictionary<int, string>();

         dynamic certificates = null;
         dynamic ports = null;

         try
         {
            certificates = ServerSession.Current.Application.Settings.SSLCertificates;
            int certificateCount = (int)certificates.Count;
            for (int i = 0; i < certificateCount; i++)
            {
               dynamic certificate = certificates.Item[i];
               try
               {
                  string certificateFile = ((string)certificate.CertificateFile ?? "").Trim();
                  string privateKeyFile = ((string)certificate.PrivateKeyFile ?? "").Trim();

                  if (certificateFile.Length == 0 || privateKeyFile.Length == 0)
                     continue;

                  usableCertificateNames[(int)certificate.ID] = (string)certificate.Name ?? "";
               }
               finally
               {
                  ServerSession.Release((object)certificate);
               }
            }

            // Per protocol, the FIRST port the server would accept - which is not the
            // first port of that protocol: the server walks the ports in order and skips
            // the unusable ones.
            var certificateIdByProtocol = new Dictionary<int, int>();

            ports = ServerSession.Current.Application.Settings.TCPIPPorts;
            int portCount = (int)ports.Count;
            for (int i = 0; i < portCount; i++)
            {
               dynamic port = ports.Item[i];
               try
               {
                  int security = (int)port.ConnectionSecurity;
                  int certificateId = (int)port.SSLCertificateID;

                  // Only a port that will actually perform a handshake has a certificate
                  // to lend: security 0 is plaintext.
                  if (security <= 0 || certificateId <= 0)
                     continue;

                  if (!usableCertificateNames.ContainsKey(certificateId))
                     continue;

                  int protocol = (int)port.Protocol;
                  if (!certificateIdByProtocol.ContainsKey(protocol))
                     certificateIdByProtocol[protocol] = certificateId;
               }
               finally
               {
                  ServerSession.Release((object)port);
               }
            }

            // The first protocol, in the server's order of preference - IMAP, then POP3,
            // then SMTP (ManageSieveServer::FindTlsCertificate_) - that lends a certificate.
            int? lendingCertificateId = new[] { ServerSession.SessionImap, ServerSession.SessionPop3, ServerSession.SessionSmtp }
               .Where(certificateIdByProtocol.ContainsKey)
               .Select(protocol => (int?)certificateIdByProtocol[protocol])
               .FirstOrDefault();
            if (lendingCertificateId.HasValue)
            {
               string name = usableCertificateNames[lendingCertificateId.Value];
               return name.Length > 0 ? name : "#" + lendingCertificateId.Value;
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)ports);
            ServerSession.Release((object)certificates);
         }

         return "";
      }
   }
}
