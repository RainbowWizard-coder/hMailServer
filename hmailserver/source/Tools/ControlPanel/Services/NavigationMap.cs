// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// One node of the navigation: either a group heading or a page. Pages carry
   /// the same navigation key the page factories in MainWindow are registered
   /// under, so this map describes routes to pages and never the pages
   /// themselves.
   /// </summary>
   public sealed class NavNode
   {
      internal NavNode(string key, string title, string purpose, IReadOnlyList<string> aliases,
         IReadOnlyList<string> seeAlso, IReadOnlyList<NavNode> children, string icon = null,
         string needs = null)
      {
         Key = key;
         Title = title;
         Purpose = purpose;
         Aliases = aliases;
         SeeAlso = seeAlso;
         Children = children;
         Icon = icon;
         Needs = needs;
      }

      /// <summary>
      /// The permission a role must hold for this page to be shown, or empty
      /// for a page every signed-in credential may open. It decides what is
      /// DRAWN and never what is ALLOWED: the server refuses the COM call
      /// whatever the shell shows, and a credential the server did not describe
      /// sees every page, as it did before roles existed.
      /// </summary>
      public string Needs { get; }

      /// <summary>The navigation key of the page, or null for a group heading.</summary>
      public string Key { get; }

      /// <summary>What the node is called on screen.</summary>
      public string Title { get; }

      /// <summary>
      /// One line saying what this is for, in the administrator's terms. Shown
      /// as the tool tip and the accessible help text in the tree, and searched
      /// by the palette, so that a page can be found by what it does and not
      /// only by what it is called.
      /// </summary>
      public string Purpose { get; }

      /// <summary>
      /// Other names for this node: legacy and renamed titles, the page's own
      /// on-screen heading where it differs from the navigation label, protocol
      /// acronyms, and the nouns an administrator would use instead of ours.
      /// Searched but never displayed.
      /// </summary>
      public IReadOnlyList<string> Aliases { get; }

      /// <summary>
      /// Page keys of the neighbouring subjects an administrator on this page
      /// very often needs next. Rendered as signposts beside the breadcrumb, as
      /// many as fit in the order listed and the rest in a drop-down, so a page's
      /// deliberate partner comes first; RelatedLinksTests holds every page to
      /// being listed by another and each documented pair to listing each other.
      /// This is how a subject that genuinely spans several pages - transport
      /// security is five subsystems to us and one question to the user - stays
      /// navigable without duplicating pages into two branches of the tree.
      /// </summary>
      public IReadOnlyList<string> SeeAlso { get; }

      /// <summary>Child nodes; empty for a page.</summary>
      public IReadOnlyList<NavNode> Children { get; }

      /// <summary>The group this node sits in, or null at the top level.</summary>
      public NavNode Parent { get; internal set; }

      /// <summary>True when this node is a navigable page rather than a heading.</summary>
      public bool IsPage => Key != null;

      /// <summary>
      /// The Segoe Fluent Icons glyph for this node, as the NAME of a
      /// Wpf.Ui.Controls.SymbolRegular member ("Globe24"). A string rather than
      /// the enum so this file stays WPF-free and testable; MainWindow resolves
      /// it, and an unknown name renders no icon rather than failing - loudly
      /// in DEBUG builds, because Enum.TryParse failing silently is how a
      /// mistyped name once shipped one group bare among seven with glyphs.
      /// Set on the top-level groups - a glyph on all fifty leaf pages would be
      /// noise, and Windows 11's own settings surfaces put them on the
      /// top level.
      /// </summary>
      public string Icon { get; }
   }

   /// <summary>
   /// The Control Panel's information architecture, as data.
   ///
   /// It replaces the hand-built tree in MainWindow, whose comment said it
   /// "mirrors the classic Administrator tree layout" - a 2000s MFC layout
   /// organised around where the product keeps things. Of 42 pages, 30 sat
   /// under a single "Settings" group, three levels deep; "Settings" is not a
   /// category, it is where things go when nobody has decided what they are.
   /// Everything here is grouped by what an administrator is trying to achieve
   /// instead, and the depth is capped at two so that a page is a heading and a
   /// click rather than a heading, a sub-heading and a click.
   ///
   /// Two rules constrain the reorganisation, because the pages themselves are
   /// not being rewritten:
   ///
   ///   - Every navigation key registered in MainWindow appears here exactly
   ///     once. A page that stops being reachable is a regression, not a
   ///     simplification, and NavigationMapTests fails if the two sets differ.
   ///   - No page title changes. Titles appear in the manual, in forum answers
   ///     and in support mail; renaming them to read better here would silently
   ///     invalidate all of it. Where a better name exists it is added as an
   ///     alias, which the palette searches, instead of replacing the title.
   ///
   ///     The title here is also the page's own heading: PageHeader.PageKey
   ///     takes it from this map, so the sidebar, the breadcrumb, the palette
   ///     and the page say one name. Where a page and its navigation entry
   ///     said two, the clearer of the pair is the title and the other an alias
   ///     (the owner's decision of 9 October 2026; review finding F215) - so
   ///     "Certificates (ACME)" is now "Automatic certificates (ACME)", as its
   ///     page always said.
   ///
   ///     Splitting a page is the one case that cannot obey that rule, because a
   ///     title naming two subjects cannot survive on a page that now holds one
   ///     of them. "Auto-ban &amp; SSL/TLS" is the only one so far, and the cost
   ///     is paid rather than avoided: the old title is an alias on BOTH halves,
   ///     the documented path to it still resolves (to the auto-ban half - see
   ///     <see cref="LegacyPaths"/>), and each half signposts the other. Anyone
   ///     splitting another page owes the reader the same three things.
   ///
   /// Living in a WPF-free file rather than in MainWindow is deliberate: the
   /// structure is the thing worth testing, and the tests reference
   /// ControlPanel.Core, which has no WPF.
   /// </summary>
   public static class NavigationMap
   {
      private static readonly List<NavNode> roots_ = BuildRoots();
      private static readonly Dictionary<string, NavNode> byKey_ = new Dictionary<string, NavNode>(StringComparer.OrdinalIgnoreCase);
      private static readonly List<NavNode> pages_ = new List<NavNode>();
      private static readonly List<NavNode> groups_ = new List<NavNode>();

      static NavigationMap()
      {
         foreach (NavNode root in roots_)
            Index(root, null);
      }

      /// <summary>The top-level nodes, in display order.</summary>
      public static IReadOnlyList<NavNode> Roots => roots_;

      /// <summary>Every page, in display order.</summary>
      public static IReadOnlyList<NavNode> Pages => pages_;

      /// <summary>Every group heading, in display order.</summary>
      public static IReadOnlyList<NavNode> Groups => groups_;

      /// <summary>
      /// The tree as one credential should see it: the same nodes, with the
      /// pages whose permission it does not hold left out, and a group left out
      /// once every page under it has gone. Pages and Groups above stay the
      /// WHOLE map on purpose - the breadcrumb, the legacy paths and the
      /// page-registration test all resolve against it, and a page that is not
      /// drawn is still a page that exists.
      /// </summary>
      public static IReadOnlyList<NavNode> VisibleRoots(Func<string, bool> mayUse)
      {
         if (mayUse == null)
            return roots_;

         return roots_.Select(root => Filter_(root, mayUse)).Where(kept => kept != null).ToList();
      }

      /// <summary>Every page this credential may open, in display order.</summary>
      public static IReadOnlyList<NavNode> VisiblePages(Func<string, bool> mayUse)
         => mayUse == null ? pages_ : pages_.Where(p => mayUse(p.Needs)).ToList();

      private static NavNode Filter_(NavNode node, Func<string, bool> mayUse)
      {
         if (node.IsPage)
            return mayUse(node.Needs) ? node : null;

         List<NavNode> children = node.Children.Select(child => Filter_(child, mayUse)).Where(kept => kept != null).ToList();

         if (children.Count == 0)
            return null;

         // A copy of the heading, so that the map itself is never narrowed: a
         // static list filtered in place would show the next credential the
         // last one's tree. The pages under it are the map's own nodes and keep
         // the map's own Parent - the breadcrumb walks that, and pointing it at
         // this short-lived copy would narrow the map after all.
         return new NavNode(null, node.Title, node.Purpose, node.Aliases, node.SeeAlso, children, node.Icon);
      }

      /// <summary>
      /// The pages <paramref name="key"/>'s Related links lead to, in its list's
      /// order: those that exist and that <paramref name="mayUse"/> lets the
      /// signed-in credential open - every one for a null <paramref name="mayUse"/>,
      /// a credential the server did not describe. A domain administrator's
      /// Domains page offered Public folders, Groups and Rules, pages the role
      /// cannot open and the sidebar hides, and a press on one did nothing
      /// (review finding F079). The same rule as <see cref="VisibleRoots"/>: what
      /// is DRAWN, never what is ALLOWED.
      /// </summary>
      public static IReadOnlyList<NavNode> RelatedOf(string key, Func<string, bool> mayUse)
      {
         NavNode page = Find(key);
         if (page == null)
            return Array.Empty<NavNode>();

         return page.SeeAlso
            .Select(Find)
            .Where(target => target != null && (mayUse == null || mayUse(target.Needs)))
            .ToList();
      }

      /// <summary>The page with this navigation key, or null.</summary>
      public static NavNode Find(string key)
         => key != null && byKey_.TryGetValue(key, out NavNode node) ? node : null;

      /// <summary>
      /// The on-screen title of a page, or the key itself when it is unknown.
      /// Returning the key rather than an empty string keeps a stale reference
      /// visible in the interface instead of rendering as a blank row.
      /// </summary>
      public static string TitleOf(string key)
      {
         NavNode node = Find(key);
         return node != null ? node.Title : key ?? "";
      }

      /// <summary>
      /// The trail from the top level down to and including the page, or an
      /// empty list for an unknown key.
      /// </summary>
      public static IReadOnlyList<NavNode> PathTo(string key)
      {
         NavNode node = Find(key);
         if (node == null)
            return Array.Empty<NavNode>();

         var trail = new List<NavNode>();
         for (NavNode current = node; current != null; current = current.Parent)
            trail.Add(current);

         trail.Reverse();
         return trail;
      }

      /// <summary>
      /// The breadcrumb as one string ("Spam &amp; virus filtering &gt; Anti-spam
      /// settings"), for a search result that has to say where the thing it
      /// found actually lives. Uses a plain "&gt;" rather than a chevron
      /// character so it is safe in a tool tip, an accessible name and a test
      /// assertion alike.
      /// </summary>
      public static string LocationOf(string key)
         => string.Join(" > ", PathTo(key).Select(n => L(n.Title)));

      /// <summary>The title of the group a page sits in, or "" at the top level.</summary>
      public static string GroupOf(string key)
      {
         NavNode node = Find(key);
         return node?.Parent != null ? L(node.Parent.Title) : "";
      }

      /// <summary>
      /// Where the page used to be. Keyed on the path an administrator would
      /// find in the manual, in a forum answer or in their own notes - both the
      /// classic Administrator paths and the 6.2.18 Control Panel paths this
      /// reorganisation replaces.
      ///
      /// The palette searches these, so a documented route still lands on the
      /// right page after the tree has moved. Without it, reorganising the
      /// navigation would quietly invalidate every set of instructions ever
      /// written about this product, which is a worse outcome than a badly
      /// organised tree.
      /// </summary>
      public static readonly IReadOnlyDictionary<string, string> LegacyPaths =
         new ReadOnlyDictionary<string, string>(new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
         {
            ["Status > Server status"] = "status",
            ["Status > Delivery queue"] = "queue",
            ["Status > Live logs"] = "logs",

            ["Settings > Protocols"] = "protocols",
            ["Settings > Protocols > SMTP"] = "protocols",
            ["Settings > Protocols > POP3"] = "protocols",
            ["Settings > Protocols > IMAP"] = "protocols",
            ["Settings > Delivery of e-mail"] = "delivery",
            ["Settings > Public folders"] = "publicfolders",
            ["Settings > Logging"] = "logging",

            ["Settings > Anti-spam"] = "antispam",
            ["Settings > Anti-spam > SURBL servers"] = "surbl",
            ["Settings > Anti-spam > DNS blacklists"] = "dnsbl",
            ["Settings > Anti-spam > White list"] = "spamwhitelist",
            ["Settings > Anti-spam > Greylisting white list"] = "greylistwhitelist",
            ["Settings > Anti-spam > Greylisting bypass list"] = "greylistbypass",
            ["Settings > Anti-spam > Domain white and black list"] = "domainpolicylist",
            ["Settings > Anti-virus"] = "antivirus",
            ["Settings > Anti-virus > Blocked attachments"] = "blockedattachments",

            ["Settings > Advanced"] = "advanced",
            ["Settings > Advanced > Routes"] = "routes",
            ["Settings > Advanced > IP ranges"] = "ipranges",
            ["Settings > Advanced > SSL certificates"] = "certs",
            ["Settings > Advanced > TCP/IP ports"] = "ports",
            ["Settings > Advanced > Incoming relays"] = "relays",
            ["Settings > Advanced > Groups"] = "groups",
            ["Settings > Advanced > Server messages"] = "servermessages",
            ["Settings > Advanced > Scripts"] = "scripts",
            ["Settings > Advanced hardening"] = "hardening",

            ["Settings > Security > Authentication"] = "authentication",
            ["Settings > Security > Administrative access"] = "adminaccess",

            // "Auto-ban & SSL/TLS" was one page holding two unrelated subjects,
            // and it is now two: "Auto-ban" under Access & abuse protection and
            // "SSL/TLS" under TLS & certificates.
            //
            // A path can only resolve to one page, so the combined path resolves
            // to the brute-force half: an administrator typing it is following a
            // note about repeated logon failures far more often than one about
            // cipher suites, and the auto-ban settings are the ones with no other
            // home (versions and ciphers are findable from four other TLS pages).
            // The other half is not lost - the old title is an alias on BOTH
            // pages, so typing it in the palette offers the two of them, and each
            // page signposts the other.
            ["Settings > Security > Auto-ban & SSL/TLS"] = "autoban",
            ["Settings > Security > Auto-ban"] = "autoban",
            ["Settings > Advanced > Auto-ban"] = "autoban",
            ["Settings > Security > SSL/TLS"] = "tls",
            ["Settings > Advanced > SSL/TLS"] = "tls",
            ["Settings > Security > IP ranges"] = "ipranges",
            ["Settings > Security > SSL certificates"] = "certs",
            ["Settings > Security > Transport security"] = "security",
            ["Settings > Security > Certificates (ACME)"] = "acme",

            ["Settings > Network > TCP/IP ports"] = "ports",
            ["Settings > Network > Incoming relays"] = "relays",
            ["Settings > Network > DNS resolver"] = "dns",
            ["Settings > Network > Web services & autoconfiguration"] = "webservices",
            ["Settings > Network > API & monitoring"] = "api",

            ["Settings > Maintenance > Performance"] = "performance",
            ["Settings > Maintenance > Advanced & scripting"] = "advanced",
            ["Settings > Maintenance > Advanced INI settings"] = "hardening",
            ["Settings > Maintenance > Event scripts"] = "scripts",
            ["Settings > Maintenance > Server messages"] = "servermessages",
            ["Settings > Maintenance > Groups"] = "groups",

            ["Utilities > MX-query"] = "mxquery",
            ["Utilities > Server sendout"] = "sendout",
            ["Utilities > Diagnostics"] = "diagnostics",
            ["Utilities > Backup"] = "backup",
            ["Utilities > Backup & restore"] = "backup"
         });

      /// <summary>
      /// The tab of a page of tabs that holds what each of its aliases names - by the
      /// page's key, then by the alias, the tab's English header as the page writes
      /// it before L() - so that Ctrl+K opens a page it found by an alias on that tab
      /// (PaletteRow.Tab, MainWindow.NavigateToTab). Transport security has been four
      /// tabs since review finding F228, and SRS, DKIM oversigning, DMARC reports,
      /// TLS-RPT and ARC opened it on Encryption, a tab with none of them. A page not
      /// listed here, and an alias it does not list, open the page as it is.
      /// TransportSecurityTabsTests holds every alias of Transport security to one of
      /// its tabs.
      /// </summary>
      public static readonly IReadOnlyDictionary<string, IReadOnlyDictionary<string, string>> AliasTabs =
         new ReadOnlyDictionary<string, IReadOnlyDictionary<string, string>>(
            new Dictionary<string, IReadOnlyDictionary<string, string>>(StringComparer.OrdinalIgnoreCase)
            {
               ["security"] = TabsOf_(
                  (N("Encryption"), "DANE|TLSA|MTA-STS|Downgrade|Opportunistic TLS|Enforce TLS"),
                  (N("Signing"), "ARC|DKIM oversigning|Oversign|DKIM expiry|DKIM timestamps|Authentication-Results|Received-SPF"),
                  (N("Forwarding"), "SRS|Sender Rewriting Scheme|BATV|Bounce address tag|Forged bounces"),
                  (N("Reports"), "TLS-RPT|TLS reporting|DMARC reports|DMARC aggregate reports|rua"))
            });

      /// <summary>
      /// The tab of page <paramref name="key"/> that holds what <paramref name="alias"/>
      /// names (<see cref="AliasTabs"/>), or null.
      /// </summary>
      public static string TabOfAlias(string key, string alias)
         => key != null && alias != null && AliasTabs.TryGetValue(key, out IReadOnlyDictionary<string, string> tabs)
            && tabs.TryGetValue(alias, out string tab) ? tab : null;

      private static IReadOnlyDictionary<string, string> TabsOf_(params (string Tab, string Aliases)[] tabs)
      {
         var byAlias = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         foreach ((string tab, string aliases) in tabs)
            foreach (string alias in Split(aliases))
               byAlias.Add(alias, tab);
         return new ReadOnlyDictionary<string, string>(byAlias);
      }

      /// <summary>
      /// A lower-case, hyphen-separated form of a title, for automation ids.
      /// Kept here rather than in MainWindow so that the ids the accessibility
      /// tooling and any UI automation depend on are derived from the same
      /// titles the map defines.
      /// </summary>
      public static string Slug(string text)
         => new string((text ?? "").ToLowerInvariant().Select(c => char.IsLetterOrDigit(c) ? c : '-').ToArray());

      private static void Index(NavNode node, NavNode parent)
      {
         node.Parent = parent;

         if (node.IsPage)
         {
            // A duplicated key would give the page two homes, and every consumer
            // here (breadcrumb, palette, tree selection) silently picks the
            // first. Fail loudly at start-up instead: this is a table in one
            // file, so a duplicate is always a typo.
            if (byKey_.ContainsKey(node.Key))
               throw new InvalidOperationException("The navigation map lists the page '" + node.Key + "' more than once.");

            byKey_[node.Key] = node;
            pages_.Add(node);
         }
         else
         {
            groups_.Add(node);
         }

         foreach (NavNode child in node.Children)
            Index(child, node);
      }

      /// <summary>
      /// Aliases are written as one pipe-separated string per page rather than
      /// as a string array. With 42 pages and up to a dozen aliases each, array
      /// syntax buries the actual content in punctuation, and the point of this
      /// table is that a maintainer can read the alias list and judge whether it
      /// covers how people talk.
      /// </summary>
      /// <summary>
      /// What a page needs when its row does not say: the server administrator.
      /// Nearly every page of this program reaches the server through
      /// Application.Settings, Status, Diagnostics or the delivery queue, and
      /// COM refuses each of those to anything less - so a page is left out for
      /// a narrower role unless its row names the permission that lets that
      /// role use it. A new page therefore starts hidden from the narrow roles,
      /// which is the safe way round: shown by mistake it would be a page of
      /// refusals, and hidden by mistake it is one line to fix.
      /// </summary>
      internal const string ServerAdministration = "administer_server";

      /// <summary>A page any signed-in credential may open.</summary>
      internal const string EveryCredential = "";

      private static NavNode Page(string key, string title, string purpose, string aliases = "", string seeAlso = "",
         string needs = ServerAdministration)
         => new NavNode(key, title, purpose, Split(aliases), Split(seeAlso), Array.Empty<NavNode>(), null, needs);

      private static NavNode Group(string title, string purpose, string aliases, params NavNode[] children)
         => new NavNode(null, title, purpose, Split(aliases), Array.Empty<string>(), children);

      private static NavNode Group(string icon, string title, string purpose, string aliases, params NavNode[] children)
         => new NavNode(null, title, purpose, Split(aliases), Array.Empty<string>(), children, icon);

      private static IReadOnlyList<string> Split(string pipeSeparated)
         => string.IsNullOrEmpty(pipeSeparated)
            ? (IReadOnlyList<string>)Array.Empty<string>()
            : pipeSeparated.Split('|').Select(s => s.Trim()).Where(s => s.Length > 0).ToList();

      /// <summary>
      /// The tree itself, in the order a new administrator needs it (27
      /// September 2026; it was ordered "is it working" first, which put the
      /// setup checklist twelfth under Monitoring). Getting started first - the
      /// welcome, the dashboard and the setup group, whose checklist opens the
      /// page for each step; then the domains and accounts; then how mail gets
      /// in and out - delivery, the ports and protocols, the certificates; then
      /// protection - spam and viruses, sign-in and access; then the day-to-day
      /// - monitoring and troubleshooting; and last the maintenance and expert
      /// settings a server rarely needs changed. The keys, and so the automation
      /// ids nav-&lt;key&gt;, are what they were; two group headings were renamed
      /// into plain words and keep their old names as aliases.
      /// </summary>
      private static List<NavNode> BuildRoots()
      {
         return new List<NavNode>
         {
            Page("welcome", N("Welcome"),
               N("Start here: jump straight to a common task, or press Ctrl+K and describe what you want to do."),
               aliases: "Home|Start|Getting started|Quick actions|First steps",
               seeAlso: "dashboard",
               needs: EveryCredential),

            Page("dashboard", N("Dashboard"),
               N("Live counters and charts for processed mail, spam, viruses and sessions."),
               aliases: "Overview|Charts|Graphs|Statistics|Counters|Throughput",
               seeAlso: "status|queue"),

            // First after the dashboard, because it is what a new server needs
            // first: the checklist that says what is done and what is next, the
            // profile that sets thirty settings for the kind of server this is,
            // and the DNS records each domain has to publish. Every item on the
            // checklist opens the page that does it, wherever that page lives.
            Group("ClipboardTaskListLtr24", N("Set up this server"),
               N("What a new server needs, in order: the checklist of what is done and what is next, a profile of settings for the kind of server this is, and the DNS records each domain publishes."),
               "Getting started|First steps|Setup|Set up|Onboarding|New server|New installation|Wizard|Setup wizard|First-hour checklist|Where do I start",

               // The server's first-hour checklist (docs/SetupChecklist.md) and
               // everything else this server needs done outside it - a DNS zone,
               // a CA, an identity provider - numbered in the order a new
               // installation meets them, each item opening the page that does
               // it. Called "External setup" until the reordering made it the
               // way in; that title is an alias, and the key is unchanged, so
               // every link and automation id still arrives here.
               Page("externalsetup", N("Setup checklist"),
                  N("What this server still needs before anybody can use it, in order - a domain, an account, TLS for mail clients, the DNS records, a backup - with what is done, what is next, and a button to the page that does each one."),
                  aliases: "External setup|External prerequisites|Prerequisites|Checklist|First-hour checklist|Outside the server|What still needs doing|What next|Publish a DNS record|TXT record|Inert features|Action needed",
                  seeAlso: "dnsrecords|domains|profiles"),

               // The setting profiles (docs/Profiles.md): which profile is in effect,
               // each one's diff against this server, apply and the way back. In
               // the setup group because a profile is the checklist's first item:
               // it sets thirty settings at once for the kind of server this is.
               Page("profiles", N("Setting profiles"),
                  N("A profile sets the thirty or so settings one kind of server needs - which, to what and why - shows the difference from this server before anything changes, and keeps what it replaces so that it can be put back."),
                  aliases: "Profile|Profiles|Server profile|Small office|Office server|Filtering gateway|Behind a gateway|Outbound relay|Relay server|High-volume sender|Bulk sender|Newsletter server|Recommended settings|Starting settings|Presets|Baseline|Kind of server",
                  seeAlso: "hardening|backup"),

               // Sits beside the setup checklist on purpose: that page says WHICH
               // steps are outstanding, this one produces the exact DNS records
               // those steps need and checks each one.
               Page("dnsrecords", N("DNS records"),
                  N("The exact SPF, DKIM, DMARC, MTA-STS and TLS-RPT records each domain should publish, with a check for whether each one is live."),
                  aliases: "SPF record|DKIM record|DMARC record|MTA-STS record|TLS-RPT record|TXT records|Reverse DNS|PTR record|What DNS records do I need|Publish DNS records|DNS checklist|Sender authentication",
                  seeAlso: "externalsetup|domains|security")),

            Group("Globe24", N("Accounts & domains"),
               N("The people and the domains this server carries mail for."),
               "Users|Mailboxes|People|Addresses|Tenants",

               // A domain administrator's page since 2007: Application.Domains
               // answers any signed-in credential, and narrows the collection
               // to the credential's own domain for anything less than the
               // server administrator.
               Page("domains", N("Domains"),
                  N("Add domains, accounts, aliases, distribution lists, DKIM signing and external (POP3) collection."),
                  aliases: "Domains & accounts|Accounts|Users|Mailboxes|Add a user|Aliases|Distribution lists|Mailing list|Quota|Size limit|DKIM|External accounts|Domain aliases|Signatures|Forwarding",
                  // The suppression list is the other page a domain administrator
                  // may open, so it is this page's link for that role, the server
                  // administrator's pages being left out of it (RelatedOf).
                  seeAlso: "publicfolders|groups|rules|suppressions|sendout",
                  needs: "administer_domain"),

               Page("groups", N("Groups"),
                  N("Security groups, so shared-folder permissions can be granted to several accounts at once."),
                  aliases: "Security groups|Membership|Permission groups",
                  seeAlso: "publicfolders|domains"),

               Page("publicfolders", N("Public folders"),
                  N("Shared IMAP folders, and which accounts or groups may read and write each one."),
                  aliases: "Shared folders|Shared mailbox|IMAP ACL|Permissions|Team folder|Shared calendar",
                  seeAlso: "groups|domains"),

               Page("roles", N("Administrator roles"),
                  N("Who may administer what: the named roles this server has, and the accounts that hold them."),
                  aliases: "Roles|Permissions|Help desk|Auditor|Domain administrator|Delegation|Least privilege|Who can do what",
                  seeAlso: "adminaccess|groups|domains",
                  needs: "view_configuration")),

            Group("MailArrowForward20", N("Mail flow & delivery"),
               N("How mail gets in, where it goes next, and what happens to it on the way."),
               "Delivery|Routing|Transport|SMTP|Relay|Mail flow",

               Page("delivery", N("Delivery of e-mail"),
                  N("Relaying to an upstream server, retry intervals, bounce handling and what happens to local mail."),
                  aliases: "SMTP relayer|Smarthost|Send through my ISP|Retries|Bounce|Non-delivery report|Deliver local to local|Rule loop limit|Sensitive content|Account-number pattern",
                  // Partners first, as the breadcrumb's Related bar draws them first
                  // wherever there is room: the Rules page, whose limits are on this
                  // page's Rules tab, among the first three.
                  seeAlso: "routes|queue|rules|ipranges|servermessages"),

               Page("routes", N("Routes"),
                  N("Deliver mail for named domains to a chosen server instead of looking up its MX record."),
                  aliases: "Static route|Transport map|Send domain to server|Internal relay|Hybrid|Split delivery",
                  seeAlso: "delivery|remotedomains|mxquery"),

               // Beside the routes because that is where somebody who has just
               // set one up will look next, and because the two are the pair an
               // administrator confuses: a route decides WHERE mail for a domain
               // goes, a remote domain policy decides what this server will do
               // when it gets there.
               Page("remotedomains", N("Remote domains"),
                  N("Per remote domain: the TLS this server demands in each direction, the size and connections it will attempt, and recipient verification for a domain it backs up."),
                  aliases: "Mandatory TLS|TLS policy|Require TLS|TLS policy map|Connector|Partner domain|Recipient callout|Recipient verification|Backup MX|Secondary MX|Rate limit|Per-domain limits",
                  seeAlso: "routes|tlsoverview|delivery"),

               Page("relays", N("Incoming relays"),
                  N("Upstream gateways whose IP address must not be treated as the connecting client in spam checks."),
                  aliases: "Trusted relay|Upstream gateway|Front-end filter|Load balancer|Proxy|Received header",
                  seeAlso: "ipranges|antispam|hardening"),

               Page("rules", N("Rules"),
                  N("Server-wide rules that inspect messages as they arrive and act on them."),
                  aliases: "Global rules|Filters|Conditions and actions|Auto-reply|Move to folder|Tag subject",
                  seeAlso: "domains|scripts|delivery"),

               Page("servermessages", N("Server messages"),
                  N("The greeting, bounce and error texts the server sends back to clients."),
                  aliases: "Banner|Greeting|Bounce text|Error text|Templates|Wording|Localise messages",
                  seeAlso: "delivery|protocols"),

               Page("sendout", N("Server sendout"),
                  N("E-mail every account on the server at once - for maintenance announcements."),
                  aliases: "Announcement|Mail all users|Broadcast|Notify everyone|Maintenance notice",
                  seeAlso: "domains")),

            Group("PlugConnected24", N("Connections & protocols"),
               N("Which services listen, on what, and how clients reach them."),
               "Network|Ports|Listeners|Protocols|Bindings|Client access",

               Page("ports", N("TCP/IP ports"),
                  N("The addresses and ports each protocol listens on, and the connection security used on each."),
                  aliases: "Listeners|Bindings|Port 25|Port 465|Port 587|Submission|Port 993|Port 995|STARTTLS|Implicit TLS|Interface|Bind address|Open a port",
                  seeAlso: "certs|tls|protocols"),

               // The switches for how a client authenticates - AUTH PLAIN and LOGIN on
               // SMTP, SASL PLAIN on IMAP - are this page's, so their names are its
               // aliases; they sent the palette to Authentication, which has none of
               // them (review finding F238). This server offers neither CRAM-MD5 nor
               // APOP, so a search for them lands where the mechanisms it does offer
               // are switched.
               Page("protocols", N("Protocols"),
                  N("Per-protocol limits and behaviour for SMTP, POP3 and IMAP."),
                  aliases: "SMTP|POP3|IMAP|Timeouts|Max connections|Message size limit|Max message size|Host name|HELO name|EHLO|Authentication required|Welcome banner|" +
                           "SASL|SASL PLAIN|AUTH PLAIN|AUTH LOGIN|Plain-text authentication|CRAM-MD5|APOP",
                  seeAlso: "ports|delivery|authentication|servermessages"),

               Page("webservices", N("Web services & autoconfiguration"),
                  N("The built-in HTTP listener: Autodiscover, autoconfig, the MTA-STS policy and ACME challenges."),
                  aliases: "Web services & client autoconfiguration|HTTP|Autodiscover|Autoconfig|Outlook setup|Thunderbird setup|Mobile setup|MTA-STS policy|Well-known|Web server",
                  seeAlso: "acme|security|api|webmail"),

               // The webmail's settings, and the other browser pages people sign in to
               // (the self-service portal, the Control Deck): sign-in, sessions, what a
               // person may do, OpenPGP, Web Push, importing. One self-contained block,
               // added with the page after 6.3.4.
               Page("webmail", N("Webmail"),
                  N("How people sign in to the webmail and the Control Deck, how long a session lasts, and what a person may do: passkeys, single sign-on, recovery, OpenPGP, notifications and importing."),
                  aliases: "Webmail|Portal|Self-service|Browser sessions|Session timeout|Passkeys|WebAuthn|Single sign-on|SSO|OpenID Connect|OIDC|SAML|Entra ID|Okta|Account recovery|Forgotten password|OpenPGP|Web Key Directory|Zero-access|Web Push|Notifications|VAPID|Import from Gmail|Import from Outlook.com|Masked addresses|Recall|Offline mail|Directory|BIMI|Brand logos",
                  seeAlso: "authentication|webservices|api"),

               Page("dns", N("DNS resolver"),
                  N("Which name servers the server asks, and whether answers are DNSSEC validated."),
                  aliases: "Name servers|Resolver|DNSSEC|Lookup failures|DNS cache|EDNS",
                  seeAlso: "security|antispam|mxquery")),

            Group("LockClosed24", N("Certificates & encryption"),
               N("Is transport security correct, and will it still be correct next month?"),
               "TLS & certificates|TLS|SSL|Certificates|Encryption|MTA-STS|DANE|Transport security",

               Page("certs", N("SSL certificates"),
                  N("The certificates this server presents, which port uses which, and when each expires."),
                  aliases: "Certificate|SSL certificate|TLS certificate|PFX|PEM|Expiry|Renew|Install a certificate|Chain|Private key",
                  seeAlso: "acme|ports|security"),

               Page("acme", N("Automatic certificates (ACME)"),
                  N("Issue and renew certificates automatically from Let's Encrypt or another ACME authority."),
                  aliases: "Certificates (ACME)|ACME|Let's Encrypt|Letsencrypt|Automatic renewal|http-01|Certbot|Free certificate",
                  seeAlso: "certs|webservices|ports"),

               // The TLS half of what used to be "Auto-ban & SSL/TLS". The title
               // is new because the old one named two subjects and this page is
               // now one of them; the old title stays as an alias, and it is also
               // an alias on the Auto-ban page, so a note naming it reaches
               // whichever half its reader wanted.
               // Four pages own the pieces of transport security and none of them
               // can answer the question an administrator arrives with: is anything
               // here carrying a password in the clear, and will it still serve a
               // valid certificate next month? The interesting states only exist in
               // the combination - a port set to use TLS with no certificate does
               // not fall back to plaintext, it fails to start; the AEAD-ONLY preset
               // leaves TLS 1.0 and 1.1 advertised with no suite they can use; the
               // one control that refuses a plaintext password lives on the IP
               // ranges page, which is why it is so often never seen.
               //
               // Read-only, like the spam and virus overviews, and for the same
               // reason: it must not become a fifth place to change the same value.
               Page("tlsoverview", N("Transport encryption overview"),
                  N("What every listener protects, which certificate it presents and when that expires, and what can still be negotiated."),
                  aliases: "TLS overview|SSL overview|Encryption overview|Is my mail encrypted|Plaintext password|Passwords in the clear|Unencrypted port|Certificate expiry|Certificate expiring|What TLS am I using|STARTTLS not required",
                  seeAlso: "tls|certs|ports"),

               Page("tls", N("SSL/TLS"),
                  N("Which TLS versions and ciphers this server will negotiate, and whether it verifies the certificate of the server it delivers to."),
                  aliases: "Auto-ban & SSL/TLS|TLS versions|TLS 1.0|TLS 1.1|TLS 1.2|TLS 1.3|Ciphers|Cipher list|Cipher suites|OpenSSL cipher string|Disable old TLS|Weak ciphers|ChaCha20|Verify remote certificate",
                  // autoban stays in this list. This page and Auto-ban are the two
                  // halves of the old "Auto-ban & SSL/TLS", and PageSplitTests holds
                  // them to signposting each other so a reader who followed the old
                  // title and landed on the wrong half can get to the right one.
                  seeAlso: "tlsoverview|security|autoban|certs|ports"),

               // Four tabs since review finding F228 - Encryption, Signing, Forwarding
               // and Reports - and a description of all four: it spoke of outbound TLS
               // alone, over ten cards of which six are mail authentication and three
               // are about inbound mail, and it names the checks on inbound mail - the
               // expired DKIM signatures refused, ARC chains from trusted sealers - as
               // well as the signatures added. Its aliases name what those cards hold,
               // and AliasTabs the tab each one is on.
               Page("security", N("Transport security"),
                  N("Encryption to other servers, the signatures this server adds and checks, the results it records, forwarded mail and the reports it sends."),
                  aliases: "DANE|TLSA|MTA-STS|TLS-RPT|TLS reporting|ARC|Downgrade|Opportunistic TLS|Enforce TLS|" +
                           "SRS|Sender Rewriting Scheme|BATV|Bounce address tag|Forged bounces|DKIM oversigning|Oversign|DKIM expiry|DKIM timestamps|" +
                           "Authentication-Results|Received-SPF|DMARC reports|DMARC aggregate reports|rua",
                  seeAlso: "certs|dns|webservices")),

            Group("ShieldCheckmark24", N("Spam & virus filtering"),
               N("Everything that decides whether a message is wanted."),
               "Anti-spam|Antispam|Anti-virus|Antivirus|Filtering|Junk|Malware|Content filtering",

               // The group had five spam pages and no answer to the question an
               // administrator actually arrives with: what will all of this do to
               // a message? Each page is a correct editor for its own subject and
               // none of them can show the pipeline - the order the checks run in,
               // what each one adds to the score, the two thresholds that turn a
               // score into a verdict, and the several arrangements in which the
               // whole thing silently does nothing.
               //
               // Deliberately read-only. It is a diagnosis, not a sixth editor:
               // every row links to the page that owns the setting, which is also
               // what keeps this page from becoming a second place to change the
               // same value.
               Page("spamoverview", N("Spam filtering overview"),
                  N("Every spam check in the order the server runs them, what each adds to the score, and what that score then does to the message."),
                  aliases: "Spam overview|Anti-spam overview|Spam pipeline|Spam summary|What is my spam configuration|Spam checks|Order of spam checks|Spam thresholds|Spam verdict|Why was this marked as spam",
                  seeAlso: "antispam|spamwhitelist|logs"),

               // Placed next to the settings that fill it rather than under a
               // diagnostics heading: the reason somebody opens this page is almost
               // always that a threshold on the anti-spam page just caught something
               // it should not have.
               Page("quarantine", N("Quarantine"),
                  N("Messages held for review instead of being refused, and the two things you can do with each: release it to the recipient, or delete it."),
                  aliases: "Quarantine|Held messages|Release a message|False positive|Review spam|Quarantined mail|Where did my email go|Blocked message|Recover a message|Spam review queue",
                  seeAlso: "antispam|spamoverview|spamwhitelist"),

               Page("antispam", N("Anti-spam settings"),
                  N("Scores and thresholds, SPF, DKIM and DMARC checks, greylisting, and SpamAssassin."),
                  aliases: "Anti-spam|Spam filter|Stop spam|Score|Threshold|SPF|DKIM|DMARC|ARC|Greylisting|SpamAssassin|Junk mail|PTR|HELO check|Subject prefix",
                  seeAlso: "spamoverview|quarantine|dnsbl|surbl|spamwhitelist|greylistwhitelist"),

               // The overview is first in most of these lists on purpose: it is the
               // one page that says what the change just made on this page does to
               // a message, and the first links are the ones drawn beside the
               // breadcrumb where there is room, and the first in its drop-down
               // where there is not. A page's own partner comes first where the two
               // are a pair: the greylisting lists, the two sender lists.
               Page("surbl", N("SURBL servers"),
                  N("Block lists checked against the links found inside a message body."),
                  aliases: "SURBL|URI block list|Link blacklist|Body URL check|Spamhaus DBL",
                  seeAlso: "spamoverview|antispam|dnsbl"),

               Page("dnsbl", N("DNS block lists (DNSBL)"),
                  N("Block lists checked against the IP address that is connecting."),
                  aliases: "DNSBL|DNS blacklists (DNSBL)|DNS blacklists|DNS block lists|RBL|Blackhole list|Spamhaus|Barracuda|Block an IP by reputation",
                  seeAlso: "spamoverview|ipranges|antispam|surbl"),

               Page("spamwhitelist", N("Spam exemptions"),
                  N("Senders and IP ranges that bypass spam protection entirely."),
                  aliases: "White list|Anti-spam white list|Whitelist|Spam filter exemptions|Allow list|Safe senders|False positive|Let a sender through|Exempt from spam filter",
                  seeAlso: "spamoverview|antispam|greylistwhitelist"),

               Page("blockedsenders", N("Blocked senders"),
                  N("Envelope senders refused outright or scored - one address, or a whole domain with its subdomains."),
                  aliases: "Blacklist|Block list|Deny list|Block a sender|Banned senders|Refuse mail from",
                  seeAlso: "senderlist|domainpolicylist|spamwhitelist|antispam|spamoverview"),

               // Beside Blocked senders on purpose, because the two are easy to
               // confuse and are not the same thing: that list refuses during the
               // SMTP conversation for every recipient at once; this one decides at
               // delivery, per recipient, and a recipient's own list or domain's
               // list can overrule it.
               Page("senderlist", N("Allow and block list"),
                  N("Addresses and domains every account wants or does not, below each account's own list and its domain's."),
                  aliases: "Tenant allow list|Tenant block list|Sender allow list|Sender block list|Allow a sender for everyone|Never junk from|Precedence of sender lists",
                  seeAlso: "blockedsenders|spamwhitelist|quarantine"),

               Page("greylistwhitelist", N("Greylisting exemptions"),
                  N("IP addresses exempt from the greylisting delay, for senders that will not retry."),
                  aliases: "Greylisting white list|Greylist whitelist|Greylist exemption|Greylisting delay|Slow mail|First message delayed|Retry delay",
                  seeAlso: "greylistbypass|spamoverview|antispam|spamwhitelist"),

               // Beside the greylisting exemptions on purpose: that list exempts an ADDRESS from
               // greylisting on no evidence, this one exempts a DOMAIN on the evidence
               // its mail carries, and the difference is the whole point of it.
               Page("greylistbypass", N("Greylisting bypass list"),
                  N("Domains and HELO suffixes let past greylisting only when the message passes SPF, DKIM or DMARC as the entry requires."),
                  aliases: "Greylisting bypass|Skip greylisting for a domain|Bypass greylisting on DKIM|Bypass greylisting on DMARC|Partner domain greylisting|Greylist exemption by domain",
                  seeAlso: "greylistwhitelist|antispam|spamoverview"),

               // Keyed by domain where the two lists above are keyed by address,
               // for the correspondent whose outbound hosts change every week:
               // an allowed one skips greylisting and the score's refusals while the
               // tests still run, a blocked one is a 550 at MAIL FROM before anything
               // runs. Named in the allow and block words of every list here (review
               // finding F214); the white and black names are its aliases.
               Page("domainpolicylist", N("Allowed and blocked domains"),
                  N("Sending domains this server has decided about: an allowed one skips greylisting and the score's refusals, a blocked one is refused at MAIL FROM - by domain or HELO suffix, not by IP address."),
                  aliases: "Domain white and black list|Domain allow and block list|Domain white list|Domain black list|Domain whitelist|Domain blacklist|Always accept a domain|Never accept a domain|HELO suffix|Refuse a domain|Trust a domain whose SPF is broken|Rotating IP addresses",
                  seeAlso: "antispam|spamwhitelist|blockedsenders"),

               // The same argument as the spam overview above, and the anti-virus
               // case is sharper: the settings page can show that a scanner is
               // switched on and cannot show whether it is able to run. A scanner
               // enabled with a missing host or executable errors on every message,
               // and VirusScanner::ScanFile_ treats "every scanner errored" as
               // NoVirusFound - so the mail is delivered as though it had been
               // examined. Nothing in an editor for those settings can say that.
               Page("virusoverview", N("Virus scanning overview"),
                  N("Which scanners can actually run, what size of message is scanned, and what happens to one that is found to be infected."),
                  aliases: "Anti-virus overview|Antivirus overview|Virus overview|Virus summary|What is my virus configuration|Is virus scanning working|Am I scanning for viruses|Scanner not working|ClamAV not scanning|Unscanned mail",
                  seeAlso: "antivirus|blockedattachments|logs"),

               Page("antivirus", N("Anti-virus settings"),
                  N("ClamAV and external scanners, and what to do with a message that is found to be infected."),
                  aliases: "Anti-virus|Virus scanner|ClamAV|Malware|Infected mail|Scan attachments|External scanner",
                  seeAlso: "virusoverview|blockedattachments"),

               Page("blockedattachments", N("Blocked attachments"),
                  N("File-name patterns stripped from incoming messages regardless of what a scanner says."),
                  aliases: "Attachment blocking|Block exe|File extensions|Strip attachments|Dangerous files|Zip",
                  seeAlso: "virusoverview|antivirus")),

            Group("ShieldKeyhole24", N("Sign-in & access"),
               N("Who may connect, who may authenticate, and what happens when someone keeps trying."),
               "Access & abuse protection|Security|Access control|Authentication|Abuse|Brute force|Who can connect",

               // Its cards in the order a server needs them: password storage, SMTP
               // AUTH, the browser pages' sign-in, then OAuth2. Two-factor, SASL,
               // CRAM-MD5 and APOP were its aliases and none of them is here: the
               // second factor is on Administrative access, the mechanisms on
               // Protocols (review finding F238).
               Page("authentication", N("Authentication"),
                  N("How mailbox passwords are stored, where SMTP AUTH is offered, and tokens from an external identity provider (OAuth2 / XOAUTH2)."),
                  aliases: "Password hashing|Password storage|bcrypt|Argon2|scrypt|PBKDF2|Pepper|SMTP AUTH|OAuth2|XOAUTH2|Modern authentication|Microsoft 365|Google",
                  seeAlso: "protocols|ldap|adminaccess|autoban|ipranges"),

               // The administrator's password and second factor, and - because
               // "Password" is where somebody looks for it - the policy for mailbox
               // passwords, which nothing in the title says: its aliases find it.
               Page("adminaccess", N("Administrative access"),
                  N("The administrator's password and second factor, and the rules every mailbox password is held to: length and characters, common and breached passwords, reuse and expiry."),
                  aliases: "Admin password|Administrator password|Change the admin password|Remote administration|COM API access|" +
                           "Two-factor|Second factor|2FA|TOTP|One-time code|Authenticator app|" +
                           "Password policy|Password rules|Password length|Password complexity|Password expiry|Password history|Breached passwords|Pwned passwords",
                  seeAlso: "roles|authentication"),

               Page("ipranges", N("IP ranges"),
                  N("Which addresses may connect, which may relay without authenticating, and which must authenticate first."),
                  aliases: "IP range|Firewall|Block an IP|Allow an IP|Open relay|Relay permissions|Require authentication|Let a device send mail|Printer|Scanner|LAN|Localhost|My IP",
                  seeAlso: "autoban|authentication|dnsbl|relays|delivery"),

               // The brute-force half of what used to be "Auto-ban & SSL/TLS".
               // The two subjects on that page shared nothing: an administrator
               // arrives at one of them because somebody is guessing passwords and
               // at the other because a client cannot negotiate a cipher, and
               // whichever they wanted, half the page was noise. Splitting it also
               // lets each half sit in the group where its subject lives - this one
               // beside IP ranges, which is where the ban it creates ends up.
               Page("autoban", N("Auto-ban"),
                  N("Lock out an address that keeps failing to log on - and let one back in that should not have been locked out; refuse or allow whole countries, and hold the mail of an account that looks taken over."),
                  aliases: "Auto-ban & SSL/TLS|Autoban|Countries|Country blocking|Block a country|GeoIP|MaxMind|Compromised accounts|Account takeover|Hijacked account|Brute force|Bruteforce|Lockout|Locked out|Failed logons|Failed logins|Password guessing|Dictionary attack|Fail2ban|Hammering|Temporary ban|Logon failure list|Never ban|Never-ban list|Do not ban|Reverse name|Provider banned|Widen ban|Neighbours|Neighbors|PTR|Subnet ban",
                  // "tls" is here, and "autoban" is on the SSL/TLS page, so a
                  // reader who followed an old note about the combined page and
                  // wanted the other half has a way across. The breadcrumb draws as
                  // many as fit and puts the rest in its More drop-down, and each
                  // half lists the other among its first three (RelatedLinksTests),
                  // so the way across is drawn wherever there is room for three, and
                  // is among the first the drop-down lists in a narrow window.
                  seeAlso: "ipranges|authentication|tls|logs"),

               // Directory authentication is how accounts prove who they are, so it
               // belongs beside Authentication - not filed under "advanced INI" because
               // that happens to be where its settings are stored.
               Page("ldap", N("Directory authentication (LDAP)"),
                  N("Authenticate accounts against Active Directory or any LDAP directory, so users sign in with their domain password."),
                  aliases: "LDAP|Active Directory|AD|Directory|Domain password|Bind|LDAPS|StartTLS|Single sign-on|Domain accounts|DC",
                  seeAlso: "directorysync|authentication|adminaccess|domains"),

               // Beside directory authentication, and deliberately not folded into it.
               // The two answer different questions: that page is about whether somebody
               // who already has a mailbox can log into it, this one about which people
               // have a mailbox at all. Somebody arriving to bulk-create accounts from
               // the directory is not looking for a bind method, and somebody debugging
               // a logon does not want a provisioning run one click away.
               Page("directorysync", N("Directory synchronisation"),
                  N("Create and update mailboxes to match an LDAP directory - preview first, then apply."),
                  aliases: "Directory sync|Provisioning|Provision accounts|Account source|Bulk create accounts|Import users|Sync users|LDAP sync|AD sync|Create accounts from Active Directory|Onboarding|Leavers",
                  seeAlso: "ldap|domains")),

            Group("Pulse24", N("Monitoring & troubleshooting"),
               N("Is mail flowing, and if it is not, where has it stopped?"),
               "Status|Health|Troubleshooting|Problems|Something is wrong|Observability",

               Page("status", N("Server status"),
                  N("Which services are running, who is connected right now, and how long the server has been up."),
                  aliases: "Sessions|Connections|Uptime|Running|Service state|Live sessions",
                  seeAlso: "queue|dashboard|logs"),

               Page("queue", N("Delivery queue"),
                  N("Messages still waiting to be delivered, why each is waiting, and when it will be retried."),
                  aliases: "Spool|Stuck mail|Pending mail|Outbound queue|Retry|Not delivered|Backlog",
                  seeAlso: "stalledmail|messagetrace|logs|delivery|status"),

               Page("logs", N("Live logs"),
                  N("Stream the server log as it is written - the fastest way to see what happened to one message."),
                  aliases: "Log viewer|Tail|Trace|Debug|SMTP conversation|Error log|Awstats",
                  seeAlso: "logging|queue|diagnostics"),

               Page("logging", N("Logging"),
                  N("Which log files the server writes, how much detail they carry and how long they are kept, what the audit trail records, and the alerts, outbound webhooks and SNMP agent that tell another system what happened."),
                  aliases: "Log level|Log files|Log folder|Retention|Delete old logs|Verbosity|Debug logging|Syslog|Alerts|Alert rules|Webhooks|Outbound webhooks|SNMP|SNMP agent|Audit trail settings",
                  seeAlso: "logs|messagetrace|audit|performance"),

               // The guide to mail that stops moving on a healthy-looking server,
               // as a page with a button beside each step. The queue shows that a
               // message is waiting; this is what to do when the reason is not on
               // the queue page - which is the case the guide was written for.
               Page("stalledmail", N("Diagnosing stalled mail"),
                  N("Mail is not moving and nothing has crashed: which half has stalled, the log lines that name the cause, and the setting that bounds each stage."),
                  aliases: "Stalled mail|Slow mail|Mail is stalled|Timed out while sending end of data|Accepted but never delivered|Hung|Wedged|Stall diagnosis|Diagnosing slow mail",
                  seeAlso: "queue|logs|diagnostics"),

               // Next to the queue on purpose: the two answer the same question at
               // different moments. The queue says where a message is now; the trace
               // says what happened to one that has already gone.
               Page("messagetrace", N("Message trace"),
                  N("What happened to a particular message - searchable by sender or recipient, instead of grepping logs."),
                  aliases: "Message trace|Delivery history|Where did my email go|Did it arrive|Track a message|Was it delivered|Message log|Audit|What happened to|Trace",
                  seeAlso: "queue|suppressions|logs|quarantine"),

               Page("diagnostics", N("Diagnostics"),
                  N("Run the built-in connectivity and configuration checks, and read the last message-store consistency scan."),
                  aliases: "Self test|Health check|Port 25 test|Connectivity|Message store consistency|Missing files",
                  seeAlso: "mxquery|logs|queue"),

               Page("mxquery", N("MX query"),
                  N("Look up where e-mail for a domain is delivered, to check DNS before blaming the server."),
                  aliases: "MX-query|MX lookup|DNS lookup|nslookup|Mail exchanger|Where does mail go",
                  seeAlso: "routes|dns|diagnostics"),

               // Beside the trace, because it answers the question that follows
               // one: the message bounced, and now the address is not being sent
               // to - why, since when, and how to undo it. A domain administrator's
               // page as well: Utilities.GetSuppressionList and the rest answer the
               // administrator of that domain, and the domains it picks from are
               // Application.Domains, narrowed to the credential's own.
               Page("suppressions", N("Suppression list"),
                  N("Addresses a domain no longer sends to because mail to them bounced - visible, and removable by hand."),
                  aliases: "Bounces|Hard bounce|Dead addresses|Suppression|Bounce list|Why is mail to this address refused|Stop sending to an address",
                  seeAlso: "messagetrace|queue|delivery|domains",
                  needs: "administer_domain"),

               // Who changed what: every administrative change as one row of a hash
               // chain, over Utilities.GetAuditTrail, with a setting's change put back
               // from its row. The server administrator's: the trail records every
               // domain's changes.
               Page("audit", N("Audit trail"),
                  N("Who changed what, when, over which interface and from where - every administrative change as one row in a hash chain - and a setting put back to the value it had."),
                  aliases: "Audit|Who changed|History|Change log|Put back|Undo a setting|Revert a change|Compliance",
                  seeAlso: "journaling|discovery|adminaccess|logs",
                  needs: "administer_server"),

               // Beside the audit trail, because both answer a compliance
               // question: who changed what, and where a copy of the mail goes.
               Page("journaling", N("Journaling and dual delivery"),
                  N("A copy of a domain's mail, or of one account's, sent to an address - as an envelope report with the message attached, or as the message itself."),
                  aliases: "Journaling|Journal|Dual delivery|Compliance archive|Copy every message|Send a copy elsewhere|Archive to another system|Migration copy|Envelope report|Blind copy to an archive",
                  seeAlso: "audit|messagetrace",
                  needs: "administer_server"),

               // eDiscovery, read-only, beside the audit trail its lines come from.
               // Read over the REST API, since discovery has no COM surface; the
               // cases are opened, searched, held and exported in the Deck. The
               // permission is the discovery reader's, so an auditor sees it.
               Page("discovery", N("Discovery"),
                  N("The eDiscovery cases, read-only: each case's searches, holds and exports, and every line the audit trail holds for it. A case is opened, searched, held and exported in the Control Deck."),
                  aliases: "eDiscovery|Legal hold|Litigation hold|Case|Custodian|Preserve mail|Search every mailbox|Export for a lawyer|Discovery search",
                  seeAlso: "audit|journaling",
                  needs: "view_discovery"),

               Page("api", N("API & monitoring"),
                  N("The REST API, Prometheus metrics and health endpoints an outside monitoring system reads, the Windows Event Log, the ManageSieve listener, and how long a stopping service waits for its sessions."),
                  aliases: "REST|Prometheus|Grafana|Metrics|Health endpoint|Nagios|Zabbix|Automation|OpenTelemetry|OTLP|Windows Event Log|Event Viewer|ManageSieve|Sieve|Sieve scripts|Graceful shutdown|Drain on stop",
                  seeAlso: "apikeys|logging|status|webservices"),

               // Its own page rather than a card on API & monitoring: keys are a
               // list of objects with their own lifecycle, and the page has to be
               // usable while the REST listener is switched off - which is the
               // state every installation is in when it first comes here.
               Page("apikeys", N("REST API keys"),
                  N("Scoped, expiring credentials for the REST API, so automation does not have to carry the administrator password."),
                  aliases: "API key|Bearer token|Access token|Credential|Service account for the API|Revoke a key|hmapi|Automation credential|Read-only key",
                  seeAlso: "api|adminaccess")),

            Group("Wrench24", N("Maintenance"),
               N("Housekeeping, tuning, and the settings that still have no better home."),
               "Housekeeping|Backup|Tuning|Advanced|Scripting|INI",

               Page("backup", N("Backup & restore"),
                  N("Back up or restore the configuration, the domains and the messages."),
                  aliases: "Backup|Restore|Export|Import|Disaster recovery|Copy to another server|Migrate",
                  seeAlso: "hardening|domains"),

               // Its own page since review finding F272: Check, Download and Install
               // were on Server status and the policy that drives them a card on API
               // & monitoring, nothing was called Updates, and Ctrl+K "update" found
               // only setting rows ranked last. Beside Backup & restore, because an
               // unattended apply backs up first. Server status, the Dashboard and
               // About say in one line when a release is waiting, with the way here.
               Page("updates", N("Updates"),
                  N("Whether a newer release is waiting, and how this server checks for one, downloads it and installs it - by a click, or on its own inside a window you set."),
                  aliases: "Update|Upgrade|New version|New release|Live update|Patch|Release channel|Prerelease|Automatic updates|Unattended update|Update window|Check for updates|Install update|Update feed|Update policy|Sigstore",
                  seeAlso: "status|backup|about"),

               Page("performance", N("Performance"),
                  N("Thread counts, database connections and caching - what to change when the server is slow."),
                  aliases: "Threads|Workers|Database connections|Cache|Slow server|Tuning|Concurrency|Memory",
                  seeAlso: "hardening|logging"),

               // Was "Advanced & scripting", which named this page after the one thing
               // it shares with its neighbour: the engine's on/off switch lives here,
               // the script itself is edited on "Event scripts". Two names for adjacent
               // concepts, one of them a catch-all - so this one is now what the page
               // calls itself, and its description says where the switch is. The old
               // title stays as an alias; the key is unchanged, so links still arrive.
               Page("advanced", N("Advanced"),
                  N("Archiving, mirroring, the default domain, IPv6 delivery, disk-space limits, mailbox migrations and PST import, and the switch that turns the event-scripting engine on."),
                  aliases: "Advanced & scripting|Archive|Archiving|Mirror|Default domain|Scripting engine|Enable scripting|VBScript|JScript|JavaScript|IPv6 preference|Prefer IPv6|Disk space|Low disk space|Free disk space|Migration|Migrations|Mailbox migration|IMAP migration|PST import|Import a PST file|Outlook data file",
                  seeAlso: "scripts|hardening"),

               Page("scripts", N("Event scripts"),
                  N("Edit the event-handler script the server runs on delivery, logon and error events. The engine itself is switched on under Advanced."),
                  aliases: "EventHandlers|Scripts|OnDeliverMessage|OnClientLogon|OnAcceptMessage|VBScript|JScript|JavaScript|Run code on mail",
                  seeAlso: "advanced|rules"),

               // Renamed from "Advanced INI settings", which named the page after
               // the file its values are stored in. Everything on it that belonged
               // to a feature with a page of its own has moved to that page; what
               // is left really is server-wide, and the title now says so. Both old
               // titles stay as aliases, and the key is unchanged, so every link,
               // bookmark and remembered path still arrives here.
               Page("hardening", N("Server limits & expert settings"),
                  N("Server-wide ceilings, durability and abuse controls that belong to no single protocol or feature - the PROXY protocol and XCLIENT, per-account sending limits and the event-script timeout among them."),
                  aliases: "Advanced INI settings|Advanced hardening|INI settings|hMailServer.INI|Registry|Undocumented|Expert settings|Everything else|" +
                           "Timeouts|Queue bounds|Sending limits|Rate limit|Throttle|fsync|Durability|DPAPI|Received headers|mailer-daemon|" +
                           "PROXY protocol|HAProxy|XCLIENT|Front-end proxy|Sending limits per account|Per-account limits|Script timeout",
                  seeAlso: "advanced|performance|backup")),

            Page("about", N("About"),
               N("Version, build, licence and where to get support."),
               aliases: "Version|Build|Licence|License|Support|Credits|Copyright",
               needs: EveryCredential)
         };
      }
   }
}
