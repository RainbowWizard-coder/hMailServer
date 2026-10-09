// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The pages and cards that are not lists keep to the rule the list pages keep
   /// (ReadFailurePageTests): a read that failed is said, at the level its cause
   /// has, and never drawn as a fact.
   ///
   /// Scanned rather than rendered, as ReadFailurePageTests is: this assembly has
   /// no WPF. Each test fails against the tree before its fix.
   /// </summary>
   public class FailureStateTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      // ---- a lost link is one event at one level ---------------------------------

      /// <summary>
      /// The Dashboard says a failed read of its statistics in a notice under the
      /// header, through ReadFailure - Critical, with Try again - where it put
      /// "Connection to the server lost." in its grey subtitle.
      /// </summary>
      [Fact]
      public void TheDashboard_SaysAFailedRead_InANoticeUnderTheHeader()
      {
         XElement page = XDocument.Parse(ReadView("DashboardView.xaml")).Root;
         var children = page.Descendants(Scaffold + "PageHeader").Single().Parent.Elements().ToList();
         int header = children.FindIndex(e => e.Name == Scaffold + "PageHeader");
         Assert.True(header >= 0 && header + 1 < children.Count, "DashboardView.xaml has nothing under its header.");
         XElement next = children[header + 1];
         Assert.True(next.Name == Scaffold + "InlineNotice" && (string)next.Attribute(X + "Name") == "ReadNotice",
            "The element under the Dashboard's header is not the InlineNotice ReadNotice.");

         string source = ReadView("DashboardView.xaml.cs");
         string refresh = WithheldRowTests.MemberText(source, "private bool Refresh()");
         Assert.True(refresh != null, "DashboardView.xaml.cs no longer has private bool Refresh().");
         Assert.Contains("ReadFailure.Show(ReadNotice", refresh, StringComparison.Ordinal);
         Assert.Contains("ReadFailure.Clear(ReadNotice", refresh, StringComparison.Ordinal);
         Assert.DoesNotContain("Connection to the server lost.", source, StringComparison.Ordinal);

         // The timer runs only while the statistics can be read.
         string enter = WithheldRowTests.MemberText(source, "public void OnEnter()");
         Assert.Matches(new Regex(@"if \(Refresh\(\)\)\s*timer_\.Start\(\);"), enter);
      }

      /// <summary>
      /// A link lost while the Dashboard's setup summary ran is not counted into
      /// "things need attention": the card says it cannot tell. Values a server that
      /// answered could not give are a Warning notice, as the setup checklist says
      /// them, where they were a grey caption with no mark or word.
      /// </summary>
      [Fact]
      public void TheDashboardsSetupCard_CannotTellOverALostLink_AndWarnsOfUnreadValues()
      {
         string source = ReadView("DashboardView.xaml.cs");
         string begin = WithheldRowTests.MemberText(source, "private void BeginSetupCheck_()");
         Assert.Matches(new Regex(@"if \(checks\.LinkLost\)\s*\{\s*ShowSetupUncheckable_\("), begin);

         string render = WithheldRowTests.MemberText(source, "private void RenderAttention_(ExternalSetupChecks checks, int pendingDnsLookups)");
         Assert.Contains("AttentionReadNotice.Show(StatusLevel.Warning", render, StringComparison.Ordinal);
         int note = render.IndexOf("AttentionNote.Text = note;", StringComparison.Ordinal);
         Assert.True(note >= 0 && render.IndexOf("could not be read", StringComparison.Ordinal) > note,
            "The Dashboard's unread values are still in its grey caption.");

         XElement page = XDocument.Parse(ReadView("DashboardView.xaml")).Root;
         XElement notice = page.Descendants(Scaffold + "InlineNotice").SingleOrDefault(e => (string)e.Attribute(X + "Name") == "AttentionReadNotice");
         Assert.True(notice != null, "DashboardView.xaml has no InlineNotice AttentionReadNotice.");
      }

      /// <summary>
      /// The setup checklist and DNS records take the level of their read-failure
      /// notice from the cause, where both said a lost link as a Warning.
      /// </summary>
      [Theory]
      [InlineData("ExternalSetupView.cs", "readFailure_.Level = ComErrorText.LevelFor(checks_.LinkLost);")]
      [InlineData("DnsRecordsView.cs", "readFailure_.Level = ComErrorText.LevelFor(linkLost_);")]
      public void TheChecklistAndDnsRecords_SayALostLinkAtCritical(string file, string line)
      {
         Assert.Contains(line, ReadView(file), StringComparison.Ordinal);
      }

      // ---- the Dashboard's figures while they cannot be read ---------------------

      /// <summary>
      /// Every KPI starts as a dash, not a 0: "In queue 0" is a statement the page
      /// cannot make before it has asked, and the XAML's 0s were what the cards
      /// showed under a lost link.
      /// </summary>
      [Theory]
      [InlineData("KpiUptime")]
      [InlineData("KpiProcessed")]
      [InlineData("KpiQueue")]
      [InlineData("KpiSpam")]
      [InlineData("KpiViruses")]
      public void EveryKpi_StartsAsADash(string name)
      {
         XElement page = XDocument.Parse(ReadView("DashboardView.xaml")).Root;
         XElement value = page.Descendants().Single(e => (string)e.Attribute(X + "Name") == name);
         Assert.Equal("—", (string)value.Attribute("Text"));
      }

      /// <summary>
      /// A failed read of the statistics shows a dash in every card when nothing
      /// was ever read, and otherwise keeps the last figures, dimmed, named with the
      /// time they were read, under a subtitle that says when that was.
      /// </summary>
      [Fact]
      public void TheKpis_SayTheyWereNotRead_OrWhenTheyWere()
      {
         string source = ReadView("DashboardView.xaml.cs");
         string refresh = WithheldRowTests.MemberText(source, "private bool Refresh()");
         int catchAt = refresh.IndexOf("catch (Exception", StringComparison.Ordinal);
         Assert.True(catchAt > 0 && refresh.IndexOf("ShowKpisUnread_();", catchAt, StringComparison.Ordinal) > catchAt,
            "The Dashboard's failed read leaves the KPI cards as they were.");

         string unread = WithheldRowTests.MemberText(source, "private void ShowKpisUnread_()");
         Assert.True(unread != null, "DashboardView.xaml.cs no longer has ShowKpisUnread_.");
         foreach (string kpi in new[] { "KpiUptime", "KpiProcessed", "KpiQueue", "KpiSpam", "KpiViruses" })
            Assert.Contains("(" + kpi + ", L(", unread, StringComparison.Ordinal);
         Assert.Contains("if (lastRead_ == null)", unread, StringComparison.Ordinal);
         Assert.Contains("value.Text = NotRead;", unread, StringComparison.Ordinal);
         Assert.Contains("F(\"{0}, not available\", label)", unread, StringComparison.Ordinal);
         Assert.Contains("KpiQueueBadge.Visibility = Visibility.Hidden;", unread, StringComparison.Ordinal);
         Assert.Contains("\"TextFillColorSecondaryBrush\"", unread, StringComparison.Ordinal);
         Assert.Contains("Header.Subtitle = F(\"Figures from {0}, the last time the server could be read.\", time);", unread, StringComparison.Ordinal);
         Assert.Contains("private const string NotRead = \"—\";", source, StringComparison.Ordinal);
      }

      // ---- every catch through DescribeComError ----------------------------------

      /// <summary>
      /// The pages' catches whose try is local and whose exception's own text is
      /// the reason: a file, the registry, a browser, an HTTPS fetch or an LDAP
      /// probe from this machine, the sign-in page's own reporting. Each by a
      /// phrase of its line, so that a new line showing an exception's raw text
      /// fails the test below until it is routed or added here with its reason.
      /// </summary>
      private static readonly (string File, string Phrase, string Why)[] LocalCatches =
      {
         ("AccountDialog.cs", "Could not read the photo", "a local image file"),
         ("ConnectView.xaml.cs", "Fail_(ex.Message)", "the sign-in page's own reporting"),
         ("DirectorySyncView.cs", "L(\"Could not save: \") + ex.Message", "hMailServer.ini on this machine"),
         ("DnsRecordsView.cs", "ex.InnerException != null ? ex.InnerException.Message", "an HTTPS fetch from this machine"),
         ("DnsRecordsView.cs", "The fetch itself failed", "an HTTPS fetch from this machine"),
         ("DomainDialog.cs", "Could not generate the DKIM key", "a key made and written on this machine"),
         ("DomainPolicyListView.cs", "The server's answer could not be read", "already a sentence about an unreadable answer"),
         ("LdapSettingsView.cs", "MessageBox.Show(F(\"Could not save: {0}\", ex.Message)", "hMailServer.ini on this machine"),
         ("LdapSettingsView.cs", "The test itself failed unexpectedly", "an LDAP probe from this machine"),
         ("MessageViewerDialog.cs", "Could not read the message file", "a local message file"),
         ("ScriptsView.cs", "F(\"Could not read the file: {0}\", ex.Message)", "a local script file"),
         ("ScriptsView.cs", "Could not write the script file", "a local script file"),
         ("ServerSettingsView.xaml.cs", "Could not open the folder", "Explorer on this machine"),
         ("StalledMailView.cs", "Could not open a browser", "a browser on this machine"),
         ("TotpSetupDialog.cs", "notice_.Show(StatusLevel.Critical, ex.Message);", "the Control Panel's own second factor, in the registry"),
         ("UtilityViews.cs", "Could not read the consistency report", "a local report file"),
         ("UtilityViews.cs", "Could not read {0}: {1}", "a local report file"),
         ("UtilityViews.cs", "Could not open {0}: {1}", "Explorer on this machine"),
      };

      /// <summary>
      /// No page shows a failed call to the server as the exception's raw text:
      /// "The RPC server is unavailable. (Exception from HRESULT: 0x800706BA)" at the
      /// foot of Server status, "Could not save: ..." with a binder's or a parser's
      /// words in a message box. Every catch of a server call goes through
      /// ServerSession.DescribeComError, which says a lost link, a database the
      /// server cannot reach, an answer that could not be read and a member an
      /// older server does not have in words, and notices the lost link.
      /// </summary>
      [Fact]
      public void NoPage_ShowsAServerFailureAsTheExceptionsRawText()
      {
         var raw = new Regex(@"\b(ex|fatalCheck)\.Message\b");
         string views = Path.GetDirectoryName(ControlPanelPath(Path.Join("Views", "DashboardView.xaml.cs")));
         var unexplained = new System.Collections.Generic.List<string>();
         var used = new System.Collections.Generic.HashSet<int>();

         foreach (string path in Directory.GetFiles(views, "*.cs"))
         {
            string name = Path.GetFileName(path);
            string[] lines = File.ReadAllLines(path);
            for (int i = 0; i < lines.Length; i++)
            {
               if (!raw.IsMatch(lines[i]) || lines[i].Contains("DescribeComError", StringComparison.Ordinal))
                  continue;

               int allowed = Array.FindIndex(LocalCatches, c => c.File == name && lines[i].Contains(c.Phrase, StringComparison.Ordinal));
               if (allowed < 0)
                  unexplained.Add(name + ":" + (i + 1) + ": " + lines[i].Trim());
               else
                  used.Add(allowed);
            }
         }

         Assert.True(unexplained.Count == 0,
            "These lines show a failure's raw text; route them through ServerSession.DescribeComError, or name them in LocalCatches with the reason:\n"
            + string.Join("\n", unexplained));

         // An entry nothing matches any more is a reason kept for a line that has gone.
         var stale = LocalCatches.Where((c, i) => !used.Contains(i)).Select(c => c.File + ": " + c.Phrase).ToList();
         Assert.True(stale.Count == 0, "LocalCatches names lines that are gone:\n" + string.Join("\n", stale));
      }

      /// <summary>
      /// DescribeComError says a failure that is not the server's words as what it
      /// is (ComErrorText), the shared failure notice keeps the runtime's code in its
      /// tool tip, and a statistic the server could not give is a Warning notice
      /// rather than body text standing in the figure's place.
      /// </summary>
      [Fact]
      public void DescribeComError_SaysWhatAFailureThatIsNotTheServersWordsIs()
      {
         string session = ReadControlPanelFile(Path.Join("Services", "ServerSession.cs"));
         string describe = WithheldRowTests.MemberText(session, "public static string DescribeComError(Exception ex)");
         Assert.Contains("return ComErrorText.NotTheServersWords(ex, msg) ?? msg;", describe, StringComparison.Ordinal);

         string failure = ReadControlPanelFile(Path.Join("Services", "ReadFailure.cs"));
         Assert.Contains("notice.ToolTip = ComErrorText.TechnicalDetail(why);", failure, StringComparison.Ordinal);

         string settings = ReadView("ServerSettingsView.xaml.cs");
         int stat = settings.IndexOf("private class ComStat", StringComparison.Ordinal);
         string build = WithheldRowTests.MemberText(settings.Substring(stat), "protected override FrameworkElement BuildEditor(object value)");
         Assert.Matches(new Regex(@"catch \(Exception ex\)[^{]*\{[^}]*return new Scaffold\.InlineNotice\s*\{\s*Level = StatusLevel\.Warning", RegexOptions.Singleline), build);
      }

      /// <summary>
      /// DescribeComError's sentence is written to follow a prefix that says what
      /// failed - "the connection to the hMailServer service was lost ...", "the
      /// server answered something this Control Panel could not read (...)" - so a
      /// place that shows it says what failed first. Six places that had shown the
      /// runtime's text bare showed the sentence bare once routed, opening in lower
      /// case and naming nothing: a message box, a dialog's notice, the Welcome
      /// page's warning, and "The mailbox could not be moved. the connection ...".
      /// The Levels tab's save, whose failure the tab now says in a Critical notice,
      /// is held to the same.
      /// </summary>
      [Theory]
      [InlineData("AdministratorTwoFactorDialog.cs", "F(\"Could not start the enrolment: {0}\", ServerSession.DescribeComError(ex))")]
      [InlineData("AdministratorTwoFactorDialog.cs", "F(\"Could not change the second factor: {0}\", ServerSession.DescribeComError(ex))")]
      [InlineData("IPRangesView.xaml.cs", "F(\"Could not restore the default IP ranges: {0}\", ServerSession.DescribeComError(ex))")]
      [InlineData("TcpIpPortsView.xaml.cs", "F(\"Could not restore the default TCP/IP ports: {0}\", ServerSession.DescribeComError(ex))")]
      [InlineData("MailboxMoveDialog.cs", "F(\"The mailbox could not be moved: {0}\", ServerSession.DescribeComError(fatalCheck))")]
      [InlineData("WelcomeView.cs", "F(\"Could not make the sample domain: {0}\", failure)")]
      [InlineData("WelcomeView.cs", "F(\"Could not remove the sample domain: {0}\", failure)")]
      [InlineData("SettingLevelsPanel.cs", "F(\"Could not save the setting levels: {0}\", ServerSession.DescribeComError(ex))")]
      public void ARoutedFailure_SaysWhatFailedBeforeItsReason(string file, string call)
      {
         Assert.Contains(call, ReadView(file), StringComparison.Ordinal);
      }

      /// <summary>
      /// No message box or dialog notice shows DescribeComError's sentence as the
      /// whole of what it says.
      /// </summary>
      [Fact]
      public void NoMessageBoxOrDialogNotice_ShowsTheReasonAlone()
      {
         var bare = new Regex(@"(MessageBox\.Show|Dialogs\.Warn|notice_\.Show\(StatusLevel\.\w+,)\s*\(?\s*ServerSession\.DescribeComError\(");
         string views = Path.GetDirectoryName(ControlPanelPath(Path.Join("Views", "DashboardView.xaml.cs")));
         var found = Directory.GetFiles(views, "*.cs")
            .SelectMany(path => File.ReadAllLines(path).Select((line, i) => (Path.GetFileName(path), i + 1, line)))
            .Where(t => bare.IsMatch(t.line))
            .Select(t => t.Item1 + ":" + t.Item2 + ": " + t.line.Trim())
            .ToList();
         Assert.True(found.Count == 0, "These show a failure's reason with nothing to say what failed:\n" + string.Join("\n", found));
         Assert.DoesNotContain("Dialogs.Warn(failure, ", ReadView("WelcomeView.cs"), StringComparison.Ordinal);
      }

      /// <summary>
      /// A failure's reason, as DescribeComError puts it, is the last thing a
      /// sentence says. Its sentences are written to end a message, and the ones for
      /// an older server and an unreadable answer end in the runtime's text in
      /// brackets with no full stop, so a sentence written after the reason ran on
      /// from it: "... older than this Control Panel (Unknown name.) The page shows
      /// the values in hMailServer.ini ..." on the hMailServer.ini pages, and the same
      /// on the notices of DNS records and the spam, virus and transport overviews
      /// and before the settings pages' sentence about the server's environment.
      /// Held for every template the Control Panel formats a reason into, directly or
      /// from a field or variable the file gives a reason to.
      /// </summary>
      [Fact]
      public void NoSentence_FollowsAFailuresReason()
      {
         string root = Path.GetDirectoryName(Path.GetDirectoryName(ControlPanelPath(Path.Join("Views", "DashboardView.xaml.cs"))));
         var holders = new Regex(@"\b(\w+)\s*\??\?=\s*(?:ServerSession\.)?DescribeComError\(");
         var formats = new Regex(@"\bF\(\s*""((?:[^""\\]|\\.)*)""\s*,");
         var offenders = new System.Collections.Generic.List<string>();

         foreach (string path in Directory.GetFiles(root, "*.cs", SearchOption.AllDirectories))
         {
            string relative = Path.GetRelativePath(root, path);
            if (relative.StartsWith("bin", StringComparison.Ordinal) || relative.StartsWith("obj", StringComparison.Ordinal))
               continue;

            string source = File.ReadAllText(path);
            var reasons = holders.Matches(source).Select(m => m.Groups[1].Value).ToHashSet(StringComparer.Ordinal);

            foreach (Match format in formats.Matches(source))
            {
               string template = format.Groups[1].Value;
               var arguments = ArgumentsFrom(source, format.Index + format.Length);
               for (int i = 0; i < arguments.Count; i++)
               {
                  string argument = Regex.Replace(arguments[i], @"\s*\?\?\s*""""\s*$", "");
                  if (!argument.Contains("DescribeComError(", StringComparison.Ordinal) && !reasons.Contains(argument))
                     continue;

                  string placeholder = "{" + i.ToString(System.Globalization.CultureInfo.InvariantCulture) + "}";
                  int at = template.IndexOf(placeholder, StringComparison.Ordinal);
                  if (at >= 0 && template.Substring(at + placeholder.Length).Trim().Length > 0)
                     offenders.Add(relative + ": \"" + template + "\" puts text after " + argument);
               }
            }
         }

         Assert.True(offenders.Count == 0, "A sentence follows a failure's reason, which may end without a full stop:\n" + string.Join("\n", offenders));

         // The settings pages put the environment's sentence before the read's, whose
         // reason is last.
         string build = WithheldRowTests.MemberText(ReadView("ServerSettingsView.xaml.cs"), "private void BuildUi()");
         Assert.Contains("environmentSentence + \" \" + readSentence", build, StringComparison.Ordinal);
         Assert.DoesNotContain("readSentence + \" \"", build, StringComparison.Ordinal);
      }

      /// <summary>
      /// The arguments of a call, from just after its first argument's comma to its
      /// closing bracket, split at the commas outside brackets and literals.
      /// </summary>
      private static System.Collections.Generic.List<string> ArgumentsFrom(string source, int start)
      {
         var arguments = new System.Collections.Generic.List<string>();
         var current = new System.Text.StringBuilder();
         int depth = 0;
         for (int i = start; i < source.Length; i++)
         {
            char c = source[i];
            if (c == '"' || c == '\'')
            {
               int end = i + 1;
               while (end < source.Length && source[end] != c)
                  end += source[end] == '\\' ? 2 : 1;
               current.Append(source, i, Math.Min(end + 1, source.Length) - i);
               i = end;
               continue;
            }

            if (c == '(' || c == '[' || c == '{')
               depth++;
            else if (c == ')' || c == ']' || c == '}')
            {
               if (depth == 0)
                  break;
               depth--;
            }
            else if (c == ',' && depth == 0)
            {
               arguments.Add(current.ToString().Trim());
               current.Clear();
               continue;
            }

            current.Append(c);
         }

         arguments.Add(current.ToString().Trim());
         return arguments;
      }

      // ---- a card that lost settings says so -------------------------------------

      /// <summary>
      /// A setting whose value could not be read keeps its caption on its card with
      /// "Not read" in place of its editor, by each road - a COM read that threw, a
      /// REST group that does not answer its key, a row kept in hMailServer.ini whose
      /// read fell back to the file - and the card says at its top how many and that
      /// saving leaves them as they are. Before, the row was skipped: a card that
      /// lost every row was a bare heading, and nothing said which card had lost
      /// what.
      /// </summary>
      [Fact]
      public void ACardThatLostSettings_MarksEachAndSaysHowMany()
      {
         string source = ReadView("ServerSettingsView.xaml.cs");
         string build = WithheldRowTests.MemberText(source, "private void BuildUi()");
         Assert.True(build != null, "ServerSettingsView.xaml.cs no longer has BuildUi.");

         // Every road to an unread row counts it and marks it before it goes on; the
         // COM read and the fall-back also record whether the link went.
         Assert.Equal(3, Regex.Matches(build, @"failedReads_\+\+;\s*diag_ \?\?= .+?;\s*(?:linkLost_ \|= ServerSession\.IsTransportFailure\(\w+\);\s*)?unread\+\+;\s*fields\.Children\.Add\(lastRow = UnreadRow_\(setting\)\);\s*continue;").Count);

         // The card's own notice, one sentence per number.
         Assert.Contains("else if (unread > 0)", build, StringComparison.Ordinal);
         Assert.Contains("L(\"One setting on this card could not be read, so it is marked Not read; saving leaves it as it is on the server.\")", build, StringComparison.Ordinal);
         Assert.Contains("F(\"{0} settings on this card could not be read, so they are marked Not read; saving leaves them as they are on the server.\", unread)", build, StringComparison.Ordinal);

         // The row: the caption with no access key, the note, and the neutral pill.
         string row = WithheldRowTests.MemberText(source, "private static Scaffold.FieldRow UnreadRow_(ComSetting setting)");
         Assert.True(row != null, "ServerSettingsView.xaml.cs no longer has UnreadRow_.");
         Assert.Contains("Level = StatusLevel.Normal", row, StringComparison.Ordinal);
         Assert.Contains("Text = L(\"Not read\")", row, StringComparison.Ordinal);
         Assert.Contains("MnemonicText.Escape(MnemonicText.Strip(", row, StringComparison.Ordinal);
         Assert.Contains("Hint = setting.Blurb", row, StringComparison.Ordinal);
      }

      /// <summary>
      /// A row kept in hMailServer.ini whose read fell back to the file on this
      /// computer - the link gone, or a server without GetIniSetting - is a setting
      /// the server did not give, and takes the road a COM read that threw takes:
      /// counted, with the link's loss recorded, marked Not read, and left out of
      /// what a save writes. Before, it fell back without a word: under a lost link
      /// the delivery page's PIPELINING and CHUNKING boxes stood ticked and every
      /// idle timeout on Protocols was a live, editable number, on the same cards as
      /// rows marked Not read, and the page's count of unread settings left them out.
      /// </summary>
      [Fact]
      public void AnIniRowThatFellBackToTheFile_IsNotRead()
      {
         string source = ReadView("ServerSettingsView.xaml.cs");
         string build = WithheldRowTests.MemberText(source, "private void BuildUi()");
         Assert.True(build != null, "ServerSettingsView.xaml.cs no longer has BuildUi.");

         // Each row's fall-back is its own: forgotten before its editor reads, asked
         // after.
         Match road = Regex.Match(build,
            @"bool iniRow = setting is IIniSetting;\s*if \(iniRow\)\s*iniStore_\.ForgetFallback\(\);\s*setting\.Changed = MarkUnsaved_;\s*lastRow = setting\.CreateRow\(value\);\s*if \(iniRow && iniStore_\.FallbackCause is Exception (\w+)\)\s*\{(.*?)\}\s*built_\.Add\(setting\);",
            RegexOptions.Singleline);
         Assert.True(road.Success, "BuildUi does not ask each hMailServer.ini row whether its read fell back to the file.");

         string cause = road.Groups[1].Value;
         string unread = road.Groups[2].Value;
         Assert.Contains("failedReads_++;", unread, StringComparison.Ordinal);
         Assert.Contains("diag_ ??= ServerSession.DescribeComError(" + cause + ");", unread, StringComparison.Ordinal);
         Assert.Contains("linkLost_ |= ServerSession.IsTransportFailure(" + cause + ");", unread, StringComparison.Ordinal);
         Assert.Contains("fields.Children.Add(lastRow = UnreadRow_(setting));", unread, StringComparison.Ordinal);
         Assert.Contains("continue;", unread, StringComparison.Ordinal);
         Assert.DoesNotContain("built_.Add", unread, StringComparison.Ordinal);
      }

      /// <summary>
      /// One event, one level, on the pages ServerSettingsView builds. A server that
      /// answered but could not give some settings is a Warning at the top of the
      /// page and on each card that lost them; a lost link is said once, at the top,
      /// at Critical, and no card repeats it. Before, the page said the unread
      /// settings at Critical whatever the cause, over a Warning on each card below
      /// it about the very same settings - on the anti-spam page a Warning on every
      /// card of six tabs under the lost link's Critical.
      /// </summary>
      [Fact]
      public void TheSettingsPages_SayUnreadSettingsAtTheLevelOfTheirCause()
      {
         string source = ReadView("ServerSettingsView.xaml.cs");
         string build = WithheldRowTests.MemberText(source, "private void BuildUi()");
         Assert.True(build != null, "ServerSettingsView.xaml.cs no longer has BuildUi.");

         // Counted afresh each build.
         Assert.Matches(new Regex(@"failedReads_ = 0;\s*linkLost_ = false;"), build);

         // The page's sentence at the cause's level, never at a fixed Critical.
         Assert.Contains("ShowNotice_(string.IsNullOrEmpty(environmentSentence) ? readSentence : environmentSentence + \" \" + readSentence, ComErrorText.LevelFor(linkLost_));", build, StringComparison.Ordinal);
         Assert.DoesNotContain("readSentence, StatusLevel.", build, StringComparison.Ordinal);

         // The cards' counts are said after every card has been read, and only when
         // the link held: a later card's read may be the first to find it gone.
         int lastTab = build.IndexOf("SettingsTabs.Items.Add(", StringComparison.Ordinal);
         int guard = build.IndexOf("if (!linkLost_)", StringComparison.Ordinal);
         int cardSentence = build.IndexOf("L(\"One setting on this card could not be read", StringComparison.Ordinal);
         Assert.True(lastTab >= 0 && lastTab < guard && guard < cardSentence,
            "A card says how many of its settings could not be read before the page knows whether the link went, or whatever the cause.");
         Assert.Contains("unreadCards.Add((fields, unread));", build, StringComparison.Ordinal);

         // A figure that could not be read over a lost link is marked Not read, as the
         // settings are, rather than given a Warning of its own about the same event.
         string stat = source.Substring(source.IndexOf("private class ComStat", StringComparison.Ordinal));
         string statBuild = WithheldRowTests.MemberText(stat, "protected override FrameworkElement BuildEditor(object value)");
         Assert.Matches(new Regex(@"catch \(Exception ex\)[^{]*\{\s*(?://[^\n]*\n\s*)*if \(ComErrorText\.IsTransportFailure\(ex\)\)\s*return NotReadFigure_\(ex\);"), statBuild);
         string figure = WithheldRowTests.MemberText(stat, "private static FrameworkElement NotReadFigure_(Exception ex)");
         Assert.True(figure != null, "ComStat no longer has NotReadFigure_.");
         Assert.Contains("Level = StatusLevel.Normal", figure, StringComparison.Ordinal);
         Assert.Contains("Text = L(\"Not read\")", figure, StringComparison.Ordinal);
      }

      /// <summary>
      /// The page's sentence says where the unread settings are and what a save does
      /// with them, one sentence per number, the reason last - where it was "29
      /// setting(s) could not be read" with nothing to say which card or that Save
      /// leaves them alone.
      /// </summary>
      [Fact]
      public void ThePagesSentence_SaysWhereTheUnreadSettingsAreAndWhatSaveDoes()
      {
         string one = hMailServer.ControlPanel.Services.ServerSettingsPageStatus.ReadSentence(1, "the reason");
         Assert.StartsWith("One setting could not be read", one, StringComparison.Ordinal);
         Assert.Contains("marked Not read on its card", one, StringComparison.Ordinal);
         Assert.EndsWith("Why: the reason", one, StringComparison.Ordinal);

         string many = hMailServer.ControlPanel.Services.ServerSettingsPageStatus.ReadSentence(29, "the reason");
         Assert.StartsWith("29 settings could not be read", many, StringComparison.Ordinal);
         Assert.Contains("saving leaves them as they are on the server", many, StringComparison.Ordinal);
         Assert.EndsWith("The first reason: the reason", many, StringComparison.Ordinal);
         Assert.DoesNotContain("(s)", one + many, StringComparison.Ordinal);
      }

      // ---- REST-backed cards give the real reason once per tab --------------------

      /// <summary>
      /// A session that holds no credential the REST API takes is told so, and not
      /// "Sign in to a server first." - which was said under a Connected pill on
      /// every card that held a REST setting.
      /// </summary>
      [Fact]
      public void ASessionWithNoCredential_IsNotToldToSignIn()
      {
         var client = new hMailServer.ControlPanel.Services.RestSettingsClient(() => null,
            () => throw new InvalidOperationException("No listener is asked for before a credential."));

         Assert.False(client.Open(null, false, out string refusal));
         Assert.NotEqual("Sign in to a server first.", refusal);
         Assert.Contains("no password the REST API can sign in with", refusal, StringComparison.Ordinal);
      }

      /// <summary>
      /// When the REST API cannot be used at all, the reason is said once per tab,
      /// under the strip, with the API & monitoring page one press away, and each
      /// REST row is marked Not read on its card; a refusal of one group alone stays
      /// on that group's card.
      /// </summary>
      [Fact]
      public void ARestApiThatCannotBeUsed_IsSaidOncePerTab()
      {
         string source = ReadView("ServerSettingsView.xaml.cs");

         string load = WithheldRowTests.MemberText(source, "private void LoadRestGroups_()");
         Assert.Contains("new RestGroup { Refusal = refusal, SessionWide = true }", load, StringComparison.Ordinal);
         Assert.Contains("SessionWide = reply.Status == 0 || !string.IsNullOrEmpty(reply.Refusal)", load, StringComparison.Ordinal);

         string build = WithheldRowTests.MemberText(source, "private void BuildUi()");
         Assert.Matches(new Regex(@"if \(group != null && group\.SessionWide\)\s*\{\s*tabRestProblem \?\?= group\.Refusal;\s*if \(setting\.WantsInitialValue\)\s*fields\.Children\.Add\(lastRow = UnreadRow_\(setting\)\);"), build);
         Assert.Contains("panel.Children.Insert(0, RestTabNotice_(tabRestProblem));", build, StringComparison.Ordinal);

         // Declared per tab, so that each tab says it once and no tab says another's.
         int tabLoop = build.IndexOf("foreach (TabDef tab in tabs_)", StringComparison.Ordinal);
         int declared = build.IndexOf("string tabRestProblem = null;", StringComparison.Ordinal);
         int cardLoop = build.IndexOf("foreach (CardDef card in tab.Cards)", StringComparison.Ordinal);
         Assert.True(tabLoop >= 0 && tabLoop < declared && declared < cardLoop, "The REST reason is not kept per tab.");

         string notice = WithheldRowTests.MemberText(source, "private static Scaffold.InlineNotice RestTabNotice_(string reason)");
         Assert.Contains("L(\"These settings are read through the REST API, which this session cannot use.\") + \" \" + reason", notice, StringComparison.Ordinal);
         Assert.Contains("L(\"_Open API & monitoring\")", notice, StringComparison.Ordinal);
         Assert.Contains("NavigateTo(\"api\")", notice, StringComparison.Ordinal);
      }

      // ---- the Levels tab: notices instead of bare text ---------------------------

      /// <summary>
      /// The domain and account dialogs' Levels tab says what it has to say in a
      /// notice at its level, where one bold text block carried everything alike: a
      /// failed read at Critical with Try again, the older server and the
      /// save-it-first notes as Information, a save as Good or the refusals as
      /// Critical.
      /// </summary>
      [Fact]
      public void TheLevelsTab_SaysEachThingAtItsLevel()
      {
         string source = ReadView("SettingLevelsPanel.cs");
         Assert.Contains("private readonly Scaffold.InlineNotice status_ = DialogFields.Notice();", source, StringComparison.Ordinal);
         Assert.DoesNotContain("status_.Text =", source, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(source, "public void Reload()");
         Assert.Contains("status_.Show(StatusLevel.Information, L(\"Save it and reopen it: a level's own value belongs to something that exists.\"));", reload, StringComparison.Ordinal);
         Assert.Contains("status_.Show(StatusLevel.Information, L(\"This server has no setting levels:", reload, StringComparison.Ordinal);
         Assert.Contains("ReadFailure.Show(status_, null, null, L(\"Could not read the setting levels: \"), ex, Reload, save_);", reload, StringComparison.Ordinal);
         Assert.Contains("ReadFailure.Clear(status_, save_);", reload, StringComparison.Ordinal);

         // Try again keeps its button across failures: the failure is not cleared
         // before the read, only after one that worked.
         int read = reload.IndexOf("owner = OpenOwner();", StringComparison.Ordinal);
         Assert.True(reload.IndexOf("ReadFailure.Clear(status_, save_);", StringComparison.Ordinal) > read,
            "The Levels tab clears its failure before it reads again, which replaces Try again and loses the keyboard.");

         string save = WithheldRowTests.MemberText(source, "private void SaveLevels()");
         Assert.Contains("status_.Show(StatusLevel.Good, L(\"Saved. What is in effect is shown beside each setting.\"));", save, StringComparison.Ordinal);
         Assert.Contains("status_.Show(StatusLevel.Critical, string.Join(Environment.NewLine, refused));", save, StringComparison.Ordinal);

         // A read that fails after the save keeps its notice, its Try again and its
         // sentence, and the save's outcome goes before it - the refusals, or that the
         // levels were saved - where the read's failure dropped them: under a lost
         // link nothing said that the save had not happened.
         Match afterRead = Regex.Match(save, @"Reload\(\);(.*?)if \(ReadFailure\.IsShown\(status_\)\)\s*\{(.*?)\}", RegexOptions.Singleline);
         Assert.True(afterRead.Success, "SaveLevels does not read the levels again after a save, or says nothing when that read fails.");
         string kept = afterRead.Groups[2].Value;
         Assert.Contains("refused.Count == 0", kept, StringComparison.Ordinal);
         Assert.Contains("L(\"The setting levels were saved.\")", kept, StringComparison.Ordinal);
         Assert.Contains("string.Join(Environment.NewLine, refused)", kept, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"status_\.Show\(StatusLevel\.Critical, \w+ \+ Environment\.NewLine \+ status_\.Text\);\s*return;\s*$"), kept);
         Assert.DoesNotContain("ReadFailure.Clear", kept, StringComparison.Ordinal);
      }

      /// <summary>
      /// Every COM read the checks and the settings pages count as failed also
      /// records whether the link went. (A document that could not be parsed is
      /// caught by type, and is never a lost link.)
      /// </summary>
      [Theory]
      [InlineData("Services/ExternalSetupChecks.cs")]
      [InlineData("Views/DnsRecordsView.cs")]
      [InlineData("Views/ServerSettingsView.xaml.cs")]
      public void EveryCountedComFailure_RecordsWhetherTheLinkWent(string file)
      {
         string source = ReadControlPanelFile(file);
         const string counts = @"catch \(Exception ex\) when \(!ExceptionPolicy\.IsFatal\(ex\)\)\s*\{\s*failedReads_\+\+;\s*(?:firstError_|diag_) \?\?= ServerSession\.DescribeComError\(ex\);";
         int counted = Regex.Matches(source, counts).Count;
         int recorded = Regex.Matches(source, counts + @"\s*linkLost_ \|= ServerSession\.IsTransportFailure\(ex\);").Count;
         Assert.True(counted > 0, file + " counts no failed COM read any more.");
         Assert.Equal(counted, recorded);
      }

      // ---- helpers ---------------------------------------------------------------

      private static string ReadView(string file) => ReadControlPanelFile(Path.Join("Views", file));

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelPath(relative));

      /// <summary>A file under the ControlPanel project, found from the test binaries.</summary>
      private static string ControlPanelPath(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
