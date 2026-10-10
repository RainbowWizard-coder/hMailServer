// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;

// The Control Panel has its own Typography (the type scale, in Services) and
// System.Windows.Documents declares one too. That import is needed here for Run and
// Inlines, so the reference is aliased to the one meant rather than dropping the import.
using Typography = hMailServer.ControlPanel.Services.Typography;
using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using FieldRow = hMailServer.ControlPanel.Views.Scaffold.FieldRow;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;
using EmptyState = hMailServer.ControlPanel.Views.Scaffold.EmptyState;

// This file needs System.IO (the DKIM key file is read to derive the public key) and
// System.Windows.Shapes (the status marks), and both declare a Path. The alias picks
// the drawing one; the file APIs used here (File) are unambiguous.
using Path = System.Windows.Shapes.Path;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The DNS records this server needs, per domain, each one exact and checkable.
   ///
   /// An administrator's DNS provider asks for a type, a host and a value; this page
   /// produces all three for SPF, DKIM, DMARC, MTA-STS and TLS-RPT, in separate
   /// selectable boxes with their own copy buttons, and a Check button per record
   /// that says whether what is actually published matches. The model is the DKIM
   /// rotation flow in <see cref="DomainDialog"/>: generate what can be generated,
   /// show exactly what to publish, verify it on the wire, and gate the dangerous
   /// next step (here: switching DKIM signing on) on that verification passing.
   ///
   /// Every generated value is read from the server's own configuration or code,
   /// never invented:
   ///
   ///   SPF      - the server only CHECKS SPF on incoming mail (SMTP/SPF/, an
   ///              implementation of RFC 7208; records beginning "v=spf1"). The suggested
   ///              record names this server's outbound identity: the HELO/EHLO host
   ///              name, which is Settings.HostName (Utilities::ComputerName falls
   ///              back to the Windows computer name when it is empty).
   ///   DKIM     - the record value is derived from the domain's configured private
   ///              key file, the same derivation DkimKeyGenerator uses when a key
   ///              is first made (the p= tag is the key's SubjectPublicKeyInfo).
   ///   DMARC    - the server's own evaluator (Common/AntiSpam/DMARC/DMARC.cpp)
   ///              queries _dmarc.&lt;domain&gt; and accepts records beginning
   ///              "v=DMARC1"; the starting policy offered is p=none.
   ///   MTA-STS  - the policy file is served by the server itself
   ///              (Common/Util/WebServicesServer.cpp, HandleMtaStsPolicy_): HTTPS
   ///              only per RFC 8461 section 3.3, for hosted active domains, built
   ///              from MtaStsPolicyMode / MtaStsPolicyMaxAge / MtaStsPolicyMx in
   ///              hMailServer.INI. There is no file to upload; what this page can
   ///              do is show what will be served, and fetch what IS served.
   ///   TLS-RPT  - the record solicits reports from OTHER servers to a mailbox of
   ///              the administrator's choosing. Separately, this server's own
   ///              report sending is inert until TlsRptFromAddress is set
   ///              (SMTP/TlsRptReporterTask.cpp discards each completed day unsent
   ///              while it is empty).
   ///   PTR      - not publishable by the administrator at all: the owner of the
   ///              IP address sets it. The page shows the HELO name the PTR should
   ///              agree with and says who to ask, and does not pretend to check
   ///              what it cannot (the server's public IP is not knowable from here).
   ///
   /// Honesty rules, in force everywhere on this page: a value that depends on
   /// facts this panel cannot see (the public IP, the administrator's other mail
   /// senders) is labelled as an assumption, the danger of copying it blindly is
   /// said at the copy button and not in a footnote, and a mismatch is never
   /// reported without showing what IS published - the administrator cannot fix a
   /// difference they cannot see. The four-way verdict (not published / published
   /// but different / published and correct / lookup failed) follows
   /// <see cref="DnsTxtLookup"/>, whose whole purpose is keeping those apart.
   ///
   /// DNS lookups and the MTA-STS policy fetch run off the UI thread; each blocks
   /// for the resolver's or the HTTP client's full timeout, and the missing-record
   /// case - the slowest - is the common one right after publishing.
   /// </summary>
   public class DnsRecordsView : UserControl, IPageLifecycle
   {
      private readonly ComboBox domainCombo_ = new() { MinWidth = 280, MaxWidth = 420, HorizontalAlignment = HorizontalAlignment.Left };

      // A read that failed is a warning in the flow of the page: the records
      // below are copied into somebody's DNS zone, so an incomplete read has to
      // be seen before they are.
      private readonly InlineNotice readFailure_ = new()
      {
         Level = StatusLevel.Warning,
         Visibility = Visibility.Collapsed
      };
      private readonly StackPanel body_ = new();
      private readonly TextBlock status_ = new();

      // How the domain stands after Check all records: one notice at the worst
      // level any check reached, a line per verdict (review finding F277).
      private readonly InlineNotice checkSummary_ = new() { Visibility = Visibility.Collapsed };
      private readonly Wpf.Ui.Controls.Button checkAll_ = new()
      {
         Content = L("Check _all records"),
         Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
         Margin = new Thickness(0, 0, 8, 0)
      };

      /// <summary>One record's check as Check all records runs it, and the pill it leaves its verdict on.</summary>
      private sealed record RecordCheck(string Record, StatusPill Mark, Func<Task> Run);

      // The checks of the cards on the page, made with them.
      private readonly List<RecordCheck> checks_ = new();

      private List<DomainInfo> domains_ = new();
      private string serverHostName_ = "";
      private bool serverHostNameKnown_;
      private int failedReads_;
      private string firstError_;

      // Whether a read failed because the link to the service had gone, which the
      // notice then says at Critical, as the shell's pill does.
      private bool linkLost_;

      // The domain list, hidden with nothing in it to choose; and what the page
      // says in its place while the server hosts no domain, with the way to add
      // one (review finding F306: an empty, enabled list above a bare sentence and
      // a twelve-pixel link).
      private FieldRow domainRow_;
      private readonly EmptyState noDomains_ = new()
      {
         Icon = Wpf.Ui.Controls.SymbolRegular.Globe24,
         Text = L("No domains yet - the per-domain records appear once one is added."),
         Margin = new Thickness(0, 8, 0, 0),
         Visibility = Visibility.Collapsed
      };

      /// <summary>Shared by the MTA-STS policy fetches. Redirects are NOT followed,
      /// because RFC 8461 section 3.3 forbids senders from following them - a
      /// policy behind a redirect is a policy senders never see.</summary>
      private static readonly HttpClient http_ = new(new HttpClientHandler { AllowAutoRedirect = false })
      {
         Timeout = TimeSpan.FromSeconds(10)
      };

      public DnsRecordsView()
      {
         var page = new StackPanel { MaxWidth = 1000, HorizontalAlignment = HorizontalAlignment.Left };

         var refresh = new Wpf.Ui.Controls.Button { Content = L("_Refresh") };
         ControlHelp.Explain(refresh, L("Re-read the domains and server configuration"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(refresh, "dns-records-refresh");
         refresh.Click += (s, e) => Reload();

         // Every record of the domain checked at once, where it took a Check on
         // each of seven cards and a scroll past all of them to read the seven
         // lines (review finding F277). Its accessible name is its label, as a
         // voice command says it and as it is seen (WCAG 2.5.3); what it does is
         // its help text and its tool tip.
         System.Windows.Automation.AutomationProperties.SetHelpText(checkAll_, L("Check every record of the domain and sum up what is published"));
         checkAll_.ToolTip = L("Check every record of the domain and sum up what is published");
         System.Windows.Automation.AutomationProperties.SetAutomationId(checkAll_, "dns-records-check-all");
         checkAll_.Click += async (s, e) => await CheckAll_();
         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         actions.Children.Add(checkAll_);
         actions.Children.Add(refresh);

         page.Children.Add(new PageHeader
         {
            PageKey = "dnsrecords",
            Subtitle = L("The records this server needs published in each domain's DNS zone: type, host and value, ready to copy into your DNS provider's console, with a check for whether each one is really live. Checks use this machine's Windows resolver - the same one the DKIM rotation check uses."),
            Actions = actions
         });

         System.Windows.Automation.AutomationProperties.SetAutomationId(domainCombo_, "dns-records-domain");
         domainCombo_.SelectionChanged += (s, e) => BuildCards();
         // A caption names its field and ends in no colon, as every other caption
         // above a field does (review finding F043).
         domainRow_ = new FieldRow { Label = L("_Domain"), Content = domainCombo_ };
         page.Children.Add(domainRow_);

         var openDomains = new Wpf.Ui.Controls.Button { Content = L("_Open Domains") };
         ControlHelp.Explain(openDomains, L("Open Domains, where domains are added"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(openDomains, "dns-records-open-domains");
         openDomains.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("domains");
         noDomains_.Action = openDomains;
         page.Children.Add(noDomains_);

         page.Children.Add(readFailure_);
         System.Windows.Automation.AutomationProperties.SetAutomationId(checkSummary_, "dns-records-check-summary");
         page.Children.Add(checkSummary_);
         page.Children.Add(body_);

         status_.Margin = new Thickness(0, 12, 0, 0);
         status_.TextWrapping = TextWrapping.Wrap;
         status_.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         page.Children.Add(status_);

         var scroller = new ScrollViewer { Content = page, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         scroller.SetResourceReference(PaddingProperty, "AppPagePadding");
         Content = scroller;
      }

      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      // ---- reading the configuration -------------------------------------------

      /// <summary>What this page needs to know about one hosted domain. Copied out
      /// of COM into plain strings up front, so nothing apartment-bound is ever
      /// touched from the background threads the checks run on.</summary>
      private sealed class DomainInfo
      {
         public string Name = "";
         public bool Active;
         public string Postmaster = "";
         public bool DkimEnabled;
         public string DkimSelector = "";
         public string DkimKeyFile = "";
         public string DkimSecondarySelector = "";

         // The server's key rotation, when it has one to report: the records it
         // derived from keys this machine may not be able to read.
         public DkimRotationStatus Rotation;
      }

      private void Reload()
      {
         failedReads_ = 0;
         firstError_ = null;
         linkLost_ = false;

         string previous = SelectedDomainName();

         domains_ = ReadDomains();
         ReadServerHostName();

         domainCombo_.Items.Clear();
         foreach (DomainInfo domain in domains_)
         {
            domainCombo_.Items.Add(new ComboBoxItem
            {
               Content = domain.Active ? domain.Name : F("{0} (inactive)", domain.Name),
               Tag = domain.Name,
               FontSize = Typography.Body
            });
         }

         if (domainCombo_.Items.Count > 0)
         {
            int select = 0;
            for (int i = 0; i < domainCombo_.Items.Count; i++)
               if (string.Equals((string)((ComboBoxItem)domainCombo_.Items[i]).Tag, previous, StringComparison.OrdinalIgnoreCase))
                  select = i;
            domainCombo_.SelectedIndex = select;   // fires BuildCards
         }
         else
         {
            BuildCards();
         }

         status_.Text = L("Configuration read from the server. Records are re-derived every time this page is opened; press Check to query DNS.");

         // The level from the cause: a lost link is Critical, as the shell's pill
         // calls it, and values a server that answered could not give are a Warning.
         readFailure_.Level = ComErrorText.LevelFor(linkLost_);
         // The reason last, where a sentence after it ran on from the runtime's text
         // in brackets that it may end with; and no count, since one failed read may
         // be a whole list of domains. The overview pages' sentences, word for word
         // (review finding F325).
         readFailure_.Text = failedReads_ == 0
            ? null
            : failedReads_ == 1
               ? F("Some values could not be read, so the sections below may be incomplete. Why: {0}", firstError_)
               : F("Some values could not be read, so the sections below may be incomplete. The first reason: {0}", firstError_);
         readFailure_.Visibility = failedReads_ == 0 ? Visibility.Collapsed : Visibility.Visible;
      }

      private string SelectedDomainName()
         => domainCombo_.SelectedItem is ComboBoxItem item ? (string)item.Tag : null;

      private DomainInfo SelectedDomain()
      {
         string name = SelectedDomainName();
         return name == null ? null : domains_.FirstOrDefault(d => string.Equals(d.Name, name, StringComparison.OrdinalIgnoreCase));
      }

      private List<DomainInfo> ReadDomains()
      {
         var result = new List<DomainInfo>();
         dynamic domains = null;
         try
         {
            domains = ServerSession.Current.Application.Domains;
            int count = (int)domains.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic domain = domains.Item[i];
               try
               {
                  result.Add(new DomainInfo
                  {
                     Name = ((string)domain.Name ?? "").Trim(),
                     Active = (bool)domain.Active,
                     Postmaster = ((string)domain.Postmaster ?? "").Trim(),
                     DkimEnabled = (bool)domain.DKIMSignEnabled,
                     DkimSelector = ((string)domain.DKIMSelector ?? "").Trim(),
                     DkimKeyFile = ((string)domain.DKIMPrivateKeyFile ?? "").Trim(),
                     DkimSecondarySelector = ReadSecondarySelector(domain),
                     Rotation = ReadRotation(domain)
                  });
               }
               finally
               {
                  ServerSession.Release((object)domain);
               }
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            linkLost_ |= ServerSession.IsTransportFailure(ex);
         }
         finally
         {
            ServerSession.Release((object)domains);
         }
         return result;
      }

      /// <summary>The server's key rotation, read defensively for the same reason as
      /// the secondary selector below: a server older than the rotation has no such
      /// property, and the page must keep working without it.</summary>
      private static DkimRotationStatus ReadRotation(dynamic domain)
      {
         try
         {
            return DkimRotationStatus.Parse((string)domain.DKIMRotationStatus);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
      }

      /// <summary>Read defensively, like DomainDialog does: against an older server
      /// that predates the rotation API this property does not exist, and the rest
      /// of the page must keep working without it.</summary>
      private static string ReadSecondarySelector(dynamic domain)
      {
         try
         {
            return ((string)domain.DKIMSecondarySelector ?? "").Trim();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return "";
         }
      }

      /// <summary>
      /// The name this server uses to identify itself when delivering outbound
      /// mail: Settings.HostName. Utilities::ComputerName (Common/Util/Utilities.cpp)
      /// returns it for the HELO/EHLO greeting, falling back to the Windows
      /// computer name when it is empty - and a bare computer name is almost never
      /// a public DNS name, which several records below care about.
      /// </summary>
      private void ReadServerHostName()
      {
         serverHostName_ = "";
         serverHostNameKnown_ = false;

         dynamic settings = null;
         try
         {
            settings = ServerSession.Current.Application.Settings;
            serverHostName_ = ((string)settings.HostName ?? "").Trim();
            serverHostNameKnown_ = true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            linkLost_ |= ServerSession.IsTransportFailure(ex);
         }
         finally
         {
            ServerSession.Release((object)settings);
         }
      }

      /// <summary>
      /// File and INI checks only mean anything on the machine the server runs on:
      /// IniFeatureStore locates hMailServer.INI through the local registry and
      /// service table, and the DKIM key file path is a path on the SERVER's disk.
      /// Connected to a remote host, those degrade to "cannot tell" instead.
      /// </summary>
      private static bool IsLocalServer()
      {
         string host = ServerSession.Current != null ? ServerSession.Current.Host : null;
         return string.IsNullOrWhiteSpace(host) || host == "localhost" || host == "127.0.0.1" || host == "::1";
      }

      // ---- building the cards ---------------------------------------------------

      private void BuildCards()
      {
         body_.Children.Clear();

         // A summary is of the cards it was made from.
         checks_.Clear();
         checkSummary_.Visibility = Visibility.Collapsed;

         DomainInfo domain = SelectedDomain();
         checkAll_.IsEnabled = domain != null;

         // No domain to choose: the list goes, and the empty state says so in its
         // place - unless the list could not be read, which the notice says, and
         // which is no claim that the server hosts none.
         domainRow_.Visibility = domains_.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
         noDomains_.Visibility = domains_.Count == 0 && failedReads_ == 0 ? Visibility.Visible : Visibility.Collapsed;

         if (domain == null)
         {
            // nothing per domain to show
         }
         else
         {
            body_.Children.Add(MxCard(domain));
            body_.Children.Add(SpfCard(domain));
            body_.Children.Add(DkimCard(domain));
            body_.Children.Add(DmarcCard(domain));
            body_.Children.Add(MtaStsCard(domain));
            body_.Children.Add(TlsRptCard(domain));
         }

         // Server-wide, not per-domain: one IP, one PTR, one HELO name.
         body_.Children.Add(PtrCard());
      }

      // ---- MX --------------------------------------------------------------------

      /// <summary>
      /// The record that makes the domain's mail come here at all: an MX naming
      /// this server's host name. The page had every record but this one, which a
      /// new installation needs before any of the others matter. The value is
      /// offered only when the host name is a name the Internet can look up
      /// (HostNameVerdict); the check reads the domain's MX records through this
      /// machine's resolver, as the MTA-STS preview does.
      /// </summary>
      private Card MxCard(DomainInfo domain)
      {
         Card card = Section(F("MX - which server takes the mail for {0}", domain.Name), out StackPanel content);

         content.Children.Add(Paragraph(L("Other mail servers look up the domain's MX record to find where to deliver its mail; without one, mail to its addresses does not arrive here. The server the record names needs an address record (A, and AAAA for IPv6) pointing at this server's public IP address - the one your router or firewall forwards port 25 to."),
            Typography.Caption));

         bool usable = serverHostNameKnown_ && HostNameVerdict.Judge(serverHostName_) == HostNameVerdict.Kind.PublicName;
         string value = usable ? "10 " + serverHostName_ : L("(set the host name first: Protocols > SMTP > Host name)");

         if (!usable)
            content.Children.Add(Paragraph(L("The record names this server by its host name, and the host name is not yet a name the Internet can look up, so there is no value to publish yet. Set it on the Protocols page's SMTP tab, then come back."), Typography.Caption));

         RecordBlock block = BuildRecordBlock(content, "dns-records-mx", L("MX"),
            host: domain.Name,
            hostNote: L("the domain itself (many DNS consoles write the zone apex as \"@\")"),
            value: value,
            copyWarning: F("If {0} already receives mail somewhere else - a hosted mailbox provider, another server - publishing this record moves that mail here. Change it only when this server is ready to take it.", domain.Name),
            valueCopyable: usable,
            mx: true);

         block.Check.Click += async (s, e) => await CheckMx(block, domain.Name);
         checks_.Add(new RecordCheck(L("MX"), block.Mark, () => CheckMx(block, domain.Name)));

         return card;
      }

      private async Task CheckMx(RecordBlock block, string domainName)
      {
         string host = (serverHostName_ ?? "").Trim().TrimEnd('.').ToLowerInvariant();

         await RunCheck(block, domainName, () =>
         {
            List<string> exchanges = QueryMxRecords(domainName);
            return (Action)(() =>
            {
               if (exchanges.Count == 0)
               {
                  block.SetResult(StatusLevel.Warning, L("Not published"),
                     Sentences(F("No MX record was found for {0} through this machine's resolver.", domainName), PropagationAdvice));
               }
               else if (host.Length > 0 && exchanges.Contains(host))
               {
                  block.SetResult(StatusLevel.Good, L("Published and correct"),
                     F("{0}'s MX records name {1}, this server's host name: {2}.", domainName, host, string.Join(", ", exchanges)));
               }
               else
               {
                  // Never "wrong": a filtering service or another server in front
                  // of this one is named in the MX by design.
                  block.SetResult(StatusLevel.Information, L("Published, names another server"),
                     F("{0}'s mail goes to {1}, not to this server's host name. That is right when a filtering service or another server takes the mail first and passes it on; otherwise change the record.", domainName, string.Join(", ", exchanges)));
               }
            });
         });
      }

      // ---- SPF -------------------------------------------------------------------

      /// <summary>
      /// The suggested SPF record. The server's outbound identity is its HELO name
      /// (Settings.HostName); when that is set, "a:&lt;name&gt;" authorises its
      /// address and "mx" authorises whatever the domain's MX records point at.
      /// When it is not set, the server greets with its Windows computer name -
      /// not a public DNS name - so no a: mechanism can honestly be offered and
      /// the suggestion falls back to the MX hosts alone.
      ///
      /// "~all" (softfail) rather than "-all" on purpose: this page cannot know
      /// the administrator's other senders, and a hard fail on an incomplete list
      /// makes receivers reject legitimate mail outright.
      /// </summary>
      private string SuggestedSpf()
         => serverHostName_.Length > 0 ? "v=spf1 mx a:" + serverHostName_ + " ~all" : "v=spf1 mx ~all"; // no-loc: a DNS record

      private Card SpfCard(DomainInfo domain)
      {
         Card card = Section(F("SPF - which machines may send mail as {0}", domain.Name), out StackPanel content);

         content.Children.Add(Paragraph(F("This server only checks SPF on incoming mail; publishing the record is for everyone else - it tells receiving servers which machines are allowed to send mail claiming to be from {0}.", domain.Name),
            Typography.Caption));

         string suggested = SuggestedSpf();

         string provenance;
         if (!serverHostNameKnown_)
            provenance = L("The server's host name could not be read, so the suggestion authorises only the domain's MX hosts (\"mx\").");
         else if (serverHostName_.Length > 0)
            provenance = F("\"mx\" authorises whatever {0}'s MX records point at; \"a:{1}\" authorises the address of the host name this server announces in HELO/EHLO (Protocols > SMTP > Host name). \"~all\" soft-fails everything else - deliberately softer than \"-all\" until you are sure the list is complete.", domain.Name, serverHostName_);
         else
            provenance = L("The server's Host name (Protocols > SMTP) is empty, so it announces its Windows computer name in HELO/EHLO - not a public DNS name - and no \"a:\" mechanism can be offered. The suggestion authorises only the domain's MX hosts. Set the host name first; several records on this page need it.");

         content.Children.Add(Paragraph(provenance, Typography.Caption));

         RecordBlock block = BuildRecordBlock(content, "dns-records-spf", L("SPF"),
            host: domain.Name,
            hostNote: L("the domain itself (many DNS consoles write the zone apex as \"@\")"),
            value: suggested,
            copyWarning: F("This suggestion cannot know your other senders. If anything else sends mail as {0} - a newsletter service, a website contact form, a hosted mailbox provider - copying this record as-is makes that mail fail SPF at every receiver that checks it. Add those senders to the record first.", domain.Name));

         block.Check.Click += async (s, e) => await CheckSpf(block, domain.Name, suggested);
         checks_.Add(new RecordCheck(L("SPF"), block.Mark, () => CheckSpf(block, domain.Name, suggested)));

         return card;
      }

      private async Task CheckSpf(RecordBlock block, string domainName, string suggested)
      {
         await RunCheck(block, domainName, () =>
         {
            DnsTxtLookup.LookupResult result = DnsTxtLookup.Query(domainName);
            return (Action)(() =>
            {
               if (result.Status == DnsTxtLookup.LookupStatus.Failed)
               {
                  SetLookupFailed(block, domainName, result.Error);
                  return;
               }

               // RFC 7208 section 4.5: an SPF record is the TXT
               // record whose value begins "v=spf1".
               List<string> spf = result.Records.Where(IsSpfRecord).ToList();

               if (spf.Count == 0)
               {
                  if (result.Records.Count > 0)
                     block.SetResult(StatusLevel.Warning, L("Not published"),
                        Sentences(PluralF(result.Records.Count,
                           "{0} TXT record exists at {1}, and it does not begin \"v=spf1\", so receivers see no SPF policy at all.",
                           "{0} TXT records exist at {1}, but none begins \"v=spf1\", so receivers see no SPF policy at all.",
                           result.Records.Count, domainName), DescribeFound(result.Records)));
                  else
                     block.SetResult(StatusLevel.Warning, L("Not published"), NotFoundAdvice(domainName));
               }
               else if (spf.Count > 1)
               {
                  block.SetResult(StatusLevel.Warning, L("Published, but duplicated"),
                     Sentences(PluralF(spf.Count,
                        "{0} record beginning \"v=spf1\" exists at {1} with others like it. RFC 7208 makes that a permanent SPF error at every receiver - worse than no record. Merge them into one.",
                        "{0} records beginning \"v=spf1\" exist at {1}. RFC 7208 makes that a permanent SPF error at every receiver - worse than no record. Merge them into one.",
                        spf.Count, domainName), DescribeFound(spf)));
               }
               else if (DnsTxtLookup.Matches(spf[0], suggested))
               {
                  block.SetResult(StatusLevel.Good, L("Published and correct"),
                     F("The record at {0} matches the suggestion.", domainName));
               }
               else
               {
                  // Never "wrong": this page cannot know the administrator's other
                  // senders, so a difference from the suggestion may be deliberate
                  // and better. Show what is published so they can judge.
                  block.SetResult(StatusLevel.Information, L("Published, differs from the suggestion"),
                     F("An SPF record is live at {0} and it is not the suggested one. That may well be deliberate - the suggestion cannot know your other senders. Published: {1} — just confirm everything that really sends mail as {0} is covered.", domainName, spf[0]));
               }
            });
         });
      }

      private static bool IsSpfRecord(string record)
      {
         string trimmed = (record ?? "").Trim();
         return trimmed.Equals("v=spf1", StringComparison.OrdinalIgnoreCase)
             || trimmed.StartsWith("v=spf1 ", StringComparison.OrdinalIgnoreCase);
      }

      // ---- DKIM ------------------------------------------------------------------

      private Card DkimCard(DomainInfo domain)
      {
         Card card = Section(F("DKIM - the public key receivers verify {0}'s signatures against", domain.Name), out StackPanel content);

         bool configured = domain.DkimSelector.Length > 0 && domain.DkimKeyFile.Length > 0;

         if (!configured)
         {
            if (domain.DkimSelector.Length > 0 || domain.DkimKeyFile.Length > 0)
            {
               // Half-configured: one of the pair is set. Generating over the top
               // of it could clobber a key the administrator meant to keep, so
               // this page points at the editor that owns the pair instead.
               content.Children.Add(Paragraph(domain.DkimSelector.Length == 0
                  ? L("DKIM is half-configured: the selector is missing, so there is no record to derive. Complete the pair on the domain's DKIM tab.")
                  : L("DKIM is half-configured: the private key file is missing, so there is no record to derive. Complete the pair on the domain's DKIM tab."),
                  Typography.Body));
               content.Children.Add(PageLink("domains", F("Open {0}", L(NavigationMap.TitleOf("domains"))),
                  L("Open Domains, where the DKIM selector and key file are configured")));
               return card;
            }

            content.Children.Add(Paragraph(F("No DKIM key is configured for {0} yet. Generate one here: the private key is saved to a file of your choosing on this machine, the selector and key file are stored on the domain, and signing stays off until the DNS record below has been published and a check on this page has seen it - switching signing on before then would sign mail with a key the world cannot look up.", domain.Name),
               Typography.Body));

            var selectorRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 4, 0, 8) };
            var selectorBox = new TextBox
            {
               Text = "dkim", // no-loc
               FontSize = Typography.Body,
               MinWidth = 180,
               Padding = new Thickness(6),
               VerticalAlignment = VerticalAlignment.Center
            };
            System.Windows.Automation.AutomationProperties.SetName(selectorBox, L("Selector name for the new DKIM key"));
            System.Windows.Automation.AutomationProperties.SetAutomationId(selectorBox, "dns-records-dkim-selector");
            selectorRow.Children.Add(selectorBox);

            var generate = new Wpf.Ui.Controls.Button { Content = L("_Generate key pair…"), Margin = new Thickness(8, 0, 0, 0) };
            ControlHelp.Explain(generate, F("Generate a DKIM key pair for {0}", domain.Name));
            System.Windows.Automation.AutomationProperties.SetAutomationId(generate, "dns-records-dkim-generate");
            generate.Click += (s, e) => GenerateDkim(domain.Name, selectorBox.Text.Trim());
            selectorRow.Children.Add(generate);
            content.Children.Add(selectorRow);

            return card;
         }

         string host = domain.DkimSelector + "._domainkey." + domain.Name;

         // The value is derived from the private key file - the p= tag is the key
         // pair's SubjectPublicKeyInfo, the same record DkimKeyGenerator emits when
         // a key is first made - so no secret leaves this process. Null when the
         // file cannot be read from this machine (typically remote administration:
         // the path is on the server's disk), in which case the check degrades to
         // presence-only and says so, exactly like the rotation flow does.
         string expected = DeriveDkimRecord(domain.DkimKeyFile);

         // A key the server's rotation generated is readable only by the server and
         // the administrators, so a Control Panel that is not elevated - or not on the
         // server at all - cannot derive the record from it. The server derived it,
         // and says what it is.
         if (expected == null && domain.Rotation?.Current != null &&
             string.Equals(domain.Rotation.Current.Selector, domain.DkimSelector, StringComparison.OrdinalIgnoreCase))
         {
            expected = domain.Rotation.Current.Records
               .Where(r => string.Equals(r.Host, host, StringComparison.OrdinalIgnoreCase) && r.Value.Length > 0)
               .Select(r => r.Value)
               .FirstOrDefault();
         }

         content.Children.Add(Paragraph(Sentences(
               F("Derived from the domain's configured key: selector \"{0}\", private key file {1}.", domain.DkimSelector, domain.DkimKeyFile),
               domain.DkimEnabled ? L("Signing is currently on.") : L("Signing is currently off - it can be switched on below once the record is confirmed published."),
               domain.DkimEnabled && expected == null
                  ? L("The key file could not be read from this machine, so the record value cannot be shown here; the domain's DKIM tab showed it when the key was made.")
                  : null),
            Typography.Caption));

         RecordBlock block = BuildRecordBlock(content, "dns-records-dkim", L("DKIM"),
            host: host,
            hostNote: null,
            value: expected ?? L("(The private key file could not be read from this machine, so the record value cannot be derived here. Run this page on the server itself, or use the domain's DKIM tab.)"),
            copyWarning: null,
            valueCopyable: expected != null);

         if (domain.Rotation?.Next != null)
         {
            // The server's rotation says what to publish next, and checks it itself;
            // the switch, the retire and the cancel are on the domain's DKIM tab.
            content.Children.Add(Paragraph(F("The server's key rotation has a next key, selector \"{0}\". Publish its record as well; signing moves to it once the record has resolved for the propagation period, and the domain's DKIM tab shows the rest.", domain.Rotation.Next.Selector),
               Typography.Caption));

            foreach (DkimRotationStatus.Record record in domain.Rotation.Next.Records)
            {
               content.Children.Add(Label(F("{0}: TXT record at {1}", DkimRotationStatus.ActionText(record.Action), record.Host)));
               TextBox next = ReadOnlyBox(0);
               next.Text = record.Value;
               System.Windows.Automation.AutomationProperties.SetName(next, F("TXT record value for {0}", record.Host));
               content.Children.Add(next);
            }
         }
         else if (domain.DkimSecondarySelector.Length > 0)
         {
            content.Children.Add(Paragraph(F("A key rotation is staged (selector \"{0}\"). The staged record and the promote step are managed from the domain's DKIM tab, which gates promotion on its own DNS check; it is deliberately not repeated here.", domain.DkimSecondarySelector),
               Typography.Caption));
         }

         if (domain.Rotation?.Retiring != null)
         {
            content.Children.Add(Paragraph(F("Keep the record of the retiring selector \"{0}\" published until {1}, for mail signed with it that is still on its way.",
                  domain.Rotation.Retiring.Selector, DkimRotationStatus.When(domain.Rotation.Retiring.RetireDue)),
               Typography.Caption));
         }

         // Switching signing on is the dangerous step this card gates: enabled
         // before the record is visible, every receiver fails the signature, and
         // under a DMARC quarantine/reject policy that junks or bounces the mail.
         Wpf.Ui.Controls.Button enable = null;
         if (!domain.DkimEnabled)
         {
            enable = new Wpf.Ui.Controls.Button
            {
               Content = L("_Enable DKIM signing"),
               Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
               IsEnabled = false,
               Margin = new Thickness(0, 8, 0, 0),
               ToolTip = L("Enabled once \"Check\" has confirmed the published record carries this key's public key.")
            };
            ToolTipService.SetShowOnDisabled(enable, true);
            ControlHelp.Explain(enable, F("Enable DKIM signing for {0}; unlocked once the DNS check has passed", domain.Name));
            System.Windows.Automation.AutomationProperties.SetAutomationId(enable, "dns-records-dkim-enable");
            enable.Click += (s, e) => EnableDkimSigning(domain.Name);
            content.Children.Add(enable);
         }

         Wpf.Ui.Controls.Button enableButton = enable;
         block.Check.Click += async (s, e) => await CheckDkim(block, host, expected, enableButton);
         checks_.Add(new RecordCheck(L("DKIM"), block.Mark, () => CheckDkim(block, host, expected, enableButton)));

         return card;
      }

      private async Task CheckDkim(RecordBlock block, string host, string expected, Wpf.Ui.Controls.Button enableButton)
      {
         await RunCheck(block, host, () =>
         {
            if (expected == null)
            {
               // Presence-only: the record can be looked up but not compared, and
               // a record that cannot be verified must not unlock the enable step.
               DnsTxtLookup.LookupResult found = DnsTxtLookup.Query(host);
               return (Action)(() =>
               {
                  switch (found.Status)
                  {
                     case DnsTxtLookup.LookupStatus.Found:
                        block.SetResult(StatusLevel.Information, L("Cannot tell"),
                           F("A TXT record exists at {0}, but the private key file could not be read from this machine, so whether it carries the right key cannot be verified from here. Run the check on the server itself, or use the domain's DKIM tab.", host));
                        break;
                     case DnsTxtLookup.LookupStatus.NoRecord:
                        block.SetResult(StatusLevel.Warning, L("Not published"), NotFoundAdvice(host));
                        break;
                     default:
                        SetLookupFailed(block, host, found.Error);
                        break;
                  }
               });
            }

            DnsTxtLookup.MatchResult result = DnsTxtLookup.CheckExpected(host, expected);
            return (Action)(() =>
            {
               bool matches = result.Status == DnsTxtLookup.MatchStatus.FoundAndMatches;

               // Any outcome short of a confirmed match locks the enable step
               // (again): a pass from earlier is stale evidence the moment a fresh
               // check stops confirming it. Same rule as the rotation's Promote.
               if (enableButton != null)
                  enableButton.IsEnabled = matches;

               switch (result.Status)
               {
                  case DnsTxtLookup.MatchStatus.FoundAndMatches:
                     block.SetResult(StatusLevel.Good, L("Published and correct"),
                        Sentences(F("The record at {0} carries this key's public key.", host),
                           enableButton != null ? L("It is safe to enable signing.") : null));
                     break;
                  case DnsTxtLookup.MatchStatus.NoRecord:
                     block.SetResult(StatusLevel.Warning, L("Not published"), NotFoundAdvice(host));
                     break;
                  case DnsTxtLookup.MatchStatus.FoundButDifferent:
                     block.SetResult(StatusLevel.Critical, L("Published, but wrong key"),
                        Sentences(F("What is published at {0} does not carry this key's public key, so every signature made with this key fails verification. Waiting will not fix this - correct the published value to match the one above.", host), DescribeFound(result.Records)));
                     break;
                  default:
                     SetLookupFailed(block, host, result.Error);
                     break;
               }
            });
         });
      }

      /// <summary>
      /// First-time key generation, reusing <see cref="DkimKeyGenerator"/> - the
      /// same generator and record format as the domain dialog. Deliberately does
      /// NOT switch signing on: that step stays behind the DNS check above, which
      /// is the entire point of this page.
      /// </summary>
      private void GenerateDkim(string domainName, string selector)
      {
         if (selector.Length == 0)
            selector = "dkim"; // no-loc: the default selector name

         var save = new Microsoft.Win32.SaveFileDialog
         {
            Title = L("Save the DKIM private key"),
            Filter = L("PEM key files (*.pem)|*.pem|All files (*.*)|*.*"),
            FileName = selector + "._domainkey." + domainName + ".pem"
         };

         if (save.ShowDialog() != true)
            return;

         try
         {
            DkimKeyGenerator.Result generated = DkimKeyGenerator.Generate(selector, domainName);
            File.WriteAllText(save.FileName, generated.PrivateKeyPem);

            dynamic domains = ServerSession.Current.Application.Domains;
            try
            {
               dynamic d = domains.ItemByName[domainName];
               d.DKIMSelector = selector;
               d.DKIMPrivateKeyFile = save.FileName;

               // Explicitly off, even though this branch is only reachable when no
               // key was configured: a domain can be left with signing switched on
               // and no key (the server reports error 5305 and sends unsigned), and
               // storing a fresh key into that state would start signing with a key
               // the world cannot look up yet. The card promises signing stays off
               // until the DNS check has passed; this line is that promise.
               d.DKIMSignEnabled = false;
               d.Save();
               ServerSession.Release(d);
            }
            finally
            {
               ServerSession.Release(domains);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not generate and store the DKIM key: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }

         Reload();
      }

      private void EnableDkimSigning(string domainName)
      {
         try
         {
            dynamic domains = ServerSession.Current.Application.Domains;
            try
            {
               dynamic d = domains.ItemByName[domainName];
               d.DKIMSignEnabled = true;
               d.Save();
               ServerSession.Release(d);
            }
            finally
            {
               ServerSession.Release(domains);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not enable DKIM signing: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }

         Reload();
      }

      /// <summary>
      /// Rebuilds the DNS TXT value from the private key's PEM file - the same
      /// derivation DomainDialog uses to restore a staged rotation. Returns null
      /// when the file cannot be read from this machine.
      /// </summary>
      private static string DeriveDkimRecord(string keyFile)
      {
         try
         {
            string pem = File.ReadAllText(keyFile);
            using var rsa = RSA.Create();
            rsa.ImportFromPem(pem);
            return "v=DKIM1; k=rsa; p=" + Convert.ToBase64String(rsa.ExportSubjectPublicKeyInfo()); // no-loc
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
      }

      // ---- DMARC -----------------------------------------------------------------

      private Card DmarcCard(DomainInfo domain)
      {
         Card card = Section(L("DMARC - what receivers should do with mail that fails SPF and DKIM"), out StackPanel content);

         // rua= is only offered when the domain has a postmaster address to send
         // reports to; inventing a mailbox would produce a record that asks the
         // world to mail reports somewhere that does not exist.
         bool haveRua = domain.Postmaster.Contains("@");
         string suggested = haveRua
            ? "v=DMARC1; p=none; rua=mailto:" + domain.Postmaster // no-loc
            : "v=DMARC1; p=none"; // no-loc: a DNS record

         content.Children.Add(Paragraph(L("p=none is the right starting point, not an oversight: it makes receivers check and report but deliver as normal. A stricter policy (quarantine or reject) tells every receiver to junk or refuse mail that fails alignment - and until reports show your own mail passing SPF or DKIM, the mail that fails will include yours. Tighten only after the reports say it is safe."),
            Typography.Caption));

         content.Children.Add(Paragraph(
            haveRua
               ? F("rua=mailto:{0} sends you the aggregate reports; the address is the domain's postmaster address as configured on the domain. Reports arrive from strangers, so the mailbox must exist and accept them.", domain.Postmaster)
               : F("No rua= is included because {0} has no postmaster address configured, and this page will not invent a mailbox. Without rua= the policy still applies, but nobody sends you reports - add \"; rua=mailto:<a mailbox you read>\" when you have one.", domain.Name),
            Typography.Caption));

         RecordBlock block = BuildRecordBlock(content, "dns-records-dmarc", L("DMARC"),
            host: "_dmarc." + domain.Name,
            hostNote: null,
            value: suggested,
            copyWarning: L("Keep p=none until you know your own mail passes. Editing this to p=quarantine or p=reject before then tells every receiver to junk or bounce your own legitimate mail."));

         block.Check.Click += async (s, e) => await CheckDmarc(block, domain.Name);
         checks_.Add(new RecordCheck(L("DMARC"), block.Mark, () => CheckDmarc(block, domain.Name)));

         return card;
      }

      private async Task CheckDmarc(RecordBlock block, string domainName)
      {
         string host = "_dmarc." + domainName;

         await RunCheck(block, host, () =>
         {
            DnsTxtLookup.LookupResult result = DnsTxtLookup.Query(host);
            return (Action)(() =>
            {
               if (result.Status == DnsTxtLookup.LookupStatus.Failed)
               {
                  SetLookupFailed(block, host, result.Error);
                  return;
               }

               // The same acceptance rule as the server's own evaluator:
               // DMARC::RetrievePolicy_ takes TXT records whose trimmed value
               // begins "v=DMARC1".
               List<string> dmarc = result.Records
                  .Where(r => (r ?? "").Trim().StartsWith("v=DMARC1", StringComparison.OrdinalIgnoreCase))
                  .ToList();

               if (dmarc.Count == 0)
               {
                  if (result.Records.Count > 0)
                     block.SetResult(StatusLevel.Warning, L("Not published"),
                        Sentences(PluralF(result.Records.Count,
                           "{0} TXT record exists at {1}, and it does not begin \"v=DMARC1\", so receivers - and this server's own DMARC check - see no policy.",
                           "{0} TXT records exist at {1}, but none begins \"v=DMARC1\", so receivers - and this server's own DMARC check - see no policy.",
                           result.Records.Count, host), DescribeFound(result.Records)));
                  else
                     block.SetResult(StatusLevel.Warning, L("Not published"), NotFoundAdvice(host));
                  return;
               }

               if (dmarc.Count > 1)
               {
                  block.SetResult(StatusLevel.Warning, L("Published, but duplicated"),
                     Sentences(PluralF(dmarc.Count,
                        "{0} record beginning \"v=DMARC1\" exists at {1} with others like it. Receivers treat multiple DMARC records as no policy at all - remove all but one.",
                        "{0} records beginning \"v=DMARC1\" exist at {1}. Receivers treat multiple DMARC records as no policy at all - remove all but one.",
                        dmarc.Count, host), DescribeFound(dmarc)));
                  return;
               }

               string record = dmarc[0];
               string policy = ParseTag(record, "p");

               if (policy == null)
               {
                  block.SetResult(StatusLevel.Warning, L("Published, but incomplete"),
                     F("The record at {0} has no p= tag, so it declares no policy and receivers ignore it. Published: {1}", host, record));
               }
               else if (policy.Equals("none", StringComparison.OrdinalIgnoreCase))
               {
                  block.SetResult(StatusLevel.Good, L("Published and correct"),
                     Sentences(F("A DMARC record with p=none is live at {0}.", host),
                        ParseTag(record, "rua") == null
                           ? L("It has no rua= tag, so nobody sends you reports - fine, but you learn nothing; add one when you have a mailbox for it.")
                           : null,
                        F("Published: {0}", record)));
               }
               else if (policy.Equals("quarantine", StringComparison.OrdinalIgnoreCase)
                     || policy.Equals("reject", StringComparison.OrdinalIgnoreCase))
               {
                  block.SetResult(StatusLevel.Information, L("Published, stricter than the starting point"),
                     F("A DMARC record with p={0} is live at {1} - stricter than the p=none starting point. That is right if you already know your mail passes alignment; if you are still setting SPF and DKIM up, mail that fails is being {2} right now. Published: {3}",
                        policy.ToLowerInvariant(), host, policy.Equals("reject", StringComparison.OrdinalIgnoreCase) ? L("rejected") : L("quarantined"), record));
               }
               else
               {
                  block.SetResult(StatusLevel.Warning, L("Published, but different"),
                     F("The record at {0} carries p={1}, which is not a policy receivers recognise (none, quarantine, reject). Published: {2}", host, policy, record));
               }
            });
         });
      }

      // ---- MTA-STS ---------------------------------------------------------------

      private Card MtaStsCard(DomainInfo domain)
      {
         Card card = Section(F("MTA-STS - require TLS from servers delivering to {0}", domain.Name), out StackPanel content);

         content.Children.Add(Paragraph(F("Two halves: a TXT record that tells senders a policy exists, and the policy itself, fetched from https://mta-sts.{0}/.well-known/mta-sts.txt. This server serves that policy itself - there is no file to upload - when 'Serve MTA-STS policies for local domains' is on and an HTTPS web-services listener is running (RFC 8461 section 3.3 allows HTTPS only). The name mta-sts.{0} also needs an A or CNAME record pointing at this server, and the listener's certificate must cover that name.", domain.Name),
            Typography.Caption));

         // The id's only job is to CHANGE whenever the policy changes, so senders
         // refetch instead of trusting their cache. Any value is as correct as
         // this one; a timestamp just makes "newer" self-evident.
         string suggestedId = DateTime.UtcNow.ToString("yyyyMMddHHmm");
         string suggested = "v=STSv1; id=" + suggestedId; // no-loc

         RecordBlock block = BuildRecordBlock(content, "dns-records-mtasts", L("MTA-STS"),
            host: "_mta-sts." + domain.Name,
            hostNote: null,
            value: suggested,
            copyWarning: null);

         content.Children.Add(Paragraph(F("The id ({0}, generated just now) is yours to choose - any value is correct. Its only job is to change whenever the policy changes, so caches refetch.", suggestedId),
            Typography.Caption));

         block.Check.Click += async (s, e) => await CheckMtaStsTxt(block, domain.Name);
         checks_.Add(new RecordCheck(L("MTA-STS"), block.Mark, () => CheckMtaStsTxt(block, domain.Name)));

         // ---- the policy the server will serve ----
         content.Children.Add(Label(L("Policy file the server will serve (read-only; served by the server itself):")));

         var policyBox = ReadOnlyBox(70);
         policyBox.SetResourceReference(Control.ForegroundProperty, "TextFillColorPrimaryBrush");
         System.Windows.Automation.AutomationProperties.SetName(policyBox, L("MTA-STS policy file content the server will serve"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(policyBox, "dns-records-mtasts-policy");
         content.Children.Add(policyBox);

         var policyNote = new TextBlock
         {
            FontSize = Typography.Caption,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 4),
            Opacity = 0.75
         };
         content.Children.Add(policyNote);

         BuildMtaStsPolicyPreview(domain.Name, policyBox, policyNote);

         // ---- fetch what is actually served ----
         var fetchButton = new Wpf.Ui.Controls.Button { Content = L("Check _policy (HTTPS)"), Margin = new Thickness(0, 4, 0, 0) };
         ControlHelp.Explain(fetchButton,
            F("Fetch the MTA-STS policy for {0} over HTTPS and show what is served", domain.Name));
         System.Windows.Automation.AutomationProperties.SetAutomationId(fetchButton, "dns-records-mtasts-fetch");
         content.Children.Add(fetchButton);

         var fetchMark = new StatusPill { Margin = new Thickness(0, 1, 10, 0), VerticalAlignment = VerticalAlignment.Top };
         var fetchText = new TextBlock { TextWrapping = TextWrapping.Wrap };
         fetchText.SetResourceReference(StyleProperty, "TextCaption");
         content.Children.Add(ResultRow(fetchMark, fetchText));
         SetResultOn(fetchMark, fetchText, StatusLevel.Normal, L("Not checked"),
            F("Press \"Check policy (HTTPS)\" to fetch https://mta-sts.{0}/.well-known/mta-sts.txt from this machine.", domain.Name));

         fetchButton.Click += async (s, e) => await FetchMtaStsPolicy(domain.Name, fetchButton, fetchMark, fetchText);
         checks_.Add(new RecordCheck(L("MTA-STS policy"), fetchMark, () => FetchMtaStsPolicy(domain.Name, fetchButton, fetchMark, fetchText)));

         return card;
      }

      /// <summary>
      /// Reproduces the policy body HandleMtaStsPolicy_ will serve, from the same
      /// inputs: mode and max_age from hMailServer.INI (clamped the way the server
      /// clamps them), mx lines from the MtaStsPolicyMx override when set,
      /// otherwise from the domain's MX records - which the server looks up live
      /// at request time, so the lookup here is this machine's view of the same
      /// answer - falling back to the server host name, as the server does.
      /// </summary>
      private void BuildMtaStsPolicyPreview(string domainName, TextBox policyBox, TextBlock policyNote)
      {
         bool local = IsLocalServer();
         IniFeatureStore ini = local ? new IniFeatureStore() : null;
         bool iniReadable = ini != null && ini.IsAvailable;

         string mode = "enforce";
         int maxAge = 604800;
         string mxOverride = "";
         bool hosting = true;
         int httpsPort = 0;

         if (iniReadable)
         {
            // Defaults and clamping mirror IniFileSettings.cpp and
            // WebServicesServer::HandleMtaStsPolicy_ exactly.
            hosting = ini.ReadBool("MtaStsHostingEnabled", true);
            httpsPort = int.TryParse(ini.Read("WebServicesHttpsPort", "0").Trim(), out int p) ? p : 0;
            mode = ini.Read("MtaStsPolicyMode", "enforce").Trim().ToLowerInvariant();
            if (mode != "enforce" && mode != "testing" && mode != "none")
               mode = "enforce";
            maxAge = int.TryParse(ini.Read("MtaStsPolicyMaxAge", "604800").Trim(), out int a) ? a : 604800;
            if (maxAge < 86400) maxAge = 86400;
            if (maxAge > 31557600) maxAge = 31557600;
            mxOverride = ini.Read("MtaStsPolicyMx", "").Trim();
         }

         string caveat = iniReadable
            ? L("Mode and max_age read from hMailServer.ini on this machine.")
            : (local
               ? L("hMailServer.ini was not found on this machine, so mode and max_age shown are the shipped defaults.")
               : L("Connected to a remote server: hMailServer.ini cannot be read from here, so mode and max_age shown are the shipped defaults."));

         if (iniReadable && !hosting)
            caveat = Sentences(caveat, L("Note: 'Serve MTA-STS policies for local domains' is off on the Web services & autoconfiguration page, so the server does not serve this policy."));
         else if (iniReadable && httpsPort <= 0)
            caveat = Sentences(caveat, L("Note: the web services listener has no HTTPS port on the Web services & autoconfiguration page, so the policy cannot be served (RFC 8461 allows HTTPS only). The TXT record would advertise a policy nothing serves."));

         List<string> mxHosts = null;
         string mxNote;

         if (mxOverride.Length > 0)
         {
            mxHosts = mxOverride.Split(',').Select(s => s.Trim().ToLowerInvariant()).Where(s => s.Length > 0).ToList();
            mxNote = L("mx lines from 'Policy MX host patterns' on the Web services & autoconfiguration page.");
         }
         else
         {
            mxNote = F("mx lines are derived from {0}'s live MX records at request time; the lookup below is this machine's view of the same answer.", domainName);
         }

         void Render(List<string> hosts, string extra)
         {
            var body = new StringBuilder();
            body.Append("version: STSv1\r\n"); // no-loc
            body.Append("mode: ").Append(mode).Append("\r\n"); // no-loc
            foreach (string mx in hosts)
               body.Append("mx: ").Append(mx).Append("\r\n"); // no-loc
            body.Append("max_age: ").Append(maxAge).Append("\r\n");
            policyBox.Text = body.ToString();
            policyNote.Text = Sentences(caveat, mxNote, extra);
         }

         if (mxHosts != null)
         {
            Render(mxHosts, "");
            return;
         }

         Render(new List<string> { F("(looking up {0}'s MX records…)", domainName) }, "");

         Task.Run(() => QueryMxRecords(domainName)).ContinueWith(t =>
         {
            List<string> found = t.Status == TaskStatus.RanToCompletion ? t.Result : null;
            if (found != null && found.Count > 0)
            {
               Render(found, "");
            }
            else if (serverHostName_.Length > 0)
            {
               // The server's own fallback when a domain has no MX records:
               // GetPolicyMxHosts_ uses Configuration::GetHostName().
               Render(new List<string> { serverHostName_.ToLowerInvariant() },
                  L("No MX records were found from this machine, so the server would fall back to its own host name."));
            }
            else
            {
               Render(new List<string> { L("(no MX records found, and the server host name is not set - the server would serve no policy)") }, "");
            }
         }, TaskScheduler.FromCurrentSynchronizationContext());
      }

      private async Task CheckMtaStsTxt(RecordBlock block, string domainName)
      {
         string host = "_mta-sts." + domainName;

         await RunCheck(block, host, () =>
         {
            DnsTxtLookup.LookupResult result = DnsTxtLookup.Query(host);
            return (Action)(() =>
            {
               if (result.Status == DnsTxtLookup.LookupStatus.Failed)
               {
                  SetLookupFailed(block, host, result.Error);
                  return;
               }

               // RFC 8461: a policy-discovery record begins "v=STSv1". The id is
               // the administrator's to choose, so any id counts as correct here.
               List<string> sts = result.Records
                  .Where(r => (r ?? "").Replace(" ", "").StartsWith("v=STSv1", StringComparison.OrdinalIgnoreCase))
                  .ToList();

               if (sts.Count == 0)
               {
                  if (result.Records.Count > 0)
                     block.SetResult(StatusLevel.Warning, L("Not published"),
                        Sentences(PluralF(result.Records.Count,
                           "{0} TXT record exists at {1}, and it does not begin \"v=STSv1\", so senders see no policy.",
                           "{0} TXT records exist at {1}, but none begins \"v=STSv1\", so senders see no policy.",
                           result.Records.Count, host), DescribeFound(result.Records)));
                  else
                     block.SetResult(StatusLevel.Warning, L("Not published"), NotFoundAdvice(host));
                  return;
               }

               string id = ParseTag(sts[0], "id");
               if (id == null)
               {
                  block.SetResult(StatusLevel.Warning, L("Published, but incomplete"),
                     F("The record at {0} has no id= tag, which RFC 8461 requires - senders may never notice policy updates. Published: {1}", host, sts[0]));
               }
               else
               {
                  block.SetResult(StatusLevel.Good, L("Published and correct"),
                     F("A v=STSv1 record is live at {0} with id {1}. The id being different from the suggestion is fine - it is yours to choose; change it whenever the policy changes.", host, id));
               }
            });
         });
      }

      /// <summary>
      /// Fetches the policy exactly the way a sending server does: over HTTPS, no
      /// redirects (RFC 8461 section 3.3). A success proves, from this machine,
      /// that mta-sts.&lt;domain&gt; resolves, the certificate validates for that
      /// name, the listener answers, and what it serves - the internet's view can
      /// still differ if a firewall treats this machine specially, and the result
      /// says so rather than claiming more than one vantage point can know. A fetch
      /// already running is the one returned, as a record's check is (RunCheck).
      /// </summary>
      private Task FetchMtaStsPolicy(string domainName, Wpf.Ui.Controls.Button button, StatusPill mark, TextBlock text)
         => Running_(button, () => FetchMtaStsPolicyOnce_(domainName, button, mark, text));

      private async Task FetchMtaStsPolicyOnce_(string domainName, Wpf.Ui.Controls.Button button, StatusPill mark, TextBlock text)
      {
         string url = "https://mta-sts." + domainName + "/.well-known/mta-sts.txt";

         button.IsEnabled = false;
         SetResultOn(mark, text, StatusLevel.Information, L("Checking…"), F("Fetching {0}…", url));

         try
         {
            HttpResponseMessage response = await http_.GetAsync(url);

            int code = (int)response.StatusCode;

            if (code >= 300 && code < 400)
            {
               SetResultOn(mark, text, StatusLevel.Warning, L("Not served"),
                  F("{0} answered with a redirect (HTTP {1}). Sending servers must not follow redirects (RFC 8461 section 3.3), so this counts as no policy.", url, code));
               return;
            }

            if (code != 200)
            {
               SetResultOn(mark, text, StatusLevel.Warning, L("Not served"),
                  F("{0} answered HTTP {1}. The server serves the policy only when 'Serve MTA-STS policies for local domains' is on, the domain is hosted and active, and the request's Host header is mta-sts.{2}.", url, code, domainName));
               return;
            }

            string bodyText = await response.Content.ReadAsStringAsync();
            string snippet = bodyText.Length > 400 ? bodyText.Substring(0, 400) + "…" : bodyText;

            if (bodyText.TrimStart().StartsWith("version:", StringComparison.OrdinalIgnoreCase) && bodyText.Contains("STSv1"))
            {
               SetResultOn(mark, text, StatusLevel.Good, L("Served"),
                  F("The policy is being served over HTTPS. From this machine that also proves mta-sts.{0} resolves, the certificate validates for that name, and the listener answers - the internet's view can still differ if a firewall treats this machine specially. Served content: {1}", domainName, snippet));
            }
            else
            {
               SetResultOn(mark, text, StatusLevel.Warning, L("Answered, but not with a policy"),
                  F("{0} answered HTTP 200, but the body is not an MTA-STS policy (it does not begin \"version: STSv1\"). Something other than the MTA-STS host is answering that URL. Served: {1}", url, snippet));
            }
         }
         catch (TaskCanceledException)
         {
            SetResultOn(mark, text, StatusLevel.Warning, L("Not served"),
               F("{0} did not answer within 10 seconds from this machine. Likely causes: no A/CNAME record for mta-sts.{1}, the HTTPS listener is not running (no HTTPS port on the Web services & autoconfiguration page), or a firewall in between. Checked from this machine only.", url, domainName));
         }
         catch (HttpRequestException ex)
         {
            string reason = ex.InnerException != null ? ex.InnerException.Message : ex.Message;
            SetResultOn(mark, text, StatusLevel.Warning, L("Not served"),
               F("{0} could not be fetched from this machine: {1} Likely causes: no A/CNAME record for mta-sts.{2}, the HTTPS listener is not running (it has no HTTPS port until one is set on the Web services & autoconfiguration page), or a certificate that does not cover mta-sts.{2}. Checked from this machine only.", url, reason, domainName));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            SetResultOn(mark, text, StatusLevel.Information, L("Lookup failed"),
               F("The fetch itself failed: {0} This says nothing about the policy.", ex.Message));
         }
         finally
         {
            button.IsEnabled = true;
         }
      }

      // ---- TLS-RPT ---------------------------------------------------------------

      private Card TlsRptCard(DomainInfo domain)
      {
         Card card = Section(F("TLS-RPT - ask other servers to report TLS failures when delivering to {0}", domain.Name), out StackPanel content);

         content.Children.Add(Paragraph(F("Publishing this record asks every server that delivers to {0} to mail you a daily report when TLS to you fails or is downgraded. The reports go to the rua= mailbox, which must exist and accept mail from strangers. This works regardless of any hMailServer setting - the senders do the reporting.", domain.Name),
            Typography.Caption));

         bool havePostmaster = domain.Postmaster.Contains("@");

         var addressRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 2, 0, 4) };
         addressRow.Children.Add(new TextBlock
         {
            Text = L("Report mailbox:"),
            FontSize = Typography.Label,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 8, 0)
         });
         var addressBox = new TextBox
         {
            Text = havePostmaster ? domain.Postmaster : "",
            FontSize = Typography.Body,
            MinWidth = 280,
            Padding = new Thickness(6),
            VerticalAlignment = VerticalAlignment.Center
         };
         System.Windows.Automation.AutomationProperties.SetName(addressBox, F("Mailbox that receives TLS reports for {0}", domain.Name));
         System.Windows.Automation.AutomationProperties.SetAutomationId(addressBox, "dns-records-tlsrpt-address");
         addressRow.Children.Add(addressBox);
         content.Children.Add(addressRow);

         content.Children.Add(Paragraph(
            havePostmaster
               ? L("Pre-filled with the domain's postmaster address, as configured on the domain. Change it if reports should go elsewhere.")
               : F("{0} has no postmaster address configured, and this page will not invent a mailbox - enter the address that should receive the reports. Until one is entered there is no record to copy.", domain.Name),
            Typography.Caption));

         RecordBlock block = BuildRecordBlock(content, "dns-records-tlsrpt", L("TLS-RPT"),
            host: "_smtp._tls." + domain.Name,
            hostNote: null,
            value: BuildTlsRptValue(addressBox.Text.Trim()),
            copyWarning: null,
            valueCopyable: addressBox.Text.Trim().Contains("@"));

         addressBox.TextChanged += (s, e) =>
         {
            string address = addressBox.Text.Trim();
            bool usable = address.Contains("@");
            block.ValueBox.Text = BuildTlsRptValue(address);
            block.CopyValue.IsEnabled = usable;
            block.CopyValue.ToolTip = usable ? null : L("Enter a report mailbox first - a record without a real rua= mailbox asks for nothing.");
         };
         ToolTipService.SetShowOnDisabled(block.CopyValue, true);
         if (!addressBox.Text.Trim().Contains("@"))
            block.CopyValue.ToolTip = L("Enter a report mailbox first - a record without a real rua= mailbox asks for nothing.");

         // The other direction, read from the server's own code: TlsRptReporterTask
         // collects statistics hourly but discards each completed day unsent while
         // TlsRptFromAddress is empty in [Settings] of hMailServer.INI.
         string sendingNote;
         if (IsLocalServer())
         {
            var ini = new IniFeatureStore();
            if (ini.IsAvailable)
            {
               string from = ini.Read("TlsRptFromAddress", "").Trim();
               sendingNote = from.Length > 0
                  ? F("Separately, this server sends such reports to other domains: its TLS report sender address is {0}, so its own reports go out.", from)
                  : L("Separately, this server sends no such reports of its own: 'TLS report sender address' on the Transport security page is empty (the default), so it collects statistics and discards every completed day unsent. Set it if you want to send reports too.");
            }
            else
            {
               sendingNote = L("Separately, this server sends its own reports to other domains only when 'TLS report sender address' is set on the Transport security page; hMailServer.ini could not be found on this machine to check it.");
            }
         }
         else
         {
            sendingNote = L("Separately, this server sends its own reports to other domains only when 'TLS report sender address' is set on the Transport security page; hMailServer.ini on the server is not readable from this machine to check it.");
         }
         content.Children.Add(Paragraph(sendingNote, Typography.Caption));

         block.Check.Click += async (s, e) => await CheckTlsRpt(block, domain.Name, addressBox);
         checks_.Add(new RecordCheck(L("TLS-RPT"), block.Mark, () => CheckTlsRpt(block, domain.Name, addressBox)));

         return card;
      }

      private static string BuildTlsRptValue(string address)
         => address.Contains("@")
            ? "v=TLSRPTv1; rua=mailto:" + address
            : L("v=TLSRPTv1; rua=mailto:<enter a report mailbox above>");

      private async Task CheckTlsRpt(RecordBlock block, string domainName, TextBox addressBox)
      {
         string host = "_smtp._tls." + domainName;
         string suggestedAddress = addressBox.Text.Trim();

         await RunCheck(block, host, () =>
         {
            DnsTxtLookup.LookupResult result = DnsTxtLookup.Query(host);
            return (Action)(() =>
            {
               if (result.Status == DnsTxtLookup.LookupStatus.Failed)
               {
                  SetLookupFailed(block, host, result.Error);
                  return;
               }

               List<string> rpt = result.Records
                  .Where(r => (r ?? "").Trim().StartsWith("v=TLSRPTv1", StringComparison.OrdinalIgnoreCase))
                  .ToList();

               if (rpt.Count == 0)
               {
                  if (result.Records.Count > 0)
                     block.SetResult(StatusLevel.Warning, L("Not published"),
                        Sentences(PluralF(result.Records.Count,
                           "{0} TXT record exists at {1}, and it does not begin \"v=TLSRPTv1\".",
                           "{0} TXT records exist at {1}, but none begins \"v=TLSRPTv1\".",
                           result.Records.Count, host),
                           DescribeFound(result.Records)));
                  else
                     block.SetResult(StatusLevel.Warning, L("Not published"), NotFoundAdvice(host));
                  return;
               }

               string record = rpt[0];
               string rua = ParseTag(record, "rua");

               if (rua == null || rua.Length == 0)
               {
                  block.SetResult(StatusLevel.Warning, L("Published, but incomplete"),
                     F("The record at {0} has no rua= tag, so it asks for reports and gives senders nowhere to send them. Published: {1}", host, record));
               }
               else if (suggestedAddress.Contains("@")
                     && rua.IndexOf("mailto:" + suggestedAddress, StringComparison.OrdinalIgnoreCase) >= 0)
               {
                  block.SetResult(StatusLevel.Good, L("Published and correct"),
                     F("A v=TLSRPTv1 record is live at {0} and its reports go to {1}.", host, suggestedAddress));
               }
               else
               {
                  block.SetResult(StatusLevel.Information, L("Published, differs from the suggestion"),
                     F("A v=TLSRPTv1 record is live at {0} with rua={1} - a different report mailbox than the one above, which is fine as long as it exists and is read. Published: {2}", host, rua, record));
               }
            });
         });
      }

      // ---- reverse DNS / PTR and the HELO name ------------------------------------

      /// <summary>
      /// The one entry on this page the administrator cannot publish themselves,
      /// said plainly instead of pretended otherwise. The PTR record for the
      /// server's public IP address lives in the reverse zone owned by whoever
      /// owns the IP - usually the hosting provider or ISP - and is changed by
      /// asking them, not by editing the domain's own zone. The page also does not
      /// know the server's public IP (behind NAT it is not visible from here at
      /// all), so it shows the one half it can know: the HELO name the PTR should
      /// agree with.
      /// </summary>
      private Card PtrCard()
      {
         Card card = Section(L("Reverse DNS (PTR) and the HELO name - set by whoever owns your IP address"), out StackPanel content);

         content.Children.Add(Paragraph(L("Receivers look up the PTR record of the connecting IP address and compare it with the name the server announces in HELO/EHLO; this server judges incoming mail by the same standard when the PTR and HELO checks on Anti-spam settings are on. Unlike every other record on this page, the PTR is not published in your domain's zone: it lives in the reverse zone of the IP address, which belongs to whoever owns the address - usually your hosting provider or ISP. Ask them to set it; there is nothing to paste into your own DNS console."),
            Typography.Caption));

         string heloName;
         string heloNote;

         if (!serverHostNameKnown_)
         {
            heloName = L("(could not be read from the server)");
            heloNote = L("The server's host name could not be read, so the name it announces in HELO/EHLO is unknown from here.");
         }
         else if (serverHostName_.Length > 0)
         {
            heloName = serverHostName_;
            heloNote = L("This is Protocols > SMTP > Host name, the name the server announces in HELO/EHLO on every outbound delivery. Ask the IP's owner to make the PTR of your public IP resolve to exactly this name, and publish an A record for this name pointing back at the same IP - receivers check both directions.");
         }
         else
         {
            heloName = IsLocalServer() ? Environment.MachineName : L("(the server's Windows computer name)");
            heloNote = IsLocalServer()
               ? L("The Host name on the Protocols page is empty, so the server falls back to its Windows computer name - shown above as this machine's name. A bare computer name is not a public DNS name: receivers that compare HELO with DNS will penalise it, and no PTR can agree with it. Set a real host name first.")
               : L("The Host name on the Protocols page is empty, so the server falls back to its Windows computer name, which cannot be read from this machine. A bare computer name is not a public DNS name: receivers that compare HELO with DNS will penalise it, and no PTR can agree with it. Set a real host name first.");
         }

         content.Children.Add(Label(L("Host name this server announces (give this to the IP's owner for the PTR):")));
         TextBox heloBox = ReadOnlyBox(0);
         heloBox.Text = heloName;
         heloBox.SetResourceReference(Control.ForegroundProperty, "TextFillColorPrimaryBrush");
         System.Windows.Automation.AutomationProperties.SetName(heloBox, L("Host name this server announces in HELO"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(heloBox, "dns-records-ptr-helo");
         content.Children.Add(heloBox);

         var copy = new Wpf.Ui.Controls.Button { Content = L("Copy _host name"), Margin = new Thickness(0, 0, 0, 6) };
         ControlHelp.Explain(copy, L("Copy the HELO host name"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(copy, "dns-records-ptr-copy");
         bool copyable = serverHostNameKnown_ && serverHostName_.Length > 0;
         copy.IsEnabled = copyable;
         if (!copyable)
         {
            copy.ToolTip = L("There is no configured host name to copy yet.");
            ToolTipService.SetShowOnDisabled(copy, true);
         }
         copy.Click += (s, e) => CopyAndConfirm_(copy, serverHostName_);
         content.Children.Add(copy);

         content.Children.Add(Paragraph(heloNote, Typography.Caption));

         content.Children.Add(Paragraph(L("Not checked from here: verifying the PTR needs the server's public IP address, which this panel cannot determine (behind NAT the machine does not know it either), and this page's checker reads TXT and MX records only. Test from outside instead: nslookup <your public IP> shows the PTR; it should name the host above."),
            Typography.Caption));

         var links = new StackPanel { Orientation = Orientation.Horizontal };
         links.Children.Add(PageLink("protocols", L("Open Protocols (host name)"), L("Open Protocols, whose SMTP tab has the Host name")));
         links.Children.Add(PageLink("antispam", L("Open Anti-spam settings (PTR and HELO checks)"), L("Open Anti-spam settings, which owns the inbound PTR and HELO checks")));
         content.Children.Add(links);

         return card;
      }

      // ---- the record block: type + host + value, each copyable, plus a check ----

      private sealed class RecordBlock
      {
         public TextBox HostBox;
         public TextBox ValueBox;
         public Wpf.Ui.Controls.Button CopyValue;
         public Wpf.Ui.Controls.Button Check;
         public StatusPill Mark;
         public TextBlock Result;

         public void SetResult(StatusLevel level, string word, string message)
            => SetResultOn(Mark, Result, level, word, message);
      }

      /// <summary>
      /// One record, presented the way a DNS provider's console asks for it: the
      /// type stated in words, the host and the value in separate read-only
      /// selectable boxes with their own copy buttons - values that must be copied
      /// exactly never live in labels - and a Check button with a four-way result.
      /// A warning passed in is rendered AT the copy buttons, because a danger
      /// stated in a footnote is a danger stated nowhere.
      /// </summary>
      private RecordBlock BuildRecordBlock(StackPanel parent, string idPrefix, string recordName,
         string host, string hostNote, string value, string copyWarning, bool valueCopyable = true, bool mx = false)
      {
         var block = new RecordBlock();

         // The type, then the name's caption with its note, each one whole text a
         // language can order, where one caption said both, "Record type: MX.
         // Host/Name - the domain itself ...:" (review finding F306).
         parent.Children.Add(Label(mx ? L("Record type: MX") : L("Record type: TXT")));
         parent.Children.Add(Label(hostNote != null ? F("Name - {0}", hostNote) : L("Name")));

         block.HostBox = ReadOnlyBox(0);
         block.HostBox.Text = host;
         block.HostBox.SetResourceReference(Control.ForegroundProperty, "TextFillColorPrimaryBrush");
         System.Windows.Automation.AutomationProperties.SetName(block.HostBox, F("{0} record host name", recordName));
         System.Windows.Automation.AutomationProperties.SetAutomationId(block.HostBox, idPrefix + "-host");
         parent.Children.Add(block.HostBox);

         parent.Children.Add(Label(mx ? L("MX value - the priority, then the name of the server that takes the mail:") : L("TXT value:")));

         // One line for an MX value, which is one line: its box was 46 pixels for
         // it below a 28-pixel name (review finding F306). A TXT value wraps.
         block.ValueBox = ReadOnlyBox(mx ? 0 : 44);
         block.ValueBox.Text = value;
         block.ValueBox.SetResourceReference(Control.ForegroundProperty, "TextFillColorPrimaryBrush");
         System.Windows.Automation.AutomationProperties.SetName(block.ValueBox, F("{0} record value", recordName));
         System.Windows.Automation.AutomationProperties.SetAutomationId(block.ValueBox, idPrefix + "-value");
         parent.Children.Add(block.ValueBox);

         if (copyWarning != null)
         {
            var notice = new InlineNotice
            {
               Level = StatusLevel.Warning,
               Text = F("Before you copy: {0}", copyWarning),
               Margin = new Thickness(0, 4, 0, 8)
            };
            System.Windows.Automation.AutomationProperties.SetName(notice, F("Warning before copying. {0}", copyWarning));
            parent.Children.Add(notice);
         }

         var copyRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 4) };

         // Copy name, beside Copy value, where Copy host stood beside the PTR card's
         // Copy host name, which copies the HELO name (review finding F306).
         var copyHost = new Wpf.Ui.Controls.Button { Content = L("Copy name") };   // per record: no access key, the rows are reached with the arrow keys
         System.Windows.Automation.AutomationProperties.SetName(copyHost, F("Copy name of the {0} record", recordName));
         System.Windows.Automation.AutomationProperties.SetAutomationId(copyHost, idPrefix + "-copy-host");
         copyHost.Click += (s, e) => CopyAndConfirm_(copyHost, block.HostBox.Text);
         copyRow.Children.Add(copyHost);

         block.CopyValue = new Wpf.Ui.Controls.Button { Content = L("Copy value"), Margin = new Thickness(8, 0, 0, 0), IsEnabled = valueCopyable };   // per record: no access key, the rows are reached with the arrow keys
         System.Windows.Automation.AutomationProperties.SetName(block.CopyValue, F("Copy value of the {0} record", recordName));
         System.Windows.Automation.AutomationProperties.SetAutomationId(block.CopyValue, idPrefix + "-copy-value");
         if (!valueCopyable)
         {
            block.CopyValue.ToolTip = L("The value cannot be produced from this machine, so there is nothing correct to copy.");
            ToolTipService.SetShowOnDisabled(block.CopyValue, true);
         }
         block.CopyValue.Click += (s, e) => { if (block.CopyValue.IsEnabled) CopyAndConfirm_(block.CopyValue, block.ValueBox.Text); };
         copyRow.Children.Add(block.CopyValue);

         block.Check = new Wpf.Ui.Controls.Button { Content = L("Check"), Margin = new Thickness(8, 0, 0, 0) };   // per record: no access key, the rows are reached with the arrow keys
         System.Windows.Automation.AutomationProperties.SetName(block.Check, F("Check whether the {0} record is published in DNS", recordName));
         System.Windows.Automation.AutomationProperties.SetAutomationId(block.Check, idPrefix + "-check");
         copyRow.Children.Add(block.Check);

         parent.Children.Add(copyRow);

         block.Mark = new StatusPill { Margin = new Thickness(0, 1, 10, 0), VerticalAlignment = VerticalAlignment.Top };
         block.Result = new TextBlock { TextWrapping = TextWrapping.Wrap };
         block.Result.SetResourceReference(StyleProperty, "TextCaption");
         parent.Children.Add(ResultRow(block.Mark, block.Result));

         block.SetResult(StatusLevel.Normal, L("Not checked"), L("Press \"Check\" to look the record up through this machine's resolver."));

         return block;
      }

      private static Grid ResultRow(StatusPill mark, TextBlock text)
      {
         var row = new Grid { Margin = new Thickness(0, 4, 0, 6) };
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         row.Children.Add(mark);
         Grid.SetColumn(text, 1);
         row.Children.Add(text);
         return row;
      }

      /// <summary>
      /// Colour, shape AND word, so none of the three is load-bearing on its own -
      /// the same three channels every status badge in this application uses.
      /// </summary>
      private static void SetResultOn(StatusPill mark, TextBlock text, StatusLevel level, string word, string message)
      {
         // The pill carries the colour, the shape and the check's own word - which
         // is more exact than the level's severity word ("Published", "Not served")
         // and is therefore the one to show; the sentence beside it says the rest.
         mark.Level = level;
         mark.Text = word;

         text.Text = message;
         System.Windows.Automation.AutomationProperties.SetName(text, word + ". " + message);
      }

      /// <summary>
      /// Runs one check: disables the button, shows "checking", runs the blocking
      /// lookup on a worker thread (it blocks for the resolver's full timeout when
      /// the record is missing - the common case right after publishing), then
      /// applies the verdict the worker prepared. DnsTxtLookup never throws; it
      /// reports through its results, and anything else that throws is said on the
      /// card as a lookup that failed. A check already running is the one returned.
      /// </summary>
      private static Task RunCheck(RecordBlock block, string host, Func<Action> work)
         => Running_(block.Check, () => RunCheckOnce_(block, host, work));

      private static async Task RunCheckOnce_(RecordBlock block, string host, Func<Action> work)
      {
         block.Check.IsEnabled = false;
         block.SetResult(StatusLevel.Information, L("Checking…"), F("Looking up {0}…", host));

         try
         {
            Action apply = await Task.Run(work);
            apply();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // A check that threw says so on its card, where its pill went on
            // saying "Checking…" and the exception reached the dispatcher from
            // the button's handler, which has no caller to catch it.
            block.SetResult(StatusLevel.Information, L("Lookup failed"),
               F("The check itself failed: {0} This says nothing about the record.", ex.Message));
         }
         finally
         {
            block.Check.IsEnabled = true;
         }
      }

      // The check each Check button has running, so that Check all records waits
      // for a check already running, where it returned at once and summed the
      // record up as its pill read then, "Checking…", and the summary was not
      // written again when the check ended (review finding F277). Keyed by the
      // button, which is made with its card: cards built again have new buttons,
      // with nothing running.
      private static readonly ConditionalWeakTable<UIElement, Task> running_ = new();

      /// <summary>
      /// The check <paramref name="button"/> has running, or the one
      /// <paramref name="start"/> starts, which is its running one until it ends.
      /// </summary>
      private static Task Running_(UIElement button, Func<Task> start)
      {
         if (running_.TryGetValue(button, out Task running) && !running.IsCompleted)
            return running;

         Task task = start();
         running_.AddOrUpdate(button, task);
         return task;
      }

      private static void SetLookupFailed(RecordBlock block, string host, string error)
      {
         block.SetResult(StatusLevel.Information, L("Lookup failed"),
            F("The DNS lookup for {0} itself failed: {1} This says nothing about the record - this machine could not get an answer from its DNS server. Check the network and try again.", host, error));
      }

      private static string PropagationAdvice =>
         L("Publish it at your DNS provider - or, if you just did, wait for propagation (usually minutes, sometimes hours) and check again.");

      private static string NotFoundAdvice(string host)
         => Sentences(F("No TXT record was found at {0}.", host), PropagationAdvice);

      /// <summary>Shows what IS published beside any mismatch verdict - the
      /// administrator cannot fix a difference they cannot see.</summary>
      private static string DescribeFound(List<string> records)
      {
         if (records == null || records.Count == 0)
            return "";

         string first = records[0];
         if (first.Length > 160)
            first = first.Substring(0, 160) + "…";
         // A sentence of its own, which the caller joins to the verdict's with Sentences.
         return records.Count > 1 ? F("Found: {0} (and {1} more)", first, records.Count - 1) : F("Found: {0}", first);
      }

      /// <summary>Reads one tag from a semicolon-separated tag=value record
      /// (DMARC, MTA-STS, TLS-RPT all use the shape). Whitespace-tolerant, like
      /// the comparison in DnsTxtLookup.</summary>
      private static string ParseTag(string record, string tag)
      {
         foreach (string trimmed in (record ?? "").Split(';').Select(part => part.Trim()))
         {
            int equals = trimmed.IndexOf('=');
            if (equals > 0 && trimmed.Substring(0, equals).Trim().Equals(tag, StringComparison.OrdinalIgnoreCase))
               return trimmed.Substring(equals + 1).Trim();
         }
         return null;
      }

      // ---- MX lookup for the MTA-STS policy preview -------------------------------

      /// <summary>
      /// MX lookup through the same Windows resolver DnsTxtLookup uses, with the
      /// same flags for the same reasons (bypass the cache: the administrator is
      /// often here moments after changing DNS; treat as FQDN: never let the
      /// machine's DNS suffix be appended). Lives here rather than in DnsTxtLookup
      /// because that class is deliberately TXT-only and is owned elsewhere.
      /// Returns the exchange host names ordered by preference, or an empty list;
      /// never throws.
      /// </summary>
      private static List<string> QueryMxRecords(string domain)
      {
         var results = new List<KeyValuePair<int, string>>();

         IntPtr records = IntPtr.Zero;
         try
         {
            int status = DnsApi.DnsQuery_W(domain.Trim(), DnsTypeMx,
               DnsQueryBypassCache | DnsQueryTreatAsFqdn, IntPtr.Zero, out records, IntPtr.Zero);

            if (status != 0)
               return new List<string>();

            int headerSize = Marshal.SizeOf<DnsRecordHeader>();

            for (IntPtr cursor = records; cursor != IntPtr.Zero;)
            {
               DnsRecordHeader header = Marshal.PtrToStructure<DnsRecordHeader>(cursor);

               if (header.Type == DnsTypeMx)
               {
                  // DNS_MX_DATAW: PWSTR pNameExchange at the start of the data
                  // area, WORD wPreference at the next pointer-aligned offset -
                  // IntPtr.Size in, on both x86 and x64, the same layout logic
                  // DnsTxtLookup documents for DNS_TXT_DATAW.
                  IntPtr data = cursor + headerSize;
                  IntPtr namePtr = Marshal.ReadIntPtr(data);
                  int preference = Marshal.ReadInt16(data, IntPtr.Size);

                  string exchange = namePtr != IntPtr.Zero ? Marshal.PtrToStringUni(namePtr) : null;
                  if (!string.IsNullOrWhiteSpace(exchange))
                  {
                     exchange = exchange.Trim().TrimEnd('.').ToLowerInvariant();
                     if (exchange.Length > 0)
                        results.Add(new KeyValuePair<int, string>(preference, exchange));
                  }
               }

               cursor = header.Next;
            }

            return results.OrderBy(r => r.Key).Select(r => r.Value).Distinct().ToList();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return new List<string>();
         }
         finally
         {
            if (records != IntPtr.Zero)
               DnsApi.DnsRecordListFree(records, DnsFreeRecordList);
         }
      }

      private const ushort DnsTypeMx = 0x000f;              // DNS_TYPE_MX
      private const uint DnsQueryBypassCache = 0x00000008;  // DNS_QUERY_BYPASS_CACHE
      private const uint DnsQueryTreatAsFqdn = 0x00001000;  // DNS_QUERY_TREAT_AS_FQDN
      private const int DnsFreeRecordList = 1;              // DNS_FREE_TYPE::DnsFreeRecordList

      /// <summary>The fixed header of a native DNS_RECORDW; same layout notes as
      /// the copy in DnsTxtLookup, which is TXT-only by design.</summary>
      [StructLayout(LayoutKind.Sequential)]
      private struct DnsRecordHeader
      {
         public IntPtr Next;
         public IntPtr Name;
         public ushort Type;
         public ushort DataLength;
         public uint Flags;
         public uint Ttl;
         public uint Reserved;
      }

      // ---- shared UI helpers -------------------------------------------------------

      /// <summary>One section of the page. Named Section and not Card because the
      /// component it builds is called that.</summary>
      private static Card Section(string title, out StackPanel content)
      {
         content = new StackPanel();
         var card = new Card { Title = title, Content = content };
         card.SetResourceReference(MarginProperty, "AppCardGap");
         return card;
      }

      private static TextBlock Paragraph(string text, double size)
      {
         var block = new TextBlock { Text = text, Margin = new Thickness(0, 0, 0, 6) };
         block.SetResourceReference(StyleProperty, size <= Typography.Caption ? "TextCaption" : "TextBody");
         return block;
      }

      private static TextBlock Label(string text)
      {
         var label = new TextBlock { Text = text, Margin = new Thickness(0, 8, 0, 4) };
         label.SetResourceReference(StyleProperty, "TextCaption");
         return label;
      }

      /// <summary>A selectable, read-only box in the style of the DKIM rotation's
      /// record display: values the user must copy exactly live in these rather
      /// than in labels, so they can be selected.</summary>
      private static TextBox ReadOnlyBox(double minHeight) => new()
      {
         IsReadOnly = true,
         AcceptsReturn = true,
         TextWrapping = TextWrapping.Wrap,
         FontSize = Typography.Caption,
         FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily),
         MinHeight = minHeight,
         Padding = new Thickness(6),
         Margin = new Thickness(0, 0, 0, 4),
         Background = System.Windows.Media.Brushes.Transparent
      };

      /// <summary>
      /// Puts the text on the clipboard and says so on the button that did it: for
      /// two seconds its words are "Copied" beside a tick, and a screen reader is
      /// told. A copy that failed - another program holding the clipboard - is said
      /// in a toast with the reason. The copy was best effort and said nothing
      /// either way, so nobody could tell whether it had worked (review finding
      /// F277).
      /// </summary>
      private static void CopyAndConfirm_(Wpf.Ui.Controls.Button button, string text)
      {
         if (!ClipboardWriter.TrySetText(text, out string error))
         {
            Toast.Failure(L("Could not copy: ") + error);
            return;
         }

         CopiedState.Show(button);
      }

      /// <summary>A copy button's moment of saying "Copied", and what it goes back to.</summary>
      private sealed class CopiedState
      {
         private static readonly ConditionalWeakTable<Wpf.Ui.Controls.Button, CopiedState> states_ = new();

         private object content_;
         private Wpf.Ui.Controls.IconElement icon_;
         private double minWidth_;
         private DispatcherTimer timer_;

         public static void Show(Wpf.Ui.Controls.Button button)
         {
            if (button == null)
               return;

            CopiedState state = states_.GetValue(button, b => new CopiedState { content_ = b.Content, icon_ = b.Icon, minWidth_ = b.MinWidth });
            state.timer_?.Stop();

            // Never narrower than it was, so that nothing beside it moves.
            button.MinWidth = Math.Max(state.minWidth_, button.ActualWidth);
            string copied = L("Copied");   // per record: a moment's state of a button without an access key
            button.Content = copied;
            button.Icon = new Wpf.Ui.Controls.SymbolIcon { Symbol = Wpf.Ui.Controls.SymbolRegular.CheckmarkCircle24 };
            Speak_(button, copied);

            state.timer_ = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            state.timer_.Tick += (s, e) =>
            {
               state.timer_.Stop();
               button.Content = state.content_;
               button.Icon = state.icon_;
               button.MinWidth = state.minWidth_;
            };
            state.timer_.Start();
         }

         /// <summary>Tells a screen reader, which hears nothing when a button's words change.</summary>
         private static void Speak_(UIElement element, string sentence)
         {
            try
            {
               AutomationPeer peer = UIElementAutomationPeer.FromElement(element) ?? UIElementAutomationPeer.CreatePeerForElement(element);
               peer?.RaiseNotificationEvent(AutomationNotificationKind.ActionCompleted,
                  AutomationNotificationProcessing.MostRecent, sentence, "hMailServerCopied");
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               // No automation client listening, or a peer that cannot be made:
               // the button says it on screen.
            }
         }
      }

      /// <summary>
      /// Runs every record's check of the domain on the page at once, then says how
      /// the domain stands in one notice at the worst level any check reached, a line
      /// per verdict naming its records (Services/DnsCheckSummary). Each card shows
      /// its own verdict as its Check does.
      /// </summary>
      private async Task CheckAll_()
      {
         DomainInfo domain = SelectedDomain();
         if (domain == null || checks_.Count == 0)
            return;

         List<RecordCheck> checks = checks_.ToList();
         checkAll_.IsEnabled = false;
         checkSummary_.Level = StatusLevel.Information;
         checkSummary_.Text = F("Checking every record of {0}…", domain.Name);
         checkSummary_.Visibility = Visibility.Visible;

         // Each check on its own, so that one that throws is summed up as a lookup
         // that failed and the others' verdicts are still said; a check a card
         // already has running is waited for (Running_).
         bool[] faulted;
         try
         {
            faulted = await Task.WhenAll(checks.Select(Faulted_));
         }
         finally
         {
            checkAll_.IsEnabled = SelectedDomain() != null;
         }

         // The cards were built again while the checks ran (Refresh, another
         // domain): what they found is of cards no longer on the page.
         if (!ReferenceEquals(checks[0], checks_.FirstOrDefault()))
            return;

         var results = checks.Select((check, i) => DnsCheckSummary.Verdict(check.Record, check.Mark.Level, check.Mark.Text, faulted[i])).ToList();
         checkSummary_.Level = DnsCheckSummary.Worst(results);
         checkSummary_.Text = DnsCheckSummary.Text(domain.Name, results);
      }

      /// <summary>
      /// Runs one check for Check all records: true when it threw, which the
      /// awaiting of every check at once would have thrown again into the button's
      /// async handler, which has no caller to catch it.
      /// </summary>
      private static async Task<bool> Faulted_(RecordCheck check)
      {
         try
         {
            await check.Run();
            return false;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return true;
         }
      }

      /// <summary>A button that opens the page owning a related setting - the same
      /// escape hatch the spam overview and the setup checklist use.</summary>
      private static FrameworkElement PageLink(string page, string caption, string accessibleName)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = caption,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            Margin = new Thickness(0, 2, 8, 0),
            HorizontalAlignment = HorizontalAlignment.Left,
            ToolTip = accessibleName
         };
         ButtonStyles.CompactPageLink(button);
         ControlHelp.NameLink(button, caption, accessibleName);
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "dns-records-open-" + page);
         button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo(page);
         return button;
      }
   }
}
