// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Reflection;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Documents;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using Typography = hMailServer.ControlPanel.Services.Typography;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// About page: where to get help first, then the versions, then who maintains
   /// it (the owner's choice of 9 October 2026, review finding F314). The page's
   /// purpose promised "where to get support" and it had no forum, no issue tracker
   /// and nothing to copy into a report; the versions were a line under a caption;
   /// the developer's name was set larger than the product's; and the copy read as
   /// a brochure ("a modern administration app", "re-armed with modern
   /// cryptography"). The text says what is so, and the name is ordinary strong text.
   /// </summary>
   public class AboutView : UserControl, IPageLifecycle, IRefreshablePage
   {
      private const string ForumUrl = "https://www.hmailserver.co.uk/";   // no-loc: the project's forum (CONTRIBUTING.md)
      private const string IssuesUrl = "https://gitlab.com/hmailserver/hmailserver/-/issues";   // no-loc
      private const string SourceUrl = "https://gitlab.com/hmailserver/hmailserver";   // no-loc
      private const string LicenceUrl = "https://gitlab.com/hmailserver/hmailserver/-/blob/master/LICENSE";   // no-loc

      private readonly TextBlock serverValue_ = Value_();
      private readonly TextBlock hostValue_ = Value_();
      private readonly TextBlock databaseValue_ = Value_();

      // The server's side of the copied details, as last read.
      private string serverVersion_ = "";
      private string host_ = "";
      private string database_ = "";

      // A newer release waiting, in one line with the page that installs it
      // (Views/UpdateNotice): About said the version and not that a newer one was
      // waiting (review finding F272).
      private readonly StackPanel updateNotice_ = new() { Visibility = Visibility.Collapsed };

      public AboutView()
      {
         var panel = new StackPanel { MaxWidth = 880, HorizontalAlignment = HorizontalAlignment.Left };

         panel.Children.Add(new PageHeader { PageKey = "about" });

         // ---- help first ------------------------------------------------------
         var help = new StackPanel();
         help.Children.Add(LabelledLink_(L("Forum"), "www.hmailserver.co.uk", ForumUrl));   // no-loc: the address shown
         help.Children.Add(LabelledLink_(L("Issues"), "gitlab.com/hmailserver/hmailserver/-/issues", IssuesUrl));   // no-loc
         help.Children.Add(LabelledLink_(L("Source code"), "gitlab.com/hmailserver/hmailserver", SourceUrl));   // no-loc

         var copy = new Wpf.Ui.Controls.Button { Content = L("_Copy version details"), Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0) };
         AutomationProperties.SetAutomationId(copy, "about-copy-version");
         ControlHelp.Explain(copy, L("The Control Panel's and the server's versions, the build, .NET, Windows and the database, for a report."));
         copy.Click += (s, e) => CopyVersionDetails_();
         help.Children.Add(copy);

         var helpCard = new Card
         {
            Title = L("Get help"),
            Description = L("Ask on the forum, or report a defect on the issue tracker. Copy the version details into either: they are what anyone helping asks for first."),
            Content = help
         };
         helpCard.SetResourceReference(MarginProperty, "AppCardGap");
         panel.Children.Add(helpCard);

         // ---- the versions, a label and a value a line -------------------------
         var versions = new Grid();
         versions.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         versions.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         AddVersion_(versions, L("Control Panel"), Value_(ControlPanelVersion_()));
         AddVersion_(versions, L("Server"), serverValue_);
         AddVersion_(versions, L("Connected to"), hostValue_);
         AddVersion_(versions, L("Database"), databaseValue_);
         AddVersion_(versions, ".NET", Value_(Environment.Version.ToString()));   // no-loc: a product's name
         AddVersion_(versions, "Windows", Value_(Environment.OSVersion.Version.ToString()));   // no-loc: a product's name
         AddVersion_(versions, L("Licence"), Link_("GNU AGPLv3", LicenceUrl));   // no-loc: the licence's name

         var inner = new StackPanel();
         inner.Children.Add(versions);
         updateNotice_.Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0);
         inner.Children.Add(updateNotice_);

         var card = new Card
         {
            Title = L("hMailServer Control Panel"),
            Description = L("Administers an hMailServer: its domains and accounts, the delivery queue, the logs, the server's settings and its transport security."),
            Content = inner,
            Footer = Text(L("hMailServer is free and open source software, licensed under the GNU AGPLv3. This Control Panel is built with WPF-UI (Fluent design) and LiveCharts2 on .NET 10."),
               "TextCaptionTertiary", new Thickness(0))
         };
         card.SetResourceReference(MarginProperty, "AppCardGap");
         panel.Children.Add(card);

         // ---- who maintains it --------------------------------------------------
         var dev = new StackPanel();

         dev.Children.Add(Text("Christopher Holloway", "TextBodyStrong", new Thickness(0, 0, 0, 2))); // no-loc
         dev.Children.Add(Text("Progressive Robot Ltd", "TextBody", new Thickness(0, 0, 0, 12))); // no-loc

         dev.Children.Add(Text(L("Progressive Robot Ltd is a software engineering company. It maintains the hMailServer 6.x line, an independent fork of the open-source mail server."),
            "TextBody", new Thickness(0, 0, 0, 12)));
         dev.Children.Add(Text(L("hMailServer 6.x is built with Visual Studio 2026 (MSVC v145) and .NET 10. To the original it adds PBKDF2 and Argon2id password hashing, SCRAM-SHA-256 and OAuth2 sign-in, DANE with DNSSEC, MTA-STS, ARC, Ed25519 DKIM, TLS-RPT, ACME (Let's Encrypt), Sieve and ManageSieve filtering, health checks and OpenTelemetry. This Control Panel replaces the earlier hMailServer Administrator."),
            "TextBody", new Thickness(0, 0, 0, 12)));
         dev.Children.Add(LabelledLink_(L("Web"), "www.progressiverobot.com", "https://www.progressiverobot.com"));   // no-loc: the address

         panel.Children.Add(new Card
         {
            Title = L("Developed & maintained by"),
            Content = dev,
            Footer = Text(L("Copyright © 2026 Christopher Holloway / Progressive Robot Ltd. hMailServer is a trademark of its respective owners; this is an independent, community-maintained fork."),
               "TextCaptionTertiary", new Thickness(0))
         });

         var scroll = new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         scroll.SetResourceReference(PaddingProperty, "AppPagePadding");
         Content = scroll;
      }

      /// <summary>
      /// The Control Panel's version and the commit it was built from, which the
      /// SDK writes after a "+" in the informational version: "6.3.6 (beca2544b)".
      /// </summary>
      private static string ControlPanelVersion_()
      {
         Assembly assembly = Assembly.GetExecutingAssembly();
         string version = assembly.GetName().Version?.ToString(3) ?? "?";
         string informational = assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "";
         int plus = informational.IndexOf('+');
         string commit = plus >= 0 ? informational.Substring(plus + 1) : "";
         return commit.Length == 0 ? version : F("{0} ({1})", version, commit.Length > 9 ? commit.Substring(0, 9) : commit);
      }

      private static TextBlock Value_(string text = "")
      {
         var block = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, DesignTokens.Space.Xs) };
         block.SetResourceReference(StyleProperty, "TextBody");
         return block;
      }

      /// <summary>One line of the versions: the label in the secondary colour, then the value.</summary>
      private static void AddVersion_(Grid grid, string label, FrameworkElement value)
      {
         int row = grid.RowDefinitions.Count;
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

         var caption = new TextBlock { Text = label, Margin = new Thickness(0, 0, DesignTokens.Space.Xl, DesignTokens.Space.Xs) };
         caption.SetResourceReference(StyleProperty, "TextBody");
         caption.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
         Grid.SetRow(caption, row);
         grid.Children.Add(caption);

         Grid.SetRow(value, row);
         Grid.SetColumn(value, 1);
         grid.Children.Add(value);
      }

      /// <summary>
      /// The details a report needs, in English whatever the Control Panel's
      /// language, so that whoever reads the report can read them - the server's
      /// line with the host whenever it is known (VersionDetailsReport).
      /// </summary>
      private void CopyVersionDetails_()
      {
         string text = VersionDetailsReport.Text(ControlPanelVersion_(), serverVersion_, host_, database_,
            Environment.Version.ToString(), Environment.OSVersion.Version.ToString());

         if (ClipboardWriter.TrySetText(text, out string error))
            Toast.Success(L("The version details are on the clipboard."));
         else
            Toast.Failure(L("Could not copy: ") + error);
      }

      private static TextBlock Text(string text, string style, Thickness margin)
      {
         var block = new TextBlock { Text = text, Margin = margin, TextWrapping = TextWrapping.Wrap };
         block.SetResourceReference(StyleProperty, style);
         return block;
      }

      /// <summary>A label in strong text, then a link that opens in the default browser.</summary>
      private static TextBlock LabelledLink_(string label, string text, string url)
      {
         TextBlock line = Link_(text, url);
         // The label is a word of its own; the gap before the link is layout.
         line.Inlines.InsertBefore(line.Inlines.FirstInline, new Run(label) { FontWeight = FontWeights.SemiBold });
         line.Inlines.InsertBefore(line.Inlines.LastInline, new Run("  "));
         line.Margin = new Thickness(0, 0, 0, DesignTokens.Space.Xs);
         return line;
      }

      /// <summary>A clickable hyperlink that opens in the default browser.</summary>
      private static TextBlock Link_(string text, string url)
      {
         var tb = new TextBlock { FontSize = Typography.Body, TextWrapping = TextWrapping.Wrap };
         var hl = new Hyperlink(new Run(text)) { NavigateUri = new Uri(url) };
         hl.RequestNavigate += (s, e) =>
         {
            Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
            e.Handled = true;
         };
         tb.Inlines.Add(hl);
         return tb;
      }

      public void OnEnter()
      {
         serverVersion_ = database_ = "";

         // The host is the session's own, known without asking the server, so that a
         // version the server does not give leaves Connected to and the report's
         // server line as they are; it was read in the version's try, and a failed
         // read of Application.Version said "Not read" of the host beside a
         // command bar that said Connected.
         host_ = ServerSession.Current?.Host ?? "";
         try
         {
            serverVersion_ = (string)ServerSession.Current.Application.Version ?? "";
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            serverVersion_ = "";
         }

         // The database a credential that may not view the configuration is not
         // told, which leaves the line Not read.
         try
         {
            dynamic db = ServerSession.Current.Application.Database;
            database_ = StatusView.DatabaseTypeName((int)db.DatabaseType);
            ServerSession.Release(db);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            database_ = "";
         }

         serverValue_.Text = serverVersion_.Length > 0 ? "hMailServer " + serverVersion_ : L("Not read");   // no-loc: the product's name
         hostValue_.Text = host_.Length > 0 ? host_ : L("Not read");
         databaseValue_.Text = database_.Length > 0 ? database_ : L("Not read");

         UpdateNotice.ShowIn(updateNotice_, "about-update-available");
      }

      public void OnLeave()
      {
      }

      void IRefreshablePage.Refresh() => OnEnter();
   }
}
