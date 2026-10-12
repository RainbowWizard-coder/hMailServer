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

      // Where the cases are opened, searched, held and exported (review finding F281):
      // read again with the cases, since the REST API may have been turned on since.
      private readonly Wpf.Ui.Controls.Button deck_ = ControlDeckLink.Button(L("Open the Control _Deck"));

      private IReadOnlyList<DiscoveryCase> loaded_ = Array.Empty<DiscoveryCase>();
      private List<AuditRow> audit_ = new();
      private bool loading_;

      // Whether every object type's audit lines were read. Where a read was
      // refused or could not be read (Reload_'s auditNote), a case with no line
      // in audit_ may have lines the page could not read, and the lines' card
      // says so rather than that the trail holds none (found re-checking
      // 5a3a76267).
      private bool auditComplete_;

      // While the REST API is asked, which can take up to eight seconds, over the
      // cases' place: the page showed nothing at all meanwhile (review finding F291).
      private readonly LoadingState reading_ = new() { Text = L("Reading the cases…"), Visibility = Visibility.Collapsed };

      // In the lines' place while no case is selected, or the one selected has
      // none: the lines' card showed its bare column headers (F291).
      private readonly EmptyState noLines_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.DocumentBulletList24, Visibility = Visibility.Collapsed };

      // The card of the selected case's lines, hidden while there are no cases to select.
      private Card linesCard_;

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
         auditComplete_ = false;
         cases_.ItemsSource = null;
         lines_.ItemsSource = null;
         empty_.Visibility = Visibility.Collapsed;
         noLines_.Visibility = Visibility.Collapsed;
         linesCard_.Visibility = Visibility.Collapsed;
      }

      // async void because it is started from OnEnter and a button, and nothing
      // escapes it: every failure is a sentence on the page.
      private async void Reload_()
      {
         if (loading_)
            return;

         loading_ = true;
         status_.Visibility = Visibility.Collapsed;
         ControlDeckLink.Update(deck_);
         Uri endpoint = null;
         Reading_(true);

         try
         {
            ServerSession session = ServerSession.Current;
            string authorization = session?.RestAuthorization();
            // A session that holds no credential the REST API takes is told so, as the
            // settings pages' REST rows are (RestSettingsClient): "Sign in to a server
            // first." was said here under the shell's Connected pill. Only a page with
            // no session at all says that.
            if (authorization == null)
            {
               Clear_();
               Status_(StatusLevel.Warning, session == null
                  ? L("Sign in to a server first.")
                  : L("This session holds no password the REST API can sign in with. Sign in again with the administrator's user name and password."));
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

            Reading_(false);
            Present_(report.Cases, audit, auditNote);
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
            Reading_(false);
            loading_ = false;
         }
      }

      /// <summary>
      /// The ring and "Reading the cases…" in the cases' place while the REST
      /// API is asked, the grid's rows, its empty state and the lines of the last
      /// read out of the way; gone again whatever the read came to.
      /// </summary>
      private void Reading_(bool reading)
      {
         reading_.Visibility = reading ? Visibility.Visible : Visibility.Collapsed;
         if (reading)
         {
            cases_.ItemsSource = null;
            empty_.Visibility = Visibility.Collapsed;
            linesCard_.Visibility = Visibility.Collapsed;
         }
      }

      /// <summary>
      /// The cases and their lines as read: the first case selected, its lines
      /// under it, and the lines' card hidden while there is no case to select.
      /// </summary>
      private void Present_(IReadOnlyList<DiscoveryCase> cases, List<AuditRow> audit, string auditNote)
      {
         loaded_ = cases ?? Array.Empty<DiscoveryCase>();
         audit_ = audit ?? new List<AuditRow>();
         auditComplete_ = auditNote == null;
         openedColumn_.Header = OpenedHeader_();
         whenColumn_.Header = AuditTrailView.WhenHeader();
         cases_.ItemsSource = loaded_.Select(CaseRow_).ToList();

         StatusText.Show(empty_, null, loaded_.Count, null,
            L("No case has been opened. Cases are opened in the Control Deck."));
         linesCard_.Visibility = loaded_.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

         if (auditNote != null)
            Status_(StatusLevel.Warning, auditNote);
         else if (loaded_.Count > 0)
            Status_(StatusLevel.Information, Plural(loaded_.Count,
               "{0} case. Select it for its lines on the audit trail.",
               "{0} cases. Select one for its lines on the audit trail."));

         if (loaded_.Count > 0)
            cases_.SelectedIndex = 0;
         ShowLines_();
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

      /// <summary>
      /// The selected case's lines, or in their place what is missing: a case to
      /// select, or any line naming the one selected. The card showed its bare
      /// column headers in both (review finding F291). Where the audit trail was
      /// not read whole - a 403 for want of view_audit, another refusal, an
      /// answer that could not be read, which the notice above says - no line
      /// for the case is not a fact the page has, and the card says the lines
      /// could not be read, where it said the trail held none under the notice
      /// that says it was not read.
      /// </summary>
      private void ShowLines_()
      {
         if (cases_.SelectedItem is not CaseRow row)
         {
            lines_.ItemsSource = null;
            noLines_.Text = L("Select a case above to see its lines on the audit trail.");
            noLines_.Visibility = Visibility.Visible;
            return;
         }

         DiscoveryCase theCase = loaded_.FirstOrDefault(c => c.Id == row.Id);
         List<LineRow> lines = DiscoveryReport.LinesOf(theCase, audit_).Select(LineRow_).ToList();
         lines_.ItemsSource = lines;
         noLines_.Text = auditComplete_
            ? L("The audit trail holds no line for this case.")
            : L("This case's lines could not be read; the notice above says why.");
         noLines_.Visibility = lines.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
      }

      /// <summary>A case as the grid shows it: the server's words turned into the reader's.</summary>
      private sealed class CaseRow
      {
         public long Id { get; init; }
         public string Name { get; init; }
         public string Description { get; init; }
         public string State { get; init; }
         public string Opened { get; init; }

         // The Unix times, to sort Opened and When by: their cells are in the reader's format.
         public long OpenedTime { get; init; }
         public string OpenedBy { get; init; }
         public long Searches { get; init; }
         public long LiveHolds { get; init; }
         public long Exports { get; init; }

         // The case and its state, then the rest with their columns' names.
         public override string ToString() => RowSpeech.Of(Name, State,
            RowSpeech.Labelled(L("Opened"), Opened), RowSpeech.Labelled(L("Opened by"), OpenedBy),
            RowSpeech.Labelled(L("Searches"), Searches.ToString(System.Globalization.CultureInfo.CurrentCulture)),
            RowSpeech.Labelled(L("Live holds"), LiveHolds.ToString(System.Globalization.CultureInfo.CurrentCulture)),
            RowSpeech.Labelled(L("Exports"), Exports.ToString(System.Globalization.CultureInfo.CurrentCulture)));
      }

      /// <summary>One of a case's rows on the audit trail, as the grid shows it.</summary>
      private sealed class LineRow
      {
         public string When { get; init; }
         public long Time { get; init; }
         public string Who { get; init; }
         public string Interface { get; init; }
         public string Object { get; init; }
         public string Action { get; init; }
         public string Detail { get; init; }

         // When, who, and what was done to what, then the detail.
         public override string ToString() => RowSpeech.Of(When, Who, Action, Object, Detail);
      }

      // In the reader's short date and long time, this computer's clock, which the
      // columns' headers name, as the audit trail's are (review finding F321).
      private static string When_(long seconds)
         => ServerTimeText.FromUnix(seconds) is DateTime when ? ServerTimeText.CellWithSeconds(when) : "—"; // no-loc

      private static CaseRow CaseRow_(DiscoveryCase c) => new()
      {
         Id = c.Id,
         Name = c.Name,
         Description = c.Description,
         State = StateWord_(c.State),
         Opened = When_(c.Created),
         OpenedTime = c.Created,
         OpenedBy = c.CreatedBy,
         Searches = c.SearchCount,
         LiveHolds = c.LiveHolds,
         Exports = c.ExportCount
      };

      private DataGridTextColumn openedColumn_;
      private DataGridTextColumn whenColumn_;

      /// <summary>The Opened column's header, naming the clock its times are shown in, as the audit trail's When does.</summary>
      private static string OpenedHeader_()
         => ServerSession.IsLocalSession ? L("Opened (server time)") : L("Opened (this computer's time)");

      private static LineRow LineRow_(AuditRow row) => new()
      {
         When = When_(row.Time),
         Time = row.Time,
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
         ControlHelp.Explain(refresh, L("Read the cases again"));
         refresh.Click += (_, _) => Reload_();
         deck_.Margin = new Thickness(0, 0, 8, 0);

         root.Children.Add(new PageHeader
         {
            PageKey = "discovery",
            Subtitle = L("The eDiscovery cases, read-only: each case's searches, holds and exports, and every line the audit trail holds for it. A case is opened, searched, held and exported in the Control Deck."),
            Actions = new StackPanel { Orientation = Orientation.Horizontal, Children = { deck_, refresh } }
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

         // The name and the detail share what the short columns leave, over a floor,
         // cut short with an ellipsis: with no floor the name was given nothing in
         // the smallest window, "Co" for Contoso dispute under a header cut to "N...".
         cases_.Columns.Add(Shared_(L("Name"), nameof(CaseRow.Name), 160));
         cases_.Columns.Add(Text_(L("State"), nameof(CaseRow.State), DataGridLength.Auto));
         openedColumn_ = Text_(OpenedHeader_(), nameof(CaseRow.Opened), DataGridLength.Auto);
         openedColumn_.SortMemberPath = nameof(CaseRow.OpenedTime);
         cases_.Columns.Add(openedColumn_);
         cases_.Columns.Add(Text_(L("Opened by"), nameof(CaseRow.OpenedBy), DataGridLength.Auto));
         // The counts right-aligned, as every count in a grid is (GridStyles.Number):
         // they were left-aligned text (review finding F291).
         cases_.Columns.Add(Count_(L("Searches"), nameof(CaseRow.Searches)));
         cases_.Columns.Add(Count_(L("Live holds"), nameof(CaseRow.LiveHolds)));
         cases_.Columns.Add(Count_(L("Exports"), nameof(CaseRow.Exports)));
         cases_.RowStyle = TipStyle_(nameof(CaseRow.Description));
         cases_.SelectionChanged += (_, _) => ShowLines_();
         AutomationProperties.SetName(cases_, L("Discovery cases"));

         // Each grid's three rows on a FillPanel of its own and its card floored
         // at what it holds, as the domain lists are: the cases' empty state and
         // the ring while they are read, and the lines' empty state, laid over the
         // rows, raise the card's floor where they need more. The two cards were
         // floored at the list cards' height and shared the room so that in the
         // smallest window the lines' grid showed no row at all (F291).
         var caseRows = new FillPanel();
         caseRows.Children.Add(FillPanel.Filling(cases_, FillPanel.ListRowsFloor));
         var host = new Grid();
         host.Children.Add(caseRows);
         host.Children.Add(empty_);
         host.Children.Add(reading_);
         EmptyStateUnderHeaders.Keep(empty_, cases_);
         var casesCard = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };
         parts.Children.Add(FillPanel.Filling(casesCard, 0));

         whenColumn_ = Text_(AuditTrailView.WhenHeader(), nameof(LineRow.When), DataGridLength.Auto);
         whenColumn_.SortMemberPath = nameof(LineRow.Time);
         lines_.Columns.Add(whenColumn_);
         lines_.Columns.Add(Text_(L("Who"), nameof(LineRow.Who), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Interface"), nameof(LineRow.Interface), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Object"), nameof(LineRow.Object), DataGridLength.Auto));
         lines_.Columns.Add(Text_(L("Action"), nameof(LineRow.Action), DataGridLength.Auto));
         lines_.Columns.Add(Shared_(L("Detail"), nameof(LineRow.Detail), 140));
         lines_.RowStyle = TipStyle_(nameof(LineRow.Detail));
         AutomationProperties.SetName(lines_, L("The case's lines on the audit trail"));

         var lineRows = new FillPanel();
         lineRows.Children.Add(FillPanel.Filling(lines_, FillPanel.ListRowsFloor));
         var linesHost = new Grid();
         linesHost.Children.Add(lineRows);
         linesHost.Children.Add(noLines_);
         EmptyStateUnderHeaders.Keep(noLines_, lines_);

         // The card padding every titled card has (AppCardPadding, the Card's own),
         // so that its title stands where the other pages' card titles do: at
         // eight pixels it stood twelve to their left (F291). Shown once there are
         // cases to choose from.
         linesCard_ = new Card
         {
            Title = L("The case's lines on the audit trail"),
            Margin = new Thickness(0, 12, 0, 0),
            Content = linesHost,
            Visibility = Visibility.Collapsed
         };
         parts.Children.Add(FillPanel.Filling(linesCard_, 0));

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

      /// <summary>A column that takes the width the others leave, over a floor of so many pixels or its header's width, its cells cut short with an ellipsis.</summary>
      private static DataGridTextColumn Shared_(string header, string path, double floor)
      {
         DataGridTextColumn column = GridStyles.Trimmed(Text_(header, path, new DataGridLength(1, DataGridLengthUnitType.Star)));
         column.MinWidth = Math.Max(floor, GridStyles.HeaderWidth(header));
         return column;
      }

      /// <summary>A column of counts: as wide as its widest and its header, the digits and the header right-aligned.</summary>
      private static DataGridTextColumn Count_(string header, string path)
      {
         DataGridTextColumn column = Text_(header, path, DataGridLength.Auto);
         GridStyles.Number(column);
         return column;
      }
   }
}
