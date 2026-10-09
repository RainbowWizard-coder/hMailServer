// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// eDiscovery, read-only: the cases, and for the one selected every row the
   /// audit trail holds for it - opened, searched (with the whole query), held,
   /// released, exported, downloaded and closed. Read over the REST API, GET
   /// /api/v1/discovery/cases and GET /api/v1/audit, because discovery has no COM
   /// surface; opening a case, searching, holding and exporting are the Control
   /// Deck's (docs/Discovery.md).
   ///
   /// The requests carry the user name and password this Control Panel signed in
   /// with, which the REST API answers to as COM does, so the page shows what the
   /// signed-in role may read and nothing more. What it cannot do is said where
   /// the table would be, never guessed around: a listener that is off, one bound
   /// to a loopback address of another machine, a certificate that does not name
   /// the address asked and, on the server itself, is not the certificate the
   /// listener serves either (the password is not sent past a certificate that
   /// cannot be checked), and an administrator with a second factor, whose
   /// one-time code the REST API asks for with every request made with the
   /// password.
   /// </summary>
   public class DiscoveryView : UserControl, IPageLifecycle
   {
      /// <summary>
      /// On the server itself, the certificate file the listener serves, which the
      /// certificate it presents may be held to when its name is not the address
      /// asked (RestSettings.TrustsCertificate); null otherwise.
      /// </summary>
      private static volatile string servedCertificateFile_;

      /// <summary>Redirects are not followed: a password is not sent to wherever
      /// a listener points it.</summary>
      private static readonly HttpClient http_ = new(new HttpClientHandler
      {
         AllowAutoRedirect = false,
         ServerCertificateCustomValidationCallback = (request, certificate, chain, errors)
            => RestSettingsClient.TrustsServedCertificate(servedCertificateFile_, certificate, errors)
      })
      {
         Timeout = TimeSpan.FromSeconds(15)
      };

      private readonly DataGrid cases_ = NewGrid_();
      private readonly DataGrid lines_ = NewGrid_();
      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.DocumentSearch24, Visibility = Visibility.Collapsed };

      private IReadOnlyList<DiscoveryCase> loaded_ = Array.Empty<DiscoveryCase>();
      private List<AuditRow> audit_ = new();
      private bool loading_;

      public DiscoveryView() => Build_();

      // Read on entry: a case opened in the Deck since the page was last shown
      // must be here when somebody comes back to it.
      public void OnEnter() => Reload_();

      public void OnLeave()
      {
      }

      /// <summary>The REST API's answer to one request.</summary>
      private sealed record Reply(int Status, string Body, bool SecondFactorRequired)
      {
         public bool Ok => Status >= 200 && Status < 300;
      }

      private void Status_(StatusLevel level, string text)
      {
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      private void Clear_()
      {
         loaded_ = Array.Empty<DiscoveryCase>();
         audit_ = new List<AuditRow>();
         cases_.ItemsSource = null;
         lines_.ItemsSource = null;
         empty_.Visibility = Visibility.Collapsed;
      }

      // async void because it is started from OnEnter and a button, and nothing
      // escapes it: every failure is a sentence on the page.
      private async void Reload_()
      {
         if (loading_)
            return;

         loading_ = true;
         status_.Visibility = Visibility.Collapsed;
         Uri endpoint = null;

         try
         {
            ServerSession session = ServerSession.Current;
            string authorization = session?.RestAuthorization();
            if (authorization == null)
            {
               Clear_();
               Status_(StatusLevel.Warning, L("Sign in to a server first."));
               return;
            }

            var ini = new IniFeatureStore();

            // The ACME certificate the listener falls back to when it has none
            // of its own: looked for on this machine's disk when it is the
            // server's, and not known from another machine.
            bool? acmeCertificate = null;
            string acmeFolder = null;
            if (ServerSession.IsLocalSession)
            {
               string dataFolder = ini.IniPath == null ? "" : ProfileApi.ReadString("Directories", "DataFolder", "", ini.IniPath, 4096); // no-loc
               acmeFolder = DiscoveryReport.AcmeCertificateFolder(ini.Read("AcmeCertificateDirectory", ""), dataFolder); // no-loc
               acmeCertificate = DiscoveryReport.AcmeCertificateIn(acmeFolder, System.IO.File.Exists);
            }

            string certificateFile = ini.Read("RestApiCertificateFile", ""); // no-loc
            endpoint = DiscoveryReport.Endpoint(
               ini.Read("RestApiPort", "0"), ini.Read("RestApiBindAddress", "127.0.0.1"), // no-loc
               certificateFile, ini.Read("RestApiPrivateKeyFile", ""), // no-loc
               acmeCertificate, ServerSession.IsLocalSession, session.Host, out string refusal);

            servedCertificateFile_ = ServerSession.IsLocalSession
               ? RestSettings.ServedCertificateFile(certificateFile, acmeFolder, acmeCertificate)
               : null;

            if (endpoint == null)
            {
               Clear_();
               Status_(StatusLevel.Warning, refusal);
               return;
            }

            Reply cases = await Get_(endpoint, "api/v1/discovery/cases", authorization); // no-loc
            if (!cases.Ok)
            {
               Clear_();
               Status_(StatusLevel.Critical, Refusal_(cases));
               return;
            }

            DiscoveryReport report = DiscoveryReport.Parse(cases.Body);
            if (report.Error != null)
            {
               Clear_();
               Status_(StatusLevel.Critical, F("The server's answer could not be read: {0}", report.Error));
               return;
            }

            // The audit route answers one object type a page; the newest thousand
            // of each is more than any case this page is for.
            var audit = new List<AuditRow>();
            string auditNote = null;
            foreach (string type in DiscoveryReport.AuditObjectTypes)
            {
               Reply reply = await Get_(endpoint, "api/v1/audit?object_type=" + Uri.EscapeDataString(type) + "&limit=1000", authorization); // no-loc
               if (reply.Status == 403)
               {
                  auditNote = L("The audit lines need view_audit as well, which this credential does not hold.");
                  break;
               }

               if (!reply.Ok)
               {
                  auditNote = Refusal_(reply);
                  break;
               }

               AuditTrailReport page = AuditTrailReport.Parse(reply.Body);
               if (page.Error != null)
               {
                  auditNote = F("The server's answer could not be read: {0}", page.Error);
                  break;
               }

               audit.AddRange(page.Entries);
            }

            loaded_ = report.Cases;
            audit_ = audit;
            cases_.ItemsSource = report.Cases.Select(CaseRow_).ToList();

            StatusText.Show(empty_, null, report.Cases.Count, null,
               L("No case has been opened. Cases are opened in the Control Deck."));

            if (auditNote != null)
               Status_(StatusLevel.Warning, auditNote);
            else if (report.Cases.Count > 0)
               Status_(StatusLevel.Information, Plural(report.Cases.Count,
                  "{0} case. Select it for its lines on the audit trail.",
                  "{0} cases. Select one for its lines on the audit trail."));

            if (report.Cases.Count > 0)
               cases_.SelectedIndex = 0;
            ShowLines_();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Clear_();

            // A refused certificate, a listener that is not there, a timeout: the
            // innermost message is the one that names the cause.
            Exception real = ex;
            while (real.InnerException != null)
               real = real.InnerException;

            Status_(StatusLevel.Critical, endpoint == null
               ? ServerSession.DescribeComError(ex)
               : F("The REST API at {0} could not be reached: {1}", endpoint, real.Message));
         }
         finally
         {
            loading_ = false;
         }
      }

      private static async Task<Reply> Get_(Uri endpoint, string path, string authorization)
      {
         using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(endpoint, path));
         request.Headers.TryAddWithoutValidation("Authorization", authorization); // no-loc
         using HttpResponseMessage response = await http_.SendAsync(request);
         string body = await response.Content.ReadAsStringAsync();

         // RestApiServer's 401 for an administrator whose second factor is
         // enrolled says so in this header (BuildUnauthorizedResponse_).
         bool secondFactor = response.Headers.TryGetValues("X-hMailServer-OTP", out IEnumerable<string> values) && // no-loc
                             values.Any(v => string.Equals(v.Trim(), "required", StringComparison.OrdinalIgnoreCase)); // no-loc

         return new Reply((int)response.StatusCode, body, secondFactor);
      }

      private static string Refusal_(Reply reply)
      {
         string sentence = DiscoveryReport.ErrorOf(reply.Body);

         if (reply.Status == 401 && reply.SecondFactorRequired)
            return L("The administrator has a second factor, and the REST API asks for a one-time code with every request made with the password, which this page does not send. Read the cases in the Control Deck.");

         if (reply.Status == 401)
            return Sentences(L("The REST API refused the credential this Control Panel signed in with."), sentence);

         return sentence.Length == 0
            ? F("The REST API answered {0}.", reply.Status)
            : F("The REST API answered {0}: {1}", reply.Status, sentence);
      }

      private void ShowLines_()
      {
         if (cases_.SelectedItem is not CaseRow row)
         {
            lines_.ItemsSource = null;
            return;
         }

         DiscoveryCase theCase = loaded_.FirstOrDefault(c => c.Id == row.Id);
         lines_.ItemsSource = DiscoveryReport.LinesOf(theCase, audit_).Select(LineRow_).ToList();
      }

      /// <summary>A case as the grid shows it: the server's words turned into the reader's.</summary>
      private sealed class CaseRow
      {
         public long Id { get; init; }
         public string Name { get; init; }
         public string Description { get; init; }
         public string State { get; init; }
         public string Opened { get; init; }
         public string OpenedBy { get; init; }
         public long Searches { get; init; }
         public long LiveHolds { get; init; }
         public long Exports { get; init; }
      }

      /// <summary>One of a case's rows on the audit trail, as the grid shows it.</summary>
      private sealed class LineRow
      {
         public string When { get; init; }
         public string Who { get; init; }
         public string Interface { get; init; }
         public string Object { get; init; }
         public string Action { get; init; }
         public string Detail { get; init; }
      }

      private static string When_(long seconds)
         => seconds > 0 ? DateTimeOffset.FromUnixTimeSeconds(seconds).ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss") : "—"; // no-loc

      private static CaseRow CaseRow_(DiscoveryCase c) => new()
      {
         Id = c.Id,
         Name = c.Name,
         Description = c.Description,
         State = StateWord_(c.State),
         Opened = When_(c.Created),
         OpenedBy = c.CreatedBy,
         Searches = c.SearchCount,
         LiveHolds = c.LiveHolds,
         Exports = c.ExportCount
      };

      private static LineRow LineRow_(AuditRow row) => new()
      {
         When = When_(row.Time),
         Who = row.Actor,
         Interface = row.Interface,
         Object = ObjectWord_(row.ObjectType),
         Action = ActionWord_(row.Action),
         Detail = row.Detail
      };

      // The server's words, as the reader's. What is not known is shown as the
      // server wrote it rather than hidden.
      private static string StateWord_(string state) => state switch
      {
         "open" => L("Open"),
         "closed" => L("Closed"),
         _ => state
      };

      private static string ObjectWord_(string objectType) => objectType switch
      {
         "discovery case" => L("Case"), // no-loc
         "discovery search" => L("Search"), // no-loc
         "discovery hold" => L("Hold"), // no-loc
         "discovery export" => L("Export"), // no-loc
         _ => objectType
      };

      private static string ActionWord_(string action) => action switch
      {
         "created" => L("Created"),
         "updated" => L("Updated"),
         "downloaded" => L("Downloaded"),
         _ => action
      };

      // No MinHeight of the grid's own: the card around each is floored, and
      // measured at the height it is given, by the page's FillPanel.
      private static DataGrid NewGrid_() => new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single
      };

      private void Build_()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         var refresh = new Wpf.Ui.Controls.Button { Content = L("_Refresh"), MinWidth = 110 };
         AutomationProperties.SetName(refresh, L("Read the cases again"));
         refresh.Click += (_, _) => Reload_();

         root.Children.Add(new PageHeader
         {
            Title = L("Discovery"),
            Subtitle = L("The eDiscovery cases, read-only: each case's searches, holds and exports, and every line the audit trail holds for it. A case is opened, searched, held and exported in the Control Deck."),
            Actions = refresh
         });

         // The IP ranges shape, with two lists: the cases and the selected case's
         // lines share the height the notice leaves in the window, equally, each
         // down to a floor of three rows, and below that the page scrolls. The
         // two were a grid's star rows under rows sized to their content, which
         // in the narrowest window (760 by 520) left the lines no room for a row.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         parts.Children.Add(status_);

         cases_.Columns.Add(Text_(L("Name"), nameof(CaseRow.Name), new DataGridLength(1, DataGridLengthUnitType.Star)));
         cases_.Columns.Add(Text_(L("State"), nameof(CaseRow.State), DataGridLength.Auto));
         cases_.Columns.Add(Text_(L("Opened"), nameof(CaseRow.Opened), DataGridLength.Auto));
         cases_.Columns.Add(Text_(L("Opened by"), nameof(CaseRow.OpenedBy), DataGridLength.Auto));
         cases_.Columns.Add(Text_(L("Searches"), nameof(CaseRow.Searches), DataGridLength.Auto));
         cases_.Columns.Add(Text_(L("Live holds"), nameof(CaseRow.LiveHolds), DataGridLength.Auto));
         cases_.Columns.Add(Text_(L("Exports"), nameof(CaseRow.Exports), DataGridLength.Auto));
         cases_.RowStyle = TipStyle_(nameof(CaseRow.Description));
         cases_.SelectionChanged += (_, _) => ShowLines_();
         AutomationProperties.SetName(cases_, L("Discovery cases"));

         var host = new Grid();
         host.Children.Add(cases_);
         host.Children.Add(empty_);
         var casesCard = new Card { Padding = new Thickness(8), Content = host };
         parts.Children.Add(FillPanel.Filling(casesCard, FillPanel.ListCardFloor));

         lines_.Columns.Add(Text_(L("When"), nameof(LineRow.When), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Who"), nameof(LineRow.Who), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Interface"), nameof(LineRow.Interface), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Object"), nameof(LineRow.Object), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Action"), nameof(LineRow.Action), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Detail"), nameof(LineRow.Detail), new DataGridLength(1, DataGridLengthUnitType.Star)));
         lines_.RowStyle = TipStyle_(nameof(LineRow.Detail));
         AutomationProperties.SetName(lines_, L("The case's lines on the audit trail"));

         var linesCard = new Card
         {
            Title = L("The case's lines on the audit trail"),
            Padding = new Thickness(8),
            Margin = new Thickness(0, 12, 0, 0),
            Content = lines_
         };
         parts.Children.Add(FillPanel.Filling(linesCard, FillPanel.TitledListCardFloor));

         Content = root;
      }

      // The whole of a long value on the row's tip: the column cuts it.
      private static Style TipStyle_(string path)
      {
         var style = new Style(typeof(DataGridRow));
         if (Application.Current?.TryFindResource(typeof(DataGridRow)) is Style appRowStyle)
            style.BasedOn = appRowStyle;
         style.Setters.Add(new Setter(ToolTipProperty, new System.Windows.Data.Binding(path)));
         return style;
      }

      private static DataGridTextColumn Text_(string header, string path, DataGridLength width)
         => new() { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width };
   }
}
