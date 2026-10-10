// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;
using Wpf.Ui.Appearance;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   public partial class MainWindow
   {
      private const string RegistryPath = @"Software\hMailServer\ControlPanel";

      /// <summary>
      /// Where the palette's recently-visited and most-used history is kept.
      /// Beside the window bounds, under the same key, because it is the same
      /// kind of thing: a per-user convenience that is worth nothing on another
      /// machine and must never be required for the application to work.
      /// </summary>
      private const string PaletteUsageValue = "PaletteUsage";

      /// <summary>
      /// Where the administrator's choice to collapse the sidebar is kept. A
      /// per-user convenience like the window bounds beside it; the window also
      /// collapses the sidebar on its own below DesignTokens.Shell.CollapseSidebarBelow,
      /// and that is not recorded, because it is the window's size that decided it.
      /// </summary>
      private const string SidebarCollapsedValue = "SidebarCollapsed";

      /// <summary>
      /// Where the help pane's state is kept: whether it is open, and the width
      /// the administrator last left it at. Beside the window bounds, the
      /// palette's history and the sidebar's collapse, and for the same reason:
      /// a per-user convenience, never required for the program to work.
      /// </summary>
      private const string HelpPaneOpenValue = "HelpPaneOpen";
      private const string HelpPaneWidthValue = "HelpPaneWidth";

      /// <summary>
      /// The wiki the help pane links to when the server it is connected to
      /// has no HelpWikiBaseUrl setting to say otherwise - an older server. A
      /// server that has the setting is believed, an empty one included.
      /// </summary>
      private const string DefaultHelpWikiBaseUrl = "https://gitlab.com/hmailserver/hmailserver/-/wikis/";

      private bool helpOpen_;
      private bool helpWanted_;
      private double helpWidth_ = 380;

      private readonly Dictionary<string, UserControl> pageCache_ = new();
      private readonly List<RadioButton> railItems_ = new();
      private bool sidebarCollapsedByUser_;
      private bool railShowing_;
      private bool sidebarShown_;
      private readonly Dictionary<string, Func<UserControl>> pageFactories_ = new();
      private PaletteUsage paletteUsage_ = new();
      private string currentPage_;
      private bool arrivedFromSearch_;
      private bool connected_;

      // A pending-bar refresh is queued and has not run yet; see the constructor.
      private volatile bool pendingRefreshQueued_;

      // Set while the application navigates on the user's behalf rather than at their
      // request, so that "Most used" counts choices and not startup.
      private bool automaticNavigation_;

      public MainWindow()
      {
         InitializeComponent();

         // The default text colour for the whole window, delivered by property
         // inheritance rather than by an implicit TextBlock style. App.xaml
         // carried such a style as a "global contrast guarantee" until it was
         // found to be the cause of the one look complaint the forum ever
         // recorded: an app-level implicit style also reaches the TextBlocks
         // that ContentPresenter generates inside control templates, and a
         // style setter outranks an inherited value - so it overwrote WPF-UI's
         // on-accent button text with the ordinary body colour, which in the
         // light theme is near-black on accent blue. Inheritance gives bare
         // TextBlocks the same default while letting templates and Appearance
         // setters override it, which is the entire difference.
         SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");

         Services.Toast.Init(RootSnackbar);
         ApplySavedTheme();
         RestoreWindowBounds();
         LoadPaletteUsage();
         LoadTourProgress_();
         WireTourCard_();
         RegisterPages();
         BuildNavTree();
         BuildNavRail_();
         LoadSidebarPreference_();
         LoadHelpPreference_();
         HelpPanel.CloseRequested += (s, e) => CloseHelp_();

         // Before sign-in the navigation and the search cannot be used, and are
         // not shown: the sidebar and the search field follow whether they can be
         // used (ApplySidebar_, ShowSearch_), so that the sign-in page is one card
         // and not sixty pages that look ready to open and do nothing when they
         // are clicked. The help button stays, dimmed, beside the theme toggle.
         NavTree.IsEnabledChanged += (s, e) => ApplySidebar_();
         SearchButton.IsEnabledChanged += (s, e) => ShowSearch_();
         NavTree.IsEnabled = false;
         NavRail.IsEnabled = false;
         SearchButton.IsEnabled = false;
         HelpButton.IsEnabled = false;
         ApplySidebar_();
         ShowSearch_();
         ShowLinkState_(LinkState.NotConnected);
         ContentHost.Content = new ConnectView(OnConnected);

         Closing += (s, e) => SaveWindowBounds();
         Closing += (s, e) => SavePaletteUsage();
         Closing += (s, e) => SaveHelpPreference_();

         ServerSession.Reconnected += OnSessionReconnected;
         Closing += (s, e) => ServerSession.Reconnected -= OnSessionReconnected;

         ServerSession.LinkStateChanged += OnLinkStateChanged;
         Closing += (s, e) => ServerSession.LinkStateChanged -= OnLinkStateChanged;

         // Every save through IniFeatureStore - any page's - refreshes the bar at the
         // foot, so it follows the saves without each page having to tell the window.
         // ONCE per save, not once per field: the writes of a save all raise Written
         // before the dispatcher runs anything queued, so one refresh is queued for
         // all of them, and it reuses the list the page read after its save
         // (IniFeatureStore.PendingNamesSinceLastWrite) rather than asking again.
         Action written = () =>
         {
            if (pendingRefreshQueued_)
               return;
            pendingRefreshQueued_ = true;
            Dispatcher.BeginInvoke(new Action(() =>
            {
               pendingRefreshQueued_ = false;
               RefreshPendingBar_(fresh: false);
            }), System.Windows.Threading.DispatcherPriority.Background);
         };
         IniFeatureStore.Written += written;
         Closing += (s, e) => IniFeatureStore.Written -= written;

         // Every page header's help button lands here: the event bubbles out of
         // whichever page raised it, so no page has to be wired to the shell.
         AddHandler(Views.Scaffold.PageHeader.ShowTourEvent, new RoutedEventHandler(OnPageHelpRequested_));

         // The sidebar follows the window's width: below the threshold it is the
         // rail whatever the administrator chose, above it their choice stands.
         SizeChanged += (s, e) => ApplySidebar_();

         // Covers every way the theme can change after ApplySavedTheme's own
         // explicit call: the toggle button, and the OS switching while the
         // application is still following the system theme.
         ApplicationThemeManager.Changed += OnThemeChanged;
         Closing += (s, e) => ApplicationThemeManager.Changed -= OnThemeChanged;

         // High Contrast can be switched on and off while the window is open
         // (Left Alt + Left Shift + Print Screen); the theme follows it both ways.
         SystemParameters.StaticPropertyChanged += OnSystemParametersChanged;
         Closing += (s, e) => SystemParameters.StaticPropertyChanged -= OnSystemParametersChanged;

         // The two keys a running tour claims - F6 to reach its card, Escape to
         // leave it, and Escape only while the keyboard is already inside the
         // card, so the page keeps its own - then the keys the window answers on
         // every page (MainWindow.Keys.cs): Ctrl+K for the palette, F1 for the
         // help, F5, Ctrl+F, Ctrl+S and F6, and Alt+Left and Alt+Right for Back
         // and Forward (MainWindow.History.cs, which also takes the mouse's side
         // buttons).
         PreviewKeyDown += (s, e) =>
         {
            if (HandleTourKey_(e) || HandleShellKey_(e))
               e.Handled = true;
         };
         WireShellKeys_();
         WireHistory_();
         WireHelpTabs_();

         // Opened again by hMailServer.Updater after an administrator's install of an
         // update (docs/ServiceAccount.md, "The live update"): sign in with the
         // handoff the Control Panel that closed for the installer wrote, and show
         // the outcome - or the sign-in page with it, where there is no handoff.
         string[] args = Environment.GetCommandLineArgs();
         int afterUpdate = Array.IndexOf(args, "--after-update"); // no-loc: a switch
         if (afterUpdate >= 0 && args.Length > afterUpdate + 1 && AdministratorUpdate.IsWorkId(args[afterUpdate + 1]))
         {
            string workId = args[afterUpdate + 1];
            Loaded += async (s, e) => await ResumeAfterUpdate_(workId);
         }

         // Optional auto-connect: hMailCP.exe /connect <host> <user> <password>
         int flag = Array.IndexOf(args, "/connect");
         if (flag >= 0 && args.Length >= flag + 4)
         {
            Loaded += (s, e) =>
            {
               var session = new ServerSession();
               if (session.Connect(args[flag + 1], args[flag + 2], args[flag + 3], out _))
               {
                  ServerSession.SetCurrent(session);
                  OnConnected();
               }
            };
         }
      }

      private async System.Threading.Tasks.Task ResumeAfterUpdate_(string workId)
      {
         // The installation as the service manager has it - where the stage wrote -
         // or else the one this program is in.
         string installation;
         if (!AdministratorUpdate.InstallationFrom(WindowsServiceInfo.ExecutableFrom(WindowsServiceInfo.Query().PathName), out installation))
            installation = System.IO.Path.GetDirectoryName(System.IO.Path.GetDirectoryName(Environment.ProcessPath ?? ""));
         if (string.IsNullOrEmpty(installation))
            return;
         string work = AdministratorUpdate.WorkFolder(installation, workId);

         AdministratorUpdate.Outcome outcome = AdministratorUpdate.ParseOutcome(AdministratorUpdate.ReadShared(System.IO.Path.Combine(work, "outcome.txt")));
         byte[] entropy = AfterUpdateHandoff.ReadEntropy(work);
         AfterUpdateHandoff.DeleteEntropy(work);
         bool handedOver = AfterUpdateHandoff.TryRead(workId, entropy, out string host, out string user, out string password);
         if (entropy != null)
            Array.Clear(entropy, 0, entropy.Length);

         string banner = outcome != null ? AdministratorUpdate.SignInBanner(outcome) : null;
         StatusLevel level = outcome != null ? AdministratorUpdate.LevelOf(outcome) : StatusLevel.Normal;
         if (!handedOver)
         {
            ContentHost.Content = new ConnectView(OnConnected, banner, level);
            return;
         }

         // The service may still be starting: tried for a minute.
         var session = new ServerSession();
         bool connected = false;
         for (int attempt = 0; attempt < 20 && !connected; attempt++)
         {
            connected = session.Connect(host, user, password, out _);
            if (!connected)
               await System.Threading.Tasks.Task.Delay(3000);
         }
         password = null;
         if (!connected)
         {
            ContentHost.Content = new ConnectView(OnConnected, banner, level);
            return;
         }

         ServerSession.SetCurrent(session);
         OnConnected();
         // The Updates page, where the install was started and its outcome is said.
         NavigateTo("updates"); // no-loc: a page key
         if (outcome != null && ContentHost.Content is FeatureSettingsView updates)
            updates.ShowInstallOutcome(outcome);
      }

      private void RegisterPages()
      {
         pageFactories_["welcome"] = () => new WelcomeView();
         pageFactories_["dashboard"] = () => new DashboardView();
         pageFactories_["status"] = () => new StatusView();
         pageFactories_["queue"] = () => new QueueView();
         pageFactories_["stalledmail"] = () => new StalledMailView();
         pageFactories_["messagetrace"] = () => new MessageTraceView();
         pageFactories_["suppressions"] = () => new SuppressionListView();
         pageFactories_["audit"] = () => new AuditTrailView();
         pageFactories_["discovery"] = () => new DiscoveryView();
         pageFactories_["logs"] = () => new LogsView();
         pageFactories_["domains"] = () => new DomainsView();
         pageFactories_["rules"] = () => new RulesView();
         pageFactories_["protocols"] = () => new ServerSettingsView(ServerSettingsView.Section.Protocols);
         pageFactories_["delivery"] = () => new ServerSettingsView(ServerSettingsView.Section.Delivery);
         pageFactories_["routes"] = () => new RoutesView();
         pageFactories_["remotedomains"] = () => CollectionSpecs.RemoteDomains();
         pageFactories_["publicfolders"] = () => new PublicFoldersView();
         pageFactories_["spamoverview"] = () => new SpamOverviewView();
         pageFactories_["quarantine"] = () => new QuarantineView();
         pageFactories_["antispam"] = () => new ServerSettingsView(ServerSettingsView.Section.AntiSpam);
         pageFactories_["surbl"] = () => CollectionSpecs.SurblServers();
         pageFactories_["dnsbl"] = () => CollectionSpecs.DnsBlackLists();
         pageFactories_["spamwhitelist"] = () => CollectionSpecs.SpamWhiteList();
         pageFactories_["blockedsenders"] = () => CollectionSpecs.BlockedSenders();
         pageFactories_["senderlist"] = () => CollectionSpecs.SenderList();
         pageFactories_["greylistwhitelist"] = () => CollectionSpecs.GreyListWhiteList();
         pageFactories_["greylistbypass"] = () => CollectionSpecs.GreyListBypass();
         pageFactories_["domainpolicylist"] = () => new DomainPolicyListView();
         pageFactories_["virusoverview"] = () => new VirusOverviewView();
         pageFactories_["antivirus"] = () => new ServerSettingsView(ServerSettingsView.Section.AntiVirus);
         pageFactories_["blockedattachments"] = () => CollectionSpecs.BlockedAttachments();
         pageFactories_["logging"] = () => new ServerSettingsView(ServerSettingsView.Section.Logging);
         pageFactories_["tlsoverview"] = () => new TlsOverviewView();
         pageFactories_["tls"] = () => new ServerSettingsView(ServerSettingsView.Section.Tls);
         pageFactories_["autoban"] = () => new ServerSettingsView(ServerSettingsView.Section.AutoBan);
         pageFactories_["performance"] = () => new ServerSettingsView(ServerSettingsView.Section.Performance);
         pageFactories_["advanced"] = () => new ServerSettingsView(ServerSettingsView.Section.Advanced);
         pageFactories_["adminaccess"] = () => new ServerSettingsView(ServerSettingsView.Section.AdminAccess);
         pageFactories_["webmail"] = () => new ServerSettingsView(ServerSettingsView.Section.Webmail);
         pageFactories_["groups"] = () => CollectionSpecs.Groups();
         pageFactories_["servermessages"] = () => CollectionSpecs.ServerMessages();
         pageFactories_["scripts"] = () => new ScriptsView();
         pageFactories_["certs"] = () => new SslCertificatesView();
         pageFactories_["ports"] = () => new TcpIpPortsView();
         pageFactories_["ipranges"] = () => new IPRangesView();
         pageFactories_["relays"] = () => new IncomingRelaysView();
         pageFactories_["security"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Security);
         pageFactories_["acme"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Automation);
         pageFactories_["api"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Integration);
         pageFactories_["apikeys"] = () => new ApiKeysView();
         pageFactories_["hardening"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Hardening);
         pageFactories_["authentication"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Authentication);
         pageFactories_["dns"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Dns);
         pageFactories_["webservices"] = () => new FeatureSettingsView(FeatureSettingsView.Section.WebServices);
         pageFactories_["backup"] = () => new BackupView();
         pageFactories_["updates"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Updates);
         pageFactories_["profiles"] = () => new ProfilesView();
         pageFactories_["mxquery"] = () => new MxQueryView();
         pageFactories_["sendout"] = () => new SendoutView();
         pageFactories_["diagnostics"] = () => new DiagnosticsView();
         pageFactories_["ldap"] = () => new LdapSettingsView();
         pageFactories_["directorysync"] = () => new DirectorySyncView();
         pageFactories_["externalsetup"] = () => new ExternalSetupView();
         pageFactories_["dnsrecords"] = () => new DnsRecordsView();
         pageFactories_["roles"] = () => new RolesView();
         pageFactories_["journaling"] = () => new JournalingView();
         pageFactories_["about"] = () => new AboutView();
      }

      /// <summary>
      /// The tree as the signed-in credential should see it. A page a role may
      /// not use is left out rather than shown and refused; a credential the
      /// server did not describe - an older server, a property that threw -
      /// sees the whole tree, exactly as before roles existed. This decides
      /// what is DRAWN and never what is ALLOWED, which stays the server's.
      /// </summary>
      private static IReadOnlyList<NavNode> VisibleRoots_()
      {
         ServerSession session = ServerSession.Current;

         if (session == null || session.Permissions == null)
            return NavigationMap.Roots;

         return NavigationMap.VisibleRoots(session.May);
      }

      /// <summary>
      /// Builds the sidebar from <see cref="NavigationMap"/>.
      ///
      /// The tree used to be written out here as nested Item/Group calls, and its
      /// comment said as much: it reproduced the classic Administrator layout,
      /// organised around where the product keeps things rather than around what
      /// an administrator is trying to do, with thirty of the forty-two pages
      /// buried under a single catch-all group three levels deep. The structure
      /// now lives in a WPF-free data file for two reasons: it can be tested
      /// (every registered page must appear exactly once, no page title may
      /// change), and the palette can search the same structure the sidebar
      /// draws instead of reverse-engineering it from TreeViewItem headers.
      ///
      /// This method deliberately contains no page names. If a name appears here
      /// again, the tree and the palette have two sources of truth.
      /// </summary>
      private void BuildNavTree()
      {
         NavTree.Items.Clear();

         foreach (NavNode node in VisibleRoots_())
            NavTree.Items.Add(BuildNavItem(node));
      }

      /// <summary>
      /// A group header is a glyph and its title; a page header stays plain
      /// text. The glyph is resolved from the map's string name so that
      /// NavigationMap stays WPF-free, and a name that does not resolve
      /// renders no icon rather than failing - a missing glyph is a cosmetic
      /// bug, not a broken sidebar.
      /// </summary>
      private static object BuildNavHeader_(NavNode node)
      {
         // A page's row is a trimming TextBlock rather than the bare string: the
         // presenter's own TextBlock does not trim, so in a narrow window
         // "Diagnosing stalled mail" and "Journaling and dual delivery" were cut
         // mid-word with no ellipsis while the group rows above them had one.
         // The full title is in the row's tool tip and its accessible name.
         if (string.IsNullOrEmpty(node.Icon))
            return new TextBlock { Text = L(node.Title), TextTrimming = TextTrimming.CharacterEllipsis };

         if (!System.Enum.TryParse(node.Icon, out Wpf.Ui.Controls.SymbolRegular symbol))
         {
            // Right for the user, wrong to be quiet about: Enum.TryParse failing
            // silently is how a mistyped name once shipped one group bare among
            // seven with glyphs, and no test can catch it because the tests are
            // deliberately WPF-free and never see this enum. Loud in DEBUG,
            // still just a missing icon in Release.
            System.Diagnostics.Debug.Fail(
               "Navigation icon \"" + node.Icon + "\" on \"" + node.Title +
               "\" is not a member of Wpf.Ui.Controls.SymbolRegular.");
            return L(node.Title);
         }

         // A Grid, not a horizontal StackPanel. A StackPanel measures its children
         // with infinite width along the stacking direction, so the label below
         // always reported its full desired width and was CLIPPED by the sidebar
         // rather than trimmed - "Monitoring & troubleshooting" ended mid-word with
         // no ellipsis to say so. A star column hands the label the width that is
         // actually left, which is what TextTrimming needs in order to do anything.
         var header = new Grid();
         header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

         // No explicit Foreground: SymbolIcon's Foreground inherits from the
         // TreeViewItem exactly as the TextBlock's does, so the glyph dims at
         // rest and brightens on hover and selection together with its label.
         // It was pinned to the secondary brush here once, which left it the
         // only part of the row that never responded to state.
         var icon = new Wpf.Ui.Controls.SymbolIcon(symbol)
         {
            FontSize = 16,
            Margin = new Thickness(0, 0, 10, 0),
            VerticalAlignment = VerticalAlignment.Center
         };
         header.Children.Add(icon);

         var label = new TextBlock
         {
            Text = L(node.Title),
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis
         };

         Grid.SetColumn(label, 1);
         header.Children.Add(label);

         return header;
      }

      private TreeViewItem BuildNavItem(NavNode node)
      {
         var item = new TreeViewItem { Header = BuildNavHeader_(node) };

         // The purpose statement as the tool tip and as the accessible help text.
         // A page whose title needs the manual has failed; this is the cheapest
         // possible fix, and it reaches a screen reader as well as a mouse.
         if (!string.IsNullOrEmpty(node.Purpose))
         {
            // The title leads, then the purpose. The title is in the tip because the
            // label can be ellipsized when the pane is narrow, and a tool tip that
            // explains a name the reader cannot finish reading is answering the
            // wrong question.
            item.ToolTip = L(node.Title) + " - " + L(node.Purpose);
            System.Windows.Automation.AutomationProperties.SetHelpText(item, L(node.Purpose));
         }
         else
         {
            item.ToolTip = L(node.Title);
         }

         System.Windows.Automation.AutomationProperties.SetName(item, L(node.Title));

         if (node.IsPage)
         {
            item.Tag = node.Key;
            item.FontWeight = FontWeights.Normal;
            System.Windows.Automation.AutomationProperties.SetAutomationId(item, "nav-" + node.Key);
            return item;
         }

         // Groups keep the automation id they had, derived from the title, so
         // that anything driving this interface by id keeps working for the
         // groups that kept their names.
         //
         // The Tag contract for the whole tree: a page's Tag is its key (a
         // string), a group's Tag is its NavNode. AllLeaves, AllGroups and
         // NavTree_SelectedItemChanged all discriminate on "Tag is string", so
         // a group's Tag must never be one. The group's node is here so that
         // RevealGroup can find the group without inspecting its Header -
         // matching on "Header as string" broke the moment headers became
         // icon-plus-text panels.
         item.Tag = node;
         item.IsExpanded = true;
         item.FontWeight = FontWeights.SemiBold;
         System.Windows.Automation.AutomationProperties.SetAutomationId(item, "navgroup-" + NavigationMap.Slug(node.Title));

         // The group's pages are rows of the shell's own as well. A TreeView
         // gives its ItemContainerStyle to its own items alone, and hands it on
         // to a child only when the child is a container the tree made for a
         // data item - these rows are TreeViewItems added as they are, their own
         // containers - so every page inside a group took WPF-UI's row: the
         // Windows accent's bar outside a shorter highlight in place of the
         // brand pill, the primary text at rest where the group rows have the
         // secondary, and no hover rule or keyboard border of the shell's.
         item.ItemContainerStyle = NavTree.ItemContainerStyle;

         foreach (NavNode child in node.Children)
            item.Items.Add(BuildNavItem(child));

         return item;
      }

      /// <summary>
      /// The rail: one glyph per top-level group, from the same map the tree is
      /// built from and in the same order, so the collapsed sidebar is the
      /// expanded one with its words folded away and not a second navigation.
      /// A group without a glyph gets its initial, so no group disappears when
      /// the sidebar collapses. Choosing a glyph expands the sidebar on that
      /// group; the page on screen is never changed by the rail.
      /// </summary>
      private void BuildNavRail_()
      {
         NavRail.Children.Clear();
         railItems_.Clear();

         foreach (NavNode node in VisibleRoots_())
         {
            object glyph = null;
            if (!string.IsNullOrEmpty(node.Icon) && System.Enum.TryParse(node.Icon, out Wpf.Ui.Controls.SymbolRegular symbol))
               glyph = new Wpf.Ui.Controls.SymbolIcon(symbol) { FontSize = 18 };

            glyph ??= new TextBlock
            {
               Text = L(node.Title).Length > 0 ? L(node.Title).Substring(0, 1) : "",
               FontSize = Typography.Body,
               FontWeight = FontWeights.SemiBold
            };

            var item = new RadioButton { Tag = glyph, GroupName = "nav-rail", ToolTip = L(node.Title) };
            item.SetResourceReference(StyleProperty, "NavRailItem");
            System.Windows.Automation.AutomationProperties.SetName(item, L(node.Title));
            System.Windows.Automation.AutomationProperties.SetAutomationId(item, "navrail-" + NavigationMap.Slug(node.Title));
            if (!string.IsNullOrEmpty(node.Purpose))
               System.Windows.Automation.AutomationProperties.SetHelpText(item, L(node.Purpose));

            // A top-level page (the welcome page, the dashboard) has no group to
            // reveal: its glyph opens it, and the rail stays as it is.
            string title = node.Title;
            string page = node.IsPage ? node.Key : null;
            NavNode group = node;
            item.Click += (s, e) =>
            {
               if (page != null)
               {
                  NavigateTo(page);
                  return;
               }

               if (TooNarrowForTree_)
               {
                  // The tree cannot be shown at this width, so the group's pages
                  // open beside the glyph (MainWindow.NavFlyout.cs).
                  ShowGroupFlyout_(group, item, System.Windows.Controls.Primitives.PlacementMode.Right);
               }
               else
               {
                  sidebarCollapsedByUser_ = false;
                  SaveSidebarPreference_();
                  ApplySidebar_();
                  RevealGroup(title);
               }

               // The click checked the glyph. The mark is the page on screen's,
               // which the click did not change, so it goes back there.
               UpdateRail_(currentPage_);
            };

            railItems_.Add(item);
            NavRail.Children.Add(item);
         }
      }

      /// <summary>Marks the rail glyph of the group the page on screen lives in.</summary>
      private void UpdateRail_(string key)
      {
         IReadOnlyList<NavNode> trail = NavigationMap.PathTo(key);
         string group = trail.Count > 0 ? L(trail[0].Title) : null;

         foreach (RadioButton item in railItems_)
            item.IsChecked = group != null && string.Equals(item.ToolTip as string, group, StringComparison.Ordinal);
      }

      /// <summary>
      /// Shows the tree or the rail, or neither before sign-in. The rail whenever
      /// the window is narrower than the threshold, or the administrator
      /// collapsed the sidebar; the tree otherwise. The column's three widths
      /// move together, because a star width with the old MinWidth would hold
      /// the rail open.
      /// </summary>
      private void ApplySidebar_()
      {
         bool rail = sidebarCollapsedByUser_ || ActualWidth < DesignTokens.Shell.CollapseSidebarBelow;

         // The sidebar is shown while it can be used. Before sign-in, and after a
         // second factor that was refused, the tree is disabled, and then the
         // whole sidebar - the tree or the rail, its footer and the toggle that
         // opens it - is not shown at all: a disabled tree drawn as it is drawn
         // when it works offered sixty pages that did nothing when clicked, with
         // nothing to say why (the owner's choice of 9 October 2026 was to hide
         // it rather than dim it). NavTree.IsEnabled is the one switch - the
         // constructor and a refused second factor clear it, OnConnected sets it
         // - and its IsEnabledChanged brings the sidebar here.
         bool shown = NavTree.IsEnabled;

         if (rail == railShowing_ && shown == sidebarShown_ && IsLoaded)
            return;

         railShowing_ = rail;
         sidebarShown_ = shown;

         SidebarPane.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
         SidebarToggle.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
         ContentSurface.CornerRadius = shown ? new CornerRadius(8, 0, 0, 0) : new CornerRadius(0);

         if (!shown)
         {
            SidebarColumn.MinWidth = 0;
            SidebarColumn.MaxWidth = 0;
            SidebarColumn.Width = new GridLength(0);
         }
         else if (rail)
         {
            SidebarColumn.MinWidth = DesignTokens.Shell.RailWidth;
            SidebarColumn.MaxWidth = DesignTokens.Shell.RailWidth;
            SidebarColumn.Width = new GridLength(DesignTokens.Shell.RailWidth);
         }
         else
         {
            SidebarColumn.MaxWidth = DesignTokens.Shell.SidebarMaxWidth;
            SidebarColumn.MinWidth = DesignTokens.Shell.SidebarMinWidth;
            SidebarColumn.Width = new GridLength(0.28, GridUnitType.Star);
         }

         NavTree.Visibility = rail ? Visibility.Collapsed : Visibility.Visible;
         NavRail.Visibility = rail ? Visibility.Visible : Visibility.Collapsed;
         SidebarFooter.Visibility = rail ? Visibility.Collapsed : Visibility.Visible;

         UpdateSidebarToggle_();
      }

      /// <summary>
      /// Shows the search field while it can be used, as the sidebar is
      /// (ApplySidebar_): before sign-in it opens nothing, and drawn as it is when
      /// it works it said otherwise. Ctrl+K waits for sign-in as well.
      /// </summary>
      private void ShowSearch_()
      {
         SearchButton.Visibility = SearchButton.IsEnabled ? Visibility.Visible : Visibility.Collapsed;
      }

      /// <summary>
      /// Points the sidebar toggle at what a click will do, in the tool tip and
      /// the accessible name alike - the same rule as the theme toggle.
      /// </summary>
      private void UpdateSidebarToggle_()
      {
         string action = railShowing_ ? L("Expand the navigation") : L("Collapse the navigation");
         SidebarToggle.ToolTip = action;
         System.Windows.Automation.AutomationProperties.SetName(SidebarToggle, action);
      }

      private void SidebarToggle_Click(object sender, RoutedEventArgs e)
      {
         // Toggling from the state on screen, not from the stored choice: on a
         // narrow window the rail is showing whatever was chosen, and a click
         // there must open the sidebar, not record a collapse nobody can see.
         sidebarCollapsedByUser_ = !railShowing_;

         if (!sidebarCollapsedByUser_ && ActualWidth < DesignTokens.Shell.CollapseSidebarBelow
             && WindowState == WindowState.Normal)
         {
            // Too narrow for the tree: widen the window to the threshold rather
            // than leave a click that does nothing.
            Width = Math.Max(Width, DesignTokens.Shell.CollapseSidebarBelow);
         }

         SaveSidebarPreference_();
         ApplySidebar_();
      }

      private void LoadSidebarPreference_()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            sidebarCollapsedByUser_ = string.Equals(key?.GetValue(SidebarCollapsedValue) as string, "1", StringComparison.Ordinal);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            sidebarCollapsedByUser_ = false;
         }

         ApplySidebar_();
      }

      private void SaveSidebarPreference_()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath);
            key?.SetValue(SidebarCollapsedValue, sidebarCollapsedByUser_ ? "1" : "0");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      private IEnumerable<TreeViewItem> AllLeaves(ItemCollection items)
      {
         foreach (TreeViewItem item in items)
         {
            if (item.Tag is string)
               yield return item;
            foreach (TreeViewItem child in AllLeaves(item.Items))
               yield return child;
         }
      }

      /// <summary>Selects the navigation leaf with the given key, expanding its
      /// parent groups. Used by the Welcome page quick-action tiles, the palette
      /// and the breadcrumb's related-page links.</summary>
      public void NavigateTo(string key) => NavigateTo(key, false);

      /// <summary>
      /// <paramref name="fromSearch"/> marks the arrival as a palette jump, which
      /// the breadcrumb says out loud once. Landing on a page with no idea how
      /// you got there is the specific way a command palette stops teaching the
      /// structure and starts replacing it.
      /// </summary>
      private void NavigateTo(string key, bool fromSearch)
      {
         foreach (TreeViewItem leaf in AllLeaves(NavTree.Items)
            .Where(l => string.Equals(l.Tag as string, key, StringComparison.OrdinalIgnoreCase)))
         {
            arrivedFromSearch_ = fromSearch;

            for (var parent = leaf.Parent as TreeViewItem; parent != null; parent = parent.Parent as TreeViewItem)
               parent.IsExpanded = true;

            // Re-selecting the page that is already selected raises no
            // SelectedItemChanged, so the breadcrumb would keep saying whatever
            // it said before - including a stale "from search" marker.
            if (leaf.IsSelected)
               UpdateBreadcrumb(key);
            else
               leaf.IsSelected = true;

            // The page's whole group on screen, not only its row at the sidebar's
            // edge (MainWindow.GroupScroll.cs, review finding F151).
            ShowGroupOf_(leaf);
            return;
         }

         // A page of the map that this role's tree leaves out: said, where the
         // link, the palette row, the Welcome tile or the tour step that asked
         // for it did nothing at all (review finding F079).
         NavNode withheld = NavigationMap.Find(key);
         if (withheld != null && connected_)
            Services.Toast.Info(F("Your role cannot open {0}.", L(withheld.Title)), L("Control Panel"));
      }

      private void Search_Click(object sender, RoutedEventArgs e) => ShowPalette();

      private void ShowPalette()
      {
         if (!connected_)
            return;

         var palette = new NavigationPalette(this, paletteUsage_, currentPage_);
         palette.ShowDialog();

         if (palette.ShowShortcuts)
         {
            ShowKeyboardShortcuts_();
            return;
         }

         // A tour row before a page row: a tour opens the page it starts on
         // itself, and navigating first would put the reader on a page and then
         // move them off it.
         if (palette.SelectedTour != null)
         {
            StartTour(palette.SelectedTour);
            return;
         }

         if (palette.SelectedPage != null)
         {
            NavigateToTab(palette.SelectedPage, palette.SelectedTab, true);
            RevealSetting_(palette.SelectedPage, palette.SelectedSetting, palette.SelectedSettingLabel);
         }
      }

      /// <summary>
      /// A setting chosen in the palette: once its page is on screen and laid out,
      /// the page is asked to show it - its tab chosen, its row in view, the
      /// keyboard in its editor and a ring round it once (Views/IRevealSetting) -
      /// where the page opened on its first tab with nothing marked (review finding
      /// F274). Not when the page did not open: a Stay at the question about unsaved
      /// changes keeps the reader where they were. The setting's label goes with its
      /// key, which more than one row of a page can share.
      /// </summary>
      private void RevealSetting_(string page, string setting, string label)
      {
         if (string.IsNullOrEmpty(setting))
            return;

         Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() =>
         {
            if (!string.Equals(currentPage_, page, StringComparison.OrdinalIgnoreCase) || ContentHost?.Content is not IRevealSetting reveal)
               return;

            try
            {
               reveal.RevealSetting(setting, label);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Showing the setting is a convenience: the page is open either way.
            }
         }));
      }

      /// <summary>
      /// Rebuilds the breadcrumb bar for the page just opened: an optional
      /// "found by search" marker, the trail down to the page, and links to the
      /// neighbouring subjects.
      /// </summary>
      private void UpdateBreadcrumb(string key)
      {
         if (BreadcrumbHost == null)
            return;

         BreadcrumbHost.Children.Clear();
         RelatedHost.Children.Clear();
         UpdateHistoryButtons_();

         IReadOnlyList<NavNode> trail = NavigationMap.PathTo(key);
         if (trail.Count == 0)
         {
            // An unknown key means the page is reachable but not in the map,
            // which NavigationMapTests fails on. Hide the bar rather than draw an
            // empty one.
            BreadcrumbBar.Visibility = Visibility.Collapsed;
            return;
         }

         BreadcrumbBar.Visibility = Visibility.Visible;

         if (arrivedFromSearch_)
         {
            BreadcrumbHost.Children.Add(SearchMarker());
            arrivedFromSearch_ = false;
         }

         for (int index = 0; index < trail.Count; index++)
         {
            if (index > 0)
               BreadcrumbHost.Children.Add(BreadcrumbSeparator());

            NavNode node = trail[index];

            if (index == trail.Count - 1)
            {
               var here = new TextBlock
               {
                  Text = L(node.Title),
                  FontSize = Typography.Label,
                  FontWeight = FontWeights.SemiBold,
                  VerticalAlignment = VerticalAlignment.Center
               };
               here.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
               System.Windows.Automation.AutomationProperties.SetAutomationId(here, "breadcrumb-page");
               BreadcrumbHost.Children.Add(here);
            }
            else
            {
               BreadcrumbHost.Children.Add(GroupSegment(node));
            }
         }

         // As many as fit beside the trail, in the map's order, and the rest in a
         // drop-down (MainWindow.Related.cs, review finding F271): the first three
         // alone were drawn, whatever the room.
         // Only the pages the session's role may open (review finding F079).
         BuildRelated_(NavigationMap.RelatedOf(key, SessionMayUse_()));
      }

      /// <summary>
      /// A group in the trail. A button rather than a label because a group has
      /// no page of its own: activating it reveals the group in the sidebar, so
      /// the user can see what else is in the same place - or, while the sidebar
      /// is the rail and the tree is hidden, opens the group's pages under the
      /// button (MainWindow.NavFlyout.cs). Keyboard reachable like any other
      /// button, which a plain text breadcrumb would not be.
      /// </summary>
      private FrameworkElement GroupSegment(NavNode group)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = L(group.Title),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            FontSize = Typography.Label,
            Padding = new Thickness(4, 2, 4, 2),
            Cursor = System.Windows.Input.Cursors.Hand,
            ToolTip = string.IsNullOrEmpty(group.Purpose)
               ? F("Show {0} in the navigation", L(group.Title))
               : L(group.Purpose)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "breadcrumb-group-" + NavigationMap.Slug(group.Title));
         System.Windows.Automation.AutomationProperties.SetName(button, F("Show the group {0} in the navigation", L(group.Title)));
         button.Click += (s, e) =>
         {
            if (railShowing_)
               ShowGroupFlyout_(group, button, System.Windows.Controls.Primitives.PlacementMode.Bottom);
            else
               RevealGroup(group.Title);
         };
         return button;
      }

      private FrameworkElement RelatedLink(NavNode page)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = L(page.Title),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            FontSize = Typography.Caption,
            Padding = new Thickness(6, 2, 6, 2),
            Cursor = System.Windows.Input.Cursors.Hand,
            ToolTip = L(page.Purpose)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "breadcrumb-related-" + page.Key);
         System.Windows.Automation.AutomationProperties.SetName(button, F("Go to the related page {0}", L(page.Title)));
         button.Click += (s, e) => NavigateTo(page.Key);
         return button;
      }

      private static FrameworkElement BreadcrumbSeparator()
      {
         var separator = new TextBlock
         {
            Text = ">",
            FontSize = Typography.Caption,
            Margin = new Thickness(3, 0, 3, 0),
            VerticalAlignment = VerticalAlignment.Center
         };
         separator.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
         return separator;
      }

      private static FrameworkElement SearchMarker()
      {
         var marker = new Border
         {
            CornerRadius = new CornerRadius(4),
            Padding = new Thickness(6, 1, 6, 1),
            Margin = new Thickness(0, 0, 8, 0),
            VerticalAlignment = VerticalAlignment.Center
         };
         marker.SetResourceReference(Border.BackgroundProperty, "SubtleFillColorSecondaryBrush");

         var text = new TextBlock { Text = L("Found by search"), FontSize = Typography.Caption };
         text.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
         marker.Child = text;

         System.Windows.Automation.AutomationProperties.SetAutomationId(marker, "breadcrumb-from-search");
         System.Windows.Automation.AutomationProperties.SetName(marker, L("You arrived here from the search palette"));
         return marker;
      }

      /// <summary>
      ///    Expands a group in the sidebar and scrolls it into view, without disturbing
      ///    which page is selected.
      ///
      ///    Deliberately does NOT call group.Focus(). A TreeView's selection follows
      ///    keyboard focus, so focusing the group heading selected it and deselected the
      ///    page the user is actually looking at: the content stayed put - because
      ///    NavTree_SelectedItemChanged early-returns for a heading, whose Tag is its
      ///    NavNode rather than a page-key string -
      ///    while the sidebar highlight and the brand pill jumped to the heading. The
      ///    sidebar then disagreed with the content, which is precisely the "where am I"
      ///    problem this navigation work exists to remove.
      ///
      ///    Expanding and bringing into view is the whole job here. Whoever wants the
      ///    keyboard in the sidebar can Tab to it, and arrowing from the selected page
      ///    still works because the selection was never moved.
      /// </summary>
      private void RevealGroup(string title)
      {
         // By the NavNode in the Tag, never by the Header: a group's header is
         // an icon-plus-text StackPanel whenever its icon name resolves, so
         // "Header as string" matched only the groups whose icon was broken.
         foreach (TreeViewItem group in AllGroups(NavTree.Items)
            .Where(g => g.Tag is NavNode node && string.Equals(node.Title, title, StringComparison.Ordinal)))
         {
            for (var parent = group.Parent as TreeViewItem; parent != null; parent = parent.Parent as TreeViewItem)
               parent.IsExpanded = true;

            group.IsExpanded = true;
            group.BringIntoView();
            return;
         }
      }

      private IEnumerable<TreeViewItem> AllGroups(ItemCollection items)
      {
         foreach (TreeViewItem item in items)
         {
            if (item.Tag is not string)
               yield return item;
            foreach (TreeViewItem child in AllGroups(item.Items))
               yield return child;
         }
      }

      /// <summary>
      /// Reads the palette history. Anything at all can be in a registry value,
      /// so a damaged one costs the history and nothing else - an error dialog on
      /// start-up because the list of recently visited pages was corrupt would be
      /// absurd.
      /// </summary>
      private void LoadPaletteUsage()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            paletteUsage_ = PaletteUsage.Deserialize(key?.GetValue(PaletteUsageValue) as string);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            paletteUsage_ = new PaletteUsage();
         }
      }

      /// <summary>
      /// Written after every visit rather than once on close. The window bounds
      /// can afford to be saved on close because losing them costs one resize;
      /// losing a session's worth of history makes the feature look like it does
      /// not work, and the process does not always get to close cleanly. One
      /// short SetValue at user-navigation rate is not worth optimising.
      /// </summary>
      private void SavePaletteUsage()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath);
            key?.SetValue(PaletteUsageValue, paletteUsage_.Serialize());
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      private void OnConnected()
      {
         if (!VerifyTwoFactor())
         {
            // Two-factor verification failed or was cancelled: drop the session
            // and return to the connect screen instead of revealing the UI - the
            // way Sign out goes, the session ended with it, where it stayed the
            // current one, signed in on its password.
            ServerSession refused = ServerSession.Current;
            ShowSignIn_(refused?.Host, refused?.UserName, focusServer: false);
            return;
         }

         connected_ = true;

         // The tree was built at start-up for nobody in particular. Built again
         // now for the credential that has just signed in, so that a page its
         // role may not use is left out rather than shown and refused - and
         // built again on every connection, because the next one may be
         // somebody else.
         BuildNavTree();
         BuildNavRail_();

         NavTree.IsEnabled = true;
         NavRail.IsEnabled = true;
         SearchButton.IsEnabled = true;
         HelpButton.IsEnabled = true;
         HelpPanel.WikiBaseUrl = ReadHelpWikiBaseUrl_();

         // "admin @ mail.example.test" is a fact, not a statement: nothing in it
         // says that this is a live connection. The pill says so in colour, shape
         // and word, and carries "Connected to host as user" as its accessible
         // name; the administrator's name on the menu button is the fact.
         ShowLinkState_(LinkState.Connected);

         ServerSession session = ServerSession.Current;
         try
         {
            VersionText.Text = session == null ? "" : "hMailServer " + (string)session.Application.Version;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            VersionText.Text = "";
         }

         // Drop any selection left over from an earlier session on this window.
         // Selecting the item that is already selected raises no
         // SelectedItemChanged, so without this a reconnect after a cancelled
         // two-factor prompt left the connect screen on display with the
         // navigation enabled beside it and nothing to click that would replace
         // it. Deselecting raises the event with a null item, which the handler
         // ignores.
         foreach (TreeViewItem selected in AllLeaves(NavTree.Items).Where(l => l.IsSelected).ToList())
            selected.IsSelected = false;

         // By key rather than by position in the tree: the first release that
         // reordered the sidebar would otherwise have opened whatever page
         // happened to land at index 1.
         //
         // Flagged as automatic so it is not counted in "Most used" - see the guard in
         // NavigateTo. try/finally because a page's OnEnter can throw, and leaving the
         // flag set would then stop counting every genuine visit for the rest of the
         // session.
         automaticNavigation_ = true;

         // The history is this sign-in's: the pages the last credential opened are
         // not this one's to go back to.
         history_.Clear();

         try
         {
            // The dashboard is the server administrator's; a narrower role lands
            // on Welcome, which every credential may open, rather than on a
            // connect screen left standing beside an enabled sidebar.
            NavNode dashboard = NavigationMap.Find("dashboard");
            bool mayUseDashboard = session == null || dashboard == null || session.May(dashboard.Needs);
            NavigateTo(mayUseDashboard ? "dashboard" : "welcome");
         }
         finally
         {
            automaticNavigation_ = false;
         }

         // The pane comes back as it was left: open, on the page just opened.
         if (helpWanted_ && !helpOpen_)
            OpenHelp_();
      }

      /// <summary>
      /// Puts one link state on the pill and the menu button: the level and the
      /// word from ConnectionStatus (colour, shape and word - never colour
      /// alone), the host and the account as the accessible name.
      /// </summary>
      private void ShowLinkState_(LinkState state)
      {
         linkState_ = state;
         ServerSession session = ServerSession.Current;
         string host = state == LinkState.NotConnected ? null : session?.Host;
         string user = state == LinkState.NotConnected ? null : session?.UserName;

         // A live link to a server whose engine is paused says Paused, at Warning
         // (MainWindow.ServerPaused.cs, review finding F019): the pill said
         // Connected in green over a server that took no mail.
         bool paused = serverPaused_ && state == LinkState.Connected;
         string sentence = paused ? ConnectionStatus.DescribePaused(host, user) : ConnectionStatus.Describe(state, host, user);

         ServerStatusPill.Level = paused ? ConnectionStatus.PausedLevel : ConnectionStatus.LevelFor(state);
         ServerStatusPill.Text = paused ? ConnectionStatus.PausedWord : ConnectionStatus.WordFor(state);
         ServerStatusPill.ToolTip = sentence;
         System.Windows.Automation.AutomationProperties.SetName(ServerStatusPill, sentence);

         AdminName.Text = user ?? "";
         AdminName.Visibility = string.IsNullOrEmpty(user) ? Visibility.Collapsed : Visibility.Visible;
      }

      /// <summary>
      /// The session's own report of its link, from inside whichever COM call
      /// found it gone - possibly on another thread - so it is deferred to the
      /// message loop and ignored once the shell is back on the sign-in page.
      /// </summary>
      private void OnLinkStateChanged(ServerSession session, LinkState state)
      {
         Dispatcher.BeginInvoke(new Action(() =>
         {
            if (!connected_ || !ReferenceEquals(session, ServerSession.Current))
               return;

            ShowLinkState_(state);
         }));
      }

      /// <summary>
      /// The session healed itself after the service went away (a restart, most
      /// often one the Control Panel triggered itself). Say so, and put fresh
      /// data on the page the user is looking at. Deferred to the message loop
      /// because the reconnect happens in the middle of somebody else's COM
      /// call, possibly on a background thread.
      /// </summary>
      private void OnSessionReconnected(ServerSession session)
      {
         Dispatcher.BeginInvoke(new Action(() =>
         {
            if (!connected_)
               return;

            Services.Toast.Info(F("Reconnected to {0} after the service restarted.", session.Host), L("Connection restored"));

            // Entering the page again reads it from the server, which would put the
            // edits on a page with unsaved changes out of the way without a word;
            // such a page keeps them, and its Reload reads the server when asked.
            if (CurrentPageHasUnsavedChanges_())
               RefreshPendingBar_(fresh: true);
            else
               EnterPage(ContentHost.Content);
         }));
      }

      private bool VerifyTwoFactor()
      {
         if (!TotpManager.IsConfigured())
            return true;

         for (int attempt = 0; attempt < 3; attempt++)
         {
            // This computer's own code, which the prompt says, so that it is not
            // taken for the server's (review finding F241).
            var prompt = new Views.TotpPromptDialog(this, Views.TotpPromptDialog.Factor.ThisComputer);
            if (prompt.ShowDialog() != true)
               return false;

            if (Totp.VerifyCode(TotpManager.ReadSecret(), prompt.Code))
               return true;

            MessageBox.Show(L("The verification code is incorrect."), L("This computer's Control Panel code"));
         }

         return false;
      }

      private void NavTree_SelectedItemChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
      {
         if (!connected_ || ContentHost == null || restoringSelection_)
            return;

         if (e.NewValue is not TreeViewItem item || item.Tag is not string key)
            return;

         // Every way to another page comes through here - a sidebar row, a Related
         // link, a palette result, a page's own link, a tour's step - so this is
         // where a page with unsaved changes is asked about. Stay leaves the page on
         // screen and puts the sidebar back on it.
         if (!string.Equals(key, currentPage_, StringComparison.OrdinalIgnoreCase) && !MayLeaveCurrentPage_())
         {
            arrivedFromSearch_ = false;
            RestoreSelection_();
            return;
         }

         if (!pageCache_.TryGetValue(key, out UserControl page))
         {
            if (!pageFactories_.TryGetValue(key, out Func<UserControl> factory))
            {
               // Nothing was navigated to, so the "from search" marker must not
               // be left armed for whatever page is opened next.
               arrivedFromSearch_ = false;
               return;
            }

            page = factory();
            pageCache_[key] = page;
         }

         // The history (MainWindow.History.cs): how the page on screen is being
         // left, then this visit, or the Back or Forward step that asked for it.
         // Here, once the page may be left and the page to open exists, so that
         // Stay and a key with no page move nothing.
         HistoryEntry returningTo = RecordVisit_(key, ContentHost.Content as DependencyObject);

         if (ContentHost.Content is IPageLifecycle oldPage)
         {
            try
            {
               oldPage.OnLeave();
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Leaving a page must never block navigating away from it.
            }
         }

         ContentHost.Content = page;
         currentPage_ = key;

         // Before the page is entered, so that a page whose OnEnter throws still
         // leaves the user able to see where they are and get back out.
         UpdateBreadcrumb(key);
         UpdateRail_(key);

         // The help follows the page and its tab, when it is open
         // (MainWindow.HelpTabs.cs).
         if (helpOpen_)
            ShowHelpForNewPage_(key);

         // Only pages the user chose. OnConnected navigates to the dashboard on every
         // launch, so counting that visit made "Most used" report the dashboard at the
         // top for ever - one count per launch, against single digits for the pages the
         // installation actually lives in. A section whose entire purpose is to surface
         // what this operator uses must not be dominated by what the application does to
         // itself before the operator has touched anything.
         if (!automaticNavigation_)
         {
            paletteUsage_.RecordVisit(key);
            SavePaletteUsage();
         }

         EnterPage(page);
         RestoreVisit_(returningTo, page);

         // A page's help button starts the tour that visits it, where one
         // does; and a running tour follows the page, because the control its
         // step points at is in the tree that has just been replaced.
         OfferPageTour_(page, key);
         TourPageChanged_();
      }

      /// <summary>
      /// Whether the page on screen holds edits that have not reached the server.
      /// A page that cannot say is taken to hold none: the question must never be
      /// what stops somebody leaving a page.
      /// </summary>
      private bool CurrentPageHasUnsavedChanges_()
      {
         if (ContentHost?.Content is not IPageLifecycle page)
            return false;

         try
         {
            return page.HasUnsavedChanges;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return false;
         }
      }

      /// <summary>
      /// Asks about the page on screen's unsaved changes, if it has any, and says
      /// whether it may be left: Save saves as the page's own Save does and goes on
      /// only when everything was saved, Discard goes on, Stay does not
      /// (Services/UnsavedChanges). Asked of the sidebar's selection, the window's
      /// close and a change of language, which restarts the Control Panel.
      /// </summary>
      private bool MayLeaveCurrentPage_()
      {
         var page = ContentHost?.Content as IPageLifecycle;
         string title = currentPage_ == null ? "" : L(NavigationMap.TitleOf(currentPage_));

         return UnsavedChanges.MayLeave(
            CurrentPageHasUnsavedChanges_(),
            () => MessageBox.AskUnsaved(UnsavedChanges.LeaveQuestion(title), L("Control Panel")),
            () =>
            {
               try
               {
                  return page == null || page.SaveChanges();
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  Services.Toast.Failure(F("Could not save: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
                  return false;
               }
            });
      }

      // Set while the sidebar is put back on the page that was kept, so that the
      // selection change it makes is not taken for a request to go there.
      private bool restoringSelection_;

      /// <summary>
      /// Puts the sidebar's selection back on the page on screen, after a Stay: the
      /// row clicked, or the one the arrow keys moved to, was selected before the
      /// question was asked. After the click has finished rather than inside it, so
      /// that the tree is not reselected in the middle of its own selection change;
      /// the keyboard goes back to the row when the tree had it.
      /// </summary>
      private void RestoreSelection_()
      {
         string key = currentPage_;
         Dispatcher.BeginInvoke(new Action(() =>
         {
            TreeViewItem leaf = AllLeaves(NavTree.Items).FirstOrDefault(l => string.Equals(l.Tag as string, key, StringComparison.OrdinalIgnoreCase));
            if (leaf == null)
               return;

            restoringSelection_ = true;
            try
            {
               bool keyboard = NavTree.IsKeyboardFocusWithin;
               leaf.IsSelected = true;
               if (keyboard)
                  leaf.Focus();
               leaf.BringIntoView();
            }
            finally
            {
               restoringSelection_ = false;
            }

            UpdateRail_(key);
         }), System.Windows.Threading.DispatcherPriority.Input);
      }

      /// <summary>
      /// Closing the window is a way of leaving the page too, and is asked about in
      /// the same words; Stay keeps the window open. Not while the Control Panel
      /// restarts (App.Restart, for a change of language or after a fault): the
      /// language was asked about before, and a fault's restart cannot be stopped.
      /// The window's own Closing handlers - the bounds, the palette's history, the
      /// events they let go of - run only when it does close.
      /// </summary>
      protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
      {
         if (connected_ && !App.Restarting && !MayLeaveCurrentPage_())
         {
            e.Cancel = true;
            return;
         }

         base.OnClosing(e);
      }

      /// <summary>
      /// A change of language restarts the Control Panel, which leaves the page as
      /// surely as closing the window does: asked about first, and only for a
      /// language other than the one in use, which changes nothing.
      /// </summary>
      private void OfferLanguage_(string tag)
      {
         if (string.Equals(tag, LanguageChoice.Stored, StringComparison.OrdinalIgnoreCase))
            return;

         if (connected_ && !MayLeaveCurrentPage_())
            return;

         LanguageChoice.Offer(tag);
      }

      /// <summary>
      /// Activates a page, reporting a server that has gone away as a message
      /// instead of an unhandled exception. Several pages load their data
      /// straight from the COM API in OnEnter, so before this the first click
      /// after the hMailServer service stopped threw the application-level
      /// error dialog.
      /// </summary>
      private void EnterPage(object page)
      {
         try
         {
            if (page is IPageLifecycle lifecycle)
               lifecycle.OnEnter();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // A fault of the page's own - a control added to a second panel, a
            // cast that failed - is not the server's: calling it "Server
            // unavailable" sent an administrator to restart a service that was
            // fine (the Auto-ban page did exactly that on every second visit).
            bool server = ex is System.Runtime.InteropServices.COMException || ServerSession.IsTransportFailure(ex);
            // With Try again, which enters the page once more while it is still the
            // page on screen (review finding F008).
            Services.Toast.Failure(F("Could not load this page: {0}", ServerSession.DescribeComError(ex)),
               server ? L("Server unavailable") : L("Could not build this page"),
               () => TryEnterPageAgain_(page));
         }
         finally
         {
            // After the page, which reads the marks the bar counts by when it has
            // fields that show them; asked afresh, since another administrator's
            // save is the kind of thing a page visit is for noticing.
            RefreshPendingBar_(fresh: true);

            // And whether the server is paused, which a page visit is for noticing
            // as well (MainWindow.ServerPaused.cs).
            RefreshServerPaused();
         }
      }

      /// <summary>
      /// The failure toast's Try again: the page is entered again only while it is
      /// still the one on screen, so that a toast pressed after the reader moved on
      /// does not run a page that is no longer shown.
      /// </summary>
      private void TryEnterPageAgain_(object page)
      {
         if (page != null && ReferenceEquals(ContentHost.Content, page))
            EnterPage(page);
      }

      // ServerStatus.StateRunning and StateDraining, as the COM enum numbers them.
      private const int ServerStateRunning_ = 3;
      private const int ServerStateDraining_ = 5;

      /// <summary>
      /// The pending bar: what the server lists as saved and not yet running, with
      /// Apply now when a reinitialise can take some of it (docs/SettingsApply.md).
      /// Asked of the server, never remembered here, so a value put back empties
      /// the bar and another administrator's save fills it at the next refresh.
      /// </summary>
      private void RefreshPendingBar_(bool fresh)
      {
         // Null is a server that cannot say - an older one, whose keys all apply at
         // its next start and which offers nothing to apply now - and shows no bar.
         IReadOnlyList<string> pending = !connected_ ? null
            : fresh ? IniFeatureStore.PendingNames() : IniFeatureStore.PendingNamesSinceLastWrite();

         if (pending == null || pending.Count == 0)
         {
            HidePendingBar_();
            return;
         }

         // Counted by the marks, and each only by its own: a key whose mark is not
         // known is offered neither Apply now nor a restart. The marks come from the
         // page's one read of them; a bar raised before any page read them reads
         // them itself, once.
         if (IniFeatureStore.ServerMarksSettings != true)
            IniFeatureStore.LoadMarks();

         int waitingForRestart = pending.Count(name => IniFeatureStore.AppliesFor(name) == SettingApplies.Restart);
         int waitingForReinitialize = pending.Count(name => IniFeatureStore.AppliesFor(name) == SettingApplies.Reinitialize);

         string banner = SettingApplies.Banner(waitingForReinitialize, waitingForRestart);
         if (banner == null)
         {
            HidePendingBar_();
            return;
         }

         PendingNotice.Text = banner;
         ApplyPendingButton.Visibility = waitingForReinitialize > 0 ? Visibility.Visible : Visibility.Collapsed;
         ApplyPendingButton.IsEnabled = true;

         // Which settings wait, by their captions and pages: the bar said how many
         // and never which.
         string detail = SettingApplies.PendingDetail(pending, IniFeatureStore.AppliesFor);
         PendingNotice.ToolTip = detail;
         System.Windows.Automation.AutomationProperties.SetHelpText(PendingNotice, detail ?? "");

         // The settings only a restart takes had nothing to press: the restart the
         // feature settings pages offer after a save, for the service on this
         // machine - the only one a restart from here reaches.
         pendingRestartCount_ = waitingForRestart;
         RestartServiceButton.Visibility = waitingForRestart > 0 && ServiceRestart.Available ? Visibility.Visible : Visibility.Collapsed;
         RestartServiceButton.IsEnabled = true;
         PendingBar.Visibility = Visibility.Visible;
         KeyboardBackToPage_();
      }

      /// <summary>The bar collapsed, nothing waiting - and the keyboard, if it was on it, on the page.</summary>
      private void HidePendingBar_()
      {
         KeyboardOffPendingBar_();
         PendingBar.Visibility = Visibility.Collapsed;
         KeyboardBackToPage_();
      }

      // Set from the moment a button of the bar sent the keyboard to the page until the
      // work it started is done: Apply now and a restart enter the page again, which
      // builds a feature settings page's cards anew under the keyboard.
      private bool keyboardSentFromBar_;

      /// <summary>
      /// The keyboard, while it is on the pending bar, goes to the page - to the control
      /// of the page that last had it, as F6 takes it there - before the bar's buttons
      /// are disabled for the work they start, or the bar is collapsed: a disabled or
      /// collapsed button gives the keyboard to the window, so that Tab started again
      /// from the window's first control and a screen reader said nothing of where the
      /// keyboard was, through the restart and after it. The bar's notice, a live
      /// region, says the restart or the reinitialise as it runs. The bar counts as
      /// having the keyboard while the window, not yet active again after the question
      /// a button asks first, will give it back to one of the bar's buttons.
      /// </summary>
      private void KeyboardOffPendingBar_()
      {
         bool onBar = PendingBar.IsKeyboardFocusWithin
            || (FocusManager.GetFocusedElement(this) is DependencyObject remembered && IsWithin_(remembered, PendingBar));
         if (!onBar)
            return;

         keyboardSentFromBar_ = true;
         KeyboardToPage_();
      }

      /// <summary>
      /// After the work a button of the bar started, once the page has been entered
      /// again and laid out: the keyboard, if the element it went to was built away,
      /// back on the page, where it went to the window.
      /// </summary>
      private void KeyboardBackToPage_()
      {
         if (!keyboardSentFromBar_)
            return;

         keyboardSentFromBar_ = false;
         Dispatcher.BeginInvoke(new Action(() =>
         {
            IInputElement focused = Keyboard.FocusedElement;
            bool lost = focused == null || ReferenceEquals(focused, this) || (focused is UIElement element && !element.IsVisible);
            if (lost)
               KeyboardToPage_();
         }), System.Windows.Threading.DispatcherPriority.Loaded);
      }

      /// <summary>
      /// The control of the page that last had the keyboard, or the page's first. The
      /// page's host is no control of its own: it takes the keyboard when the control
      /// that had it is built away, and a screen reader then says only "Page content".
      /// </summary>
      private void KeyboardToPage_()
      {
         IInputElement remembered = RememberedPageFocus_();
         if (ReferenceEquals(remembered, ContentHost))
            remembered = null;
         (remembered ?? FirstFocusable_(ContentHost))?.Focus();
      }

      // How many settings the bar last counted as waiting for a restart: what its
      // Restart the service asks about.
      private int pendingRestartCount_;

      /// <summary>
      /// Restart the service: asks first, naming how many saved settings wait and
      /// that connected clients are disconnected, with Cancel the answer Enter gives;
      /// then restarts it and attaches the session again (Services/ServiceRestart).
      /// The reattach raises the session's Reconnected, which enters the page again
      /// and refreshes the bar (OnSessionReconnected); a restart that failed says why
      /// and puts the bar back.
      /// </summary>
      private async void RestartPending_Click(object sender, RoutedEventArgs e)
      {
         if (!MessageBox.Confirm(SettingApplies.RestartQuestion(pendingRestartCount_), L("Control Panel"), L("_Restart the service"), cancelByDefault: true))
            return;

         KeyboardOffPendingBar_();
         ApplyPendingButton.IsEnabled = false;
         RestartServiceButton.IsEnabled = false;
         PendingNotice.Level = StatusLevel.Information;
         PendingNotice.Text = L("Restarting the hMailServer service...");

         (ServiceRestartOutcome outcome, string reason) = await ServiceRestart.RunAsync();
         PendingNotice.Level = StatusLevel.Warning;

         if (outcome == ServiceRestartOutcome.NotRestarted)
            Services.Toast.Failure(F("Could not restart the service: {0}", reason), L("Control Panel"));
         else if (outcome == ServiceRestartOutcome.NotReattached)
            Services.Toast.Failure(F("The service was restarted but the Control Panel could not reconnect to it: {0}\n\nIt will keep trying as you use the application.", reason), L("Control Panel"));

         if (outcome != ServiceRestartOutcome.Restarted)
            RefreshPendingBar_(fresh: true);
      }

      /// <summary>
      /// Apply now: reinitialises the server - every service stopped and started
      /// again in place - which takes every key marked reinitialize, then waits for
      /// the server to report Running and asks again what is pending. Asked first,
      /// and asked differently when the server is draining, because a reinitialise
      /// ends maintenance mode. Called on this thread, as every COM call here is:
      /// the session's objects belong to it.
      /// </summary>
      private void ApplyPending_Click(object sender, RoutedEventArgs e)
      {
         ServerSession session = ServerSession.Current;
         if (session == null)
            return;

         bool draining = false;
         try
         {
            draining = (int)session.Application.ServerState == ServerStateDraining_;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Cannot tell: the question says the ordinary thing, and the reinitialise
            // below reports what it finds.
         }

         if (!MessageBox.Confirm(SettingApplies.ApplyNowQuestion(draining), L("Control Panel"), L("_Apply now"),
                cancelByDefault: true, decline: L("_Later")))
            return;

         KeyboardOffPendingBar_();
         ApplyPendingButton.IsEnabled = false;
         RestartServiceButton.IsEnabled = false;
         PendingNotice.Level = StatusLevel.Information;
         PendingNotice.Text = L("Reinitialising the server...");
         Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.Render);

         Mouse.OverrideCursor = Cursors.Wait;
         string failure = null;
         try
         {
            session.Application.Reinitialize();

            // Reinitialize returns once the servers are started again; the state is
            // what says they are serving. A few seconds' grace for a slow start.
            bool running = false;
            for (int attempt = 0; attempt < 60 && !running; attempt++)
            {
               try
               {
                  running = (int)session.Application.ServerState == ServerStateRunning_;
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  running = false;
               }

               if (!running)
                  System.Threading.Thread.Sleep(500);
            }

            if (!running)
               failure = L("The server did not report Running within thirty seconds; the Server status page says what it is doing.");
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failure = ServerSession.DescribeComError(ex);
         }
         finally
         {
            Mouse.OverrideCursor = null;
            PendingNotice.Level = StatusLevel.Warning;
         }

         if (failure == null)
         {
            // A reinitialise takes the keys marked reinitialize and not those marked
            // restart: asked again rather than assumed, so a service account saved
            // beside a port is still said to wait.
            IReadOnlyList<string> stillPending = IniFeatureStore.PendingNames() ?? Array.Empty<string>();
            int stillWaitingForRestart = stillPending.Count(name => IniFeatureStore.AppliesFor(name) == SettingApplies.Restart);
            Services.Toast.Success(SettingApplies.AppliedSentence(stillWaitingForRestart));
         }
         else
         {
            Services.Toast.Failure(F("Could not reinitialise: {0}", failure), L("Control Panel"));
         }

         // Entering the page again refreshes the bar and puts what the server now
         // runs on in front of the administrator - except over a page's unsaved
         // changes, which a reinitialise does not touch and a fresh read would lose.
         if (CurrentPageHasUnsavedChanges_())
            RefreshPendingBar_(fresh: true);
         else
            EnterPage(ContentHost.Content);
      }

      /// <summary>
      /// The help pane, open: the column takes the width it was last left at,
      /// bounded so that neither the page nor the pane can be squeezed to
      /// nothing, and the pane shows the page's document.
      /// </summary>
      private void OpenHelp_()
      {
         helpOpen_ = true;
         helpWanted_ = true;
         HelpColumn.MinWidth = 260;
         HelpColumn.MaxWidth = 760;
         HelpColumn.Width = new GridLength(Math.Max(260, Math.Min(760, helpWidth_)));
         HelpSplitter.Visibility = Visibility.Visible;
         HelpPanel.Visibility = Visibility.Visible;
         ShowHelpForPage_();
         SaveHelpPreference_();
      }

      private void CloseHelp_()
      {
         if (helpOpen_ && HelpColumn.ActualWidth > 0)
            helpWidth_ = HelpColumn.ActualWidth;

         helpOpen_ = false;
         helpWanted_ = false;
         HelpColumn.MinWidth = 0;
         HelpColumn.MaxWidth = double.PositiveInfinity;
         HelpColumn.Width = new GridLength(0);
         HelpSplitter.Visibility = Visibility.Collapsed;
         HelpPanel.Visibility = Visibility.Collapsed;
         SaveHelpPreference_();
         HelpButton.Focus();
      }

      private void ToggleHelp_()
      {
         if (helpOpen_)
            CloseHelp_();
         else
            OpenHelp_();
      }

      private void Help_Click(object sender, RoutedEventArgs e)
      {
         if (connected_)
            ToggleHelp_();
      }

      /// <summary>
      /// The wiki the pane's "Open on the wiki" is made against, from the
      /// server's HelpWikiBaseUrl setting over COM - late-bound, as the settings
      /// pages read, so a server without the property (an older one) is a
      /// server whose link goes to the project's wiki rather than a failed
      /// connection. An empty value is the administrator hiding the link.
      /// </summary>
      private static string ReadHelpWikiBaseUrl_()
      {
         try
         {
            dynamic settings = ServerSession.Current?.Application?.Settings;
            if (settings == null)
               return DefaultHelpWikiBaseUrl;

            return ((string)settings.HelpWikiBaseUrl ?? "").Trim();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return DefaultHelpWikiBaseUrl;
         }
      }

      private void LoadHelpPreference_()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            helpWanted_ = string.Equals(key?.GetValue(HelpPaneOpenValue) as string, "1", StringComparison.Ordinal);
            double width = ToDouble(key?.GetValue(HelpPaneWidthValue));
            if (width >= 260 && width <= 760)
               helpWidth_ = width;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            helpWanted_ = false;
         }
      }

      private void SaveHelpPreference_()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath);
            if (key == null)
               return;

            key.SetValue(HelpPaneOpenValue, helpWanted_ ? "1" : "0");
            double width = helpOpen_ && HelpColumn.ActualWidth > 0 ? HelpColumn.ActualWidth : helpWidth_;
            key.SetValue(HelpPaneWidthValue, (int)width);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      private void RestoreWindowBounds()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            if (key == null)
               return;

            double w = ToDouble(key.GetValue("WindowWidth"));
            double h = ToDouble(key.GetValue("WindowHeight"));
            double left = ToDouble(key.GetValue("WindowLeft"), double.NaN);
            double top = ToDouble(key.GetValue("WindowTop"), double.NaN);

            if (w >= MinWidth && h >= MinHeight)
            {
               Width = w;
               Height = h;
            }

            // Only restore the position if it lands on a currently-visible screen,
            // so the window can't open off-screen after a monitor change.
            if (!double.IsNaN(left) && !double.IsNaN(top) &&
                left + w > SystemParameters.VirtualScreenLeft + 40 &&
                top + h > SystemParameters.VirtualScreenTop + 40 &&
                left < SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth - 40 &&
                top < SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight - 40)
            {
               WindowStartupLocation = WindowStartupLocation.Manual;
               Left = left;
               Top = top;
            }

            if (string.Equals(key.GetValue("WindowMaximized") as string, "1", StringComparison.Ordinal))
               WindowState = WindowState.Maximized;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      private void SaveWindowBounds()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath);
            if (key == null)
               return;

            bool maximized = WindowState == WindowState.Maximized;
            // Persist the normal (restored) bounds so un-maximizing next time works.
            Rect bounds = maximized ? RestoreBounds : new Rect(Left, Top, Width, Height);

            key.SetValue("WindowWidth", (int)bounds.Width);
            key.SetValue("WindowHeight", (int)bounds.Height);
            key.SetValue("WindowLeft", (int)bounds.Left);
            key.SetValue("WindowTop", (int)bounds.Top);
            key.SetValue("WindowMaximized", maximized ? "1" : "0");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      private static double ToDouble(object value, double fallback = 0)
      {
         if (value == null)
            return fallback;
         return double.TryParse(value.ToString(), out double d) ? d : fallback;
      }

      private void OnSystemParametersChanged(object sender, System.ComponentModel.PropertyChangedEventArgs e)
      {
         if (e.PropertyName == nameof(SystemParameters.HighContrast))
            Dispatcher.BeginInvoke(new Action(ApplySavedTheme));
      }

      private void ApplySavedTheme()
      {
         // High Contrast is the operating system's choice and wins over the saved
         // light/dark preference: Wpf.Ui's High Contrast dictionary paints every
         // control - its buttons, text boxes, the DataGrid scrollbars, the title
         // bar - from SystemColors, the same source our own tokens already read.
         // Without it, the chart card repainted in the system window colour sat
         // under two dark-theme grey buttons.
         if (Services.ThemeTokens.IsHighContrast)
         {
            try
            {
               SystemThemeWatcher.UnWatch(this);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }

            ApplicationThemeManager.Apply(ApplicationTheme.HighContrast);
            Services.ThemeTokens.Refresh();
            UpdateThemeToggle_();
            return;
         }

         string saved = null;
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            saved = key?.GetValue("Theme") as string;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }

         if (saved == "Light")
         {
            ApplicationThemeManager.Apply(ApplicationTheme.Light);
         }
         else if (saved == "Dark")
         {
            ApplicationThemeManager.Apply(ApplicationTheme.Dark);
         }
         else
         {
            // No saved preference: follow the OS theme and keep tracking it until
            // the user makes an explicit choice with the theme toggle.
            try
            {
               ApplicationThemeManager.ApplySystemTheme();
               SystemThemeWatcher.Watch(this);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
         }

         // Make sure the semantic tokens match whatever theme we just applied.
         Services.ThemeTokens.Refresh();

         UpdateThemeToggle_();
      }

      /// <summary>
      /// Points the theme toggle at the state a click will produce: a sun while
      /// the application is dark, a moon while it is light. It used to show
      /// DarkTheme24 in both states - an icon disagreeing with its verb, the
      /// same defect the Pause button had. The tool tip and accessible name
      /// follow, so "what will this do" reads the same by eye and by ear.
      /// </summary>
      private void UpdateThemeToggle_()
      {
         bool dark = ApplicationThemeManager.GetAppTheme() == ApplicationTheme.Dark;

         ThemeButton.Icon = new Wpf.Ui.Controls.SymbolIcon(
            dark ? Wpf.Ui.Controls.SymbolRegular.WeatherSunny24
                 : Wpf.Ui.Controls.SymbolRegular.WeatherMoon24);

         string action = dark ? L("Switch to the light theme") : L("Switch to the dark theme");
         ThemeButton.ToolTip = action;
         System.Windows.Automation.AutomationProperties.SetName(ThemeButton, action);
      }

      private void OnThemeChanged(ApplicationTheme theme, System.Windows.Media.Color accent)
         => UpdateThemeToggle_();

      /// <summary>
      /// The administrator's menu: who is signed in and where and the server's
      /// version (as facts, not commands), then the language - one entry per
      /// catalogue plus "whatever Windows shows", the current one ticked;
      /// choosing another restarts the Control Panel (LanguageChoice says why),
      /// and the same choice is offered on the sign-in screen, which is where
      /// somebody who cannot read the current language needs it most - then the
      /// theme, then the About page.
      /// </summary>
      private void Admin_Click(object sender, RoutedEventArgs e)
      {
         var menu = new ContextMenu
         {
            PlacementTarget = AdminButton,
            Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom
         };

         ServerSession session = ServerSession.Current;
         if (connected_ && session != null)
         {
            // With the link gone and not coming back by itself, the way back first
            // (review finding F012): the sign-in page, on this server and account.
            if (linkState_ == LinkState.Lost)
            {
               var again = new MenuItem { Header = L("Sign in again") };
               System.Windows.Automation.AutomationProperties.SetAutomationId(again, "admin-sign-in-again");
               again.Click += (s, args) => SignOut_(anotherServer: false);
               menu.Items.Add(again);
               menu.Items.Add(new Separator());
            }

            menu.Items.Add(new MenuItem
            {
               Header = F("Connected to {0} as {1}", session.Host, session.UserName),
               IsEnabled = false
            });

            if (VersionText.Text.Length > 0)
               menu.Items.Add(new MenuItem { Header = VersionText.Text, IsEnabled = false });

            menu.Items.Add(new Separator());
         }

         var language = new MenuItem { Header = L("Change the language") };
         foreach (MenuItem item in Loc.Languages.Select(candidate => new MenuItem
         {
            Header = L(candidate.NativeName),
            IsCheckable = true,
            IsChecked = string.Equals(candidate.Tag, LanguageChoice.Stored, StringComparison.OrdinalIgnoreCase),
            Tag = candidate.Tag
         }))
         {
            item.Click += (s, args) => OfferLanguage_((string)((MenuItem)s).Tag);
            language.Items.Add(item);
         }
         menu.Items.Add(language);

         if (!Services.ThemeTokens.IsHighContrast)
         {
            bool dark = ApplicationThemeManager.GetAppTheme() == ApplicationTheme.Dark;
            var theme = new MenuItem { Header = dark ? L("Switch to the light theme") : L("Switch to the dark theme") };
            theme.Click += Theme_Click;
            menu.Items.Add(theme);
         }

         if (connected_)
         {
            menu.Items.Add(new Separator());
            var about = new MenuItem { Header = L(NavigationMap.TitleOf("about")) };
            about.Click += (s, args) => NavigateTo("about");
            menu.Items.Add(about);

            // Ending the session, or starting one on another server, without
            // closing the program (review finding F012): the sign-in page came
            // back only after a refused second factor.
            menu.Items.Add(new Separator());
            var other = new MenuItem { Header = L("Connect to another server…") };
            System.Windows.Automation.AutomationProperties.SetAutomationId(other, "admin-connect-other");
            other.Click += (s, args) => SignOut_(anotherServer: true);
            menu.Items.Add(other);
            var signOut = new MenuItem { Header = L("Sign out") };
            System.Windows.Automation.AutomationProperties.SetAutomationId(signOut, "admin-sign-out");
            signOut.Click += (s, args) => SignOut_(anotherServer: false);
            menu.Items.Add(signOut);
         }

         menu.IsOpen = true;
      }

      // The link's state as the pill last showed it: the administrator's menu offers
      // Sign in again while it is Lost.
      private LinkState linkState_ = LinkState.NotConnected;

      /// <summary>
      /// Sign out, Connect to another server and Sign in again: the page's unsaved
      /// changes asked about first, as every way off a page asks, then the session
      /// ended and the sign-in page shown - on the same server and account, the
      /// keyboard in the password, or with the server's box chosen to type another.
      /// </summary>
      private void SignOut_(bool anotherServer)
      {
         if (!MayLeaveCurrentPage_())
            return;

         ServerSession session = ServerSession.Current;
         ShowSignIn_(session?.Host, session?.UserName, focusServer: anotherServer);
      }

      /// <summary>
      /// The window as it is before sign-in, from a session: any tour ended, the
      /// page on screen left and every page built for the session let go - another
      /// sign-in may be another account or another server, and a page is built for
      /// what its credential may do - the history cleared, the navigation, the search
      /// and the help put away, the breadcrumb and the pending bar hidden, the session
      /// ended and no longer the current one, and the sign-in page shown with the
      /// server and the account it was on.
      /// </summary>
      private void ShowSignIn_(string host, string userName, bool focusServer)
      {
         connected_ = false;
         EndTour(record: false);

         if (ContentHost.Content is IPageLifecycle page)
         {
            try
            {
               page.OnLeave();
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Leaving a page must never block signing out.
            }
         }

         pageCache_.Clear();
         history_.Clear();
         NavTree.IsEnabled = false;
         NavRail.IsEnabled = false;
         SearchButton.IsEnabled = false;
         HelpButton.IsEnabled = false;
         if (helpOpen_)
         {
            // Put away, and open again at the next sign-in as it was left.
            bool wanted = helpWanted_;
            CloseHelp_();
            helpWanted_ = wanted;
            SaveHelpPreference_();
         }
         BreadcrumbBar.Visibility = Visibility.Collapsed;
         HidePendingBar_();
         ShowServerPaused_(false);
         currentPage_ = null;
         VersionText.Text = "";

         ServerSession.Current?.SignOut();
         ServerSession.SetCurrent(null);
         ShowLinkState_(LinkState.NotConnected);
         ContentHost.Content = new ConnectView(OnConnected, host, userName, focusServer);
      }

      private void Theme_Click(object sender, RoutedEventArgs e)
      {
         // Under High Contrast the operating system owns the palette; a light or
         // dark dictionary on top of it would undo exactly what the user asked for.
         if (Services.ThemeTokens.IsHighContrast)
            return;

         // An explicit toggle takes over from the OS, so stop following it.
         try
         {
            SystemThemeWatcher.UnWatch(this);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }

         bool toLight = ApplicationThemeManager.GetAppTheme() == ApplicationTheme.Dark;
         ApplicationThemeManager.Apply(toLight ? ApplicationTheme.Light : ApplicationTheme.Dark);

         // Recompute semantic tokens for the newly applied theme.
         Services.ThemeTokens.Refresh();

         // OnThemeChanged has already run via the Changed event; calling again
         // costs nothing and keeps the button honest even if a future WPF-UI
         // stops raising Changed from Apply.
         UpdateThemeToggle_();

         try
         {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath);
            key?.SetValue("Theme", toLight ? "Light" : "Dark");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }
   }

   /// <summary>Optional page activation hooks (start/stop timers etc.).</summary>
   public interface IPageLifecycle
   {
      void OnEnter();
      void OnLeave();

      /// <summary>
      /// Whether the page holds edits that have not reached the server. The shell
      /// asks before it leaves such a page - by the sidebar, a link, the palette, a
      /// tour, a change of language or closing the window - because the page is kept
      /// and read again from the server as it is next opened, and the edits would be
      /// gone. A page with nothing to edit says no, which is the default.
      /// </summary>
      bool HasUnsavedChanges => false;

      /// <summary>
      /// Saves the unsaved changes as the page's own Save does, reporting on the
      /// page as that does; true only when every change was saved, so that a save
      /// that failed anywhere keeps the administrator on the page with its sentence.
      /// </summary>
      bool SaveChanges() => true;
   }
}
