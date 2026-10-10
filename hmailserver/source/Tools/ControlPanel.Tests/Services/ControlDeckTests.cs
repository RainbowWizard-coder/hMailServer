// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Control Deck's button (review finding F281). Twenty-nine texts sent the
   /// reader to the Deck - a case is opened there, alert rules and subscriptions are
   /// set there, trap addresses declared and held messages reviewed there - with no
   /// button and no address, and called it the Web Control Deck and the Deck as
   /// well. The button opens the REST listener's root, where the Deck is served, and
   /// is disabled with the reason while the REST API cannot serve it here.
   /// </summary>
   public class ControlDeckTests
   {
      [Fact]
      public void TheDeck_IsTheListenersRoot()
      {
         Uri deck = ControlDeck.Address("8045", "127.0.0.1", "", "", null, localSession: true, host: "mail.example.com", out string whyNot);
         Assert.Equal(new Uri("http://127.0.0.1:8045/"), deck);
         Assert.Null(whyNot);

         // With a certificate on every address, from this machine and from another.
         Assert.Equal(new Uri("https://localhost:8045/"), ControlDeck.Address("8045", "0.0.0.0", "c.pem", "k.pem", null, true, "mail.example.com", out _));
         Assert.Equal(new Uri("https://mail.example.com:8045/"), ControlDeck.Address("8045", "0.0.0.0", "c.pem", "k.pem", null, false, "mail.example.com", out _));
      }

      [Fact]
      public void TheButton_SaysWhyItCannotOpenTheDeck()
      {
         Assert.Null(ControlDeck.Address("0", "127.0.0.1", "", "", null, true, null, out string off));
         Assert.Equal("The Control Deck is served by the REST API, which is off - give it a port on the API & monitoring page.", off);
         Assert.Null(ControlDeck.Address("", null, null, null, null, true, null, out string empty));
         Assert.Equal(off, empty);

         Assert.Null(ControlDeck.Address("8045", "127.0.0.1", "", "", null, false, "mail.example.com", out string remote));
         Assert.Equal("The REST API listens on 127.0.0.1 only, so the Control Deck opens only in a browser on the server itself.", remote);

         Assert.Null(ControlDeck.Address("8045", "0.0.0.0", "", "", false, true, null, out string uncertified));
         Assert.Equal("The REST API is not running: listening on 0.0.0.0 needs a certificate, set on the API & monitoring page.", uncertified);
      }

      /// <summary>
      /// The listener runs on what the server last applied, and hMailServer.ini holds
      /// what was saved: the REST API turned on and saved listens nowhere until the
      /// server applies it, and a port changed and saved still listens on the old one
      /// (IniSettingsBaseline.h marks the four AppliesReinitialize). While one of them
      /// waits, the button is disabled and says so, where it opened an address nothing
      /// listened on; a port saved as 0 is the REST API turned off, said as it is.
      /// </summary>
      [Fact]
      public void TheButton_WaitsForTheListenersSettingsToApply()
      {
         Assert.Equal(new[] { "RestApiPort", "RestApiBindAddress", "RestApiCertificateFile", "RestApiPrivateKeyFile" }, ControlDeck.ListenerSettings);
         const string waiting = "The REST API's new settings are saved but not yet in effect - the Control Deck can be opened once the server applies them.";

         foreach (string name in ControlDeck.ListenerSettings)
         {
            Assert.Null(ControlDeck.Address("8045", "127.0.0.1", "", "", null, true, null, new[] { name.ToLowerInvariant() }, null, out string whyNot));
            Assert.Equal(waiting, whyNot);
         }

         // Nothing of the listener's waiting - another setting, an empty list, or a
         // server that cannot say - and the Deck is where the file says.
         Assert.Equal(new Uri("http://127.0.0.1:8045/"), ControlDeck.Address("8045", "127.0.0.1", "", "", null, true, null, new[] { "MaxDeliveryThreads" }, null, out _));
         Assert.Equal(new Uri("http://127.0.0.1:8045/"), ControlDeck.Address("8045", "127.0.0.1", "", "", null, true, null, Array.Empty<string>(), null, out _));
         Assert.Equal(new Uri("http://127.0.0.1:8045/"), ControlDeck.Address("8045", "127.0.0.1", "", "", null, true, null, null, null, out _));

         // Turned off and saved: off, whatever is still listening.
         Assert.Null(ControlDeck.Address("0", "127.0.0.1", "", "", null, true, null, new[] { "RestApiPort" }, null, out string turnedOff));
         Assert.Equal("The Control Deck is served by the REST API, which is off - give it a port on the API & monitoring page.", turnedOff);

         // The button asks the server for what waits.
         string link = ReadControlPanelFile(Path.Join("Views", "ControlDeckLink.cs"));
         Assert.Contains("IniFeatureStore.PendingNames(), whyOffHere, out whyNot);", link, StringComparison.Ordinal);
      }

      /// <summary>
      /// On the REST API's own card the Port field is two rows below the button, and
      /// the button's reason says so, where it sent the reader to the API &amp;
      /// monitoring page they were on; every other page names that page.
      /// </summary>
      [Fact]
      public void OnThePortsOwnPage_TheReasonPointsBelow()
      {
         const string below = "The Control Deck is served by the REST API, which is off - give it a port below.";
         Assert.Null(ControlDeck.Address("0", "127.0.0.1", "", "", null, true, null, null, below, out string here));
         Assert.Equal(below, here);

         // Only for a REST API that is off: the other reasons are the same on every page.
         Assert.Null(ControlDeck.Address("8045", "127.0.0.1", "", "", null, false, "mail.example.com", null, below, out string remote));
         Assert.Equal("The REST API listens on 127.0.0.1 only, so the Control Deck opens only in a browser on the server itself.", remote);

         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         string setting = WithheldRowTests.MemberText(feature, "private sealed class DeckLinkSetting : Setting");
         Assert.True(setting != null, "FeatureSettingsView.DeckLinkSetting is gone.");
         Assert.Contains("ControlDeckLink.Row(L(\"Open the Control Deck\"), L(\"" + below + "\"))", setting, StringComparison.Ordinal);
         Assert.Single(Regex.Matches(feature, @"new DeckLinkSetting\(\)"));
         Assert.Matches(new Regex(@"Settings =\s*\{\s*new DeckLinkSetting\(\),\s*[^}]*Key = ""RestApiPort"""), feature);
      }

      /// <summary>
      /// Discovery's reason for a REST API that is off sends the reader to give it a
      /// port, and not to the Control Deck, which reads the cases over the same REST
      /// API and whose button beside it is disabled for the same reason; it said "Turn
      /// it on under Settings, or read the cases in the Control Deck" (found
      /// re-checking eeff3c210). A REST API that cannot listen without a certificate is
      /// the same; only one that listens on the server alone sends the reader to the
      /// Deck - on the server, where it opens.
      /// </summary>
      [Theory]
      [InlineData("0", "127.0.0.1", "", "", true, true)]
      [InlineData("", null, null, null, true, true)]
      [InlineData("70000", "127.0.0.1", "", "", true, true)]
      [InlineData("off", "127.0.0.1", "", "", false, true)]
      [InlineData("8045", "0.0.0.0", "", "", true, false)]
      public void NoRefusalOfTheRestApi_SendsTheReaderToADeckItAlsoRefuses(string port, string bind, string certificate, string key, bool local, bool off)
      {
         Assert.Null(DiscoveryReport.Endpoint(port, bind, certificate, key, false, local, "mail.example.com", out string refusal));
         Assert.Null(ControlDeck.Address(port, bind, certificate, key, false, local, "mail.example.com", out _));
         Assert.DoesNotContain("in the Control Deck", refusal, StringComparison.Ordinal);
         Assert.DoesNotContain("under Settings", refusal, StringComparison.Ordinal);
         if (off)
            Assert.Contains("Give it a port there.", refusal, StringComparison.Ordinal);

         // Listening on the server alone, from another machine: the Deck in a browser there.
         Assert.Null(DiscoveryReport.Endpoint("8045", "127.0.0.1", "", "", null, false, "mail.example.com", out string remote));
         Assert.EndsWith("Read the cases in the Control Deck on the server.", remote, StringComparison.Ordinal);
      }

      /// <summary>
      /// The button is where the texts send the reader: in Discovery's header, read
      /// again with the cases, and on the cards whose settings are the server's half
      /// of what the Deck holds, and on the REST API's own card.
      /// </summary>
      [Fact]
      public void TheButton_IsBesideTheTextsThatNameTheDeck()
      {
         string discovery = ReadControlPanelFile(Path.Join("Views", "DiscoveryView.cs"));
         Assert.Contains("private readonly Wpf.Ui.Controls.Button deck_ = ControlDeckLink.Button(L(\"Open the Control _Deck\"));", discovery, StringComparison.Ordinal);
         Assert.Contains("Actions = new StackPanel { Orientation = Orientation.Horizontal, Children = { deck_, refresh } }", discovery, StringComparison.Ordinal);
         string reload = WithheldRowTests.MemberText(discovery, "private async void Reload_()");
         Assert.True(reload != null, "DiscoveryView.Reload_ is gone.");
         Assert.Contains("ControlDeckLink.Update(deck_);", reload, StringComparison.Ordinal);

         string settings = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         foreach ((string card, string title) in new[]
         {
            ("traps", "Spam traps"), ("compromise", "Compromised accounts"), ("alerts", "Alerts"),
            ("webhooks", "Outbound webhooks"), ("migration", "Migrations")
         })
         {
            // The card says the Deck holds the rest, and its first row is the button.
            Match statement = Regex.Match(settings, @"var " + card + @" = Card\(L\(""" + Regex.Escape(title) + @"""\),\s*L\(""(?<blurb>[^""]*)""\)\);\s*" + card + @"\.Settings\.Add\(new DeckLinkRow\(\)\);");
            Assert.True(statement.Success, "The " + title + " card's first row is not the Control Deck's button.");
            Assert.Contains("Control Deck", statement.Groups["blurb"].Value, StringComparison.Ordinal);
         }

         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.Matches(new Regex(@"Title = L\(""REST administration API and the Control Deck""\),\s*Blurb = L\(""[^""]*the browser-based Control Deck at the listener root[^""]*""\),\s*Settings =\s*\{\s*new DeckLinkSetting\(\),"), feature);
      }

      /// <summary>
      /// A dialog's note that sends the reader to the Deck has the button under it -
      /// the account's booking policy, the domain's reports received and webhook
      /// subscriptions, the list manager's members' delivery - as the setup
      /// checklist's sandbox finding does, which says where in the Deck to look: each
      /// named the Deck with no button (found re-checking eeff3c210).
      /// </summary>
      [Fact]
      public void TheButton_IsUnderTheDialogsNotesAndTheChecklistsFinding()
      {
         int notes = 0;
         foreach (string path in Directory.GetFiles(Path.GetDirectoryName(ControlPanelPath(Path.Join("Views", "ControlDeckLink.cs"))), "*Dialog.cs"))
         {
            string code = File.ReadAllText(path).Replace("\r\n", "\n");
            foreach (Match note in Regex.Matches(code, @"(?:Note|WithHint)\(L\(""(?<text>[^""]*\bControl Deck\b[^""]*)""\)\)\);\n(?<next>[^\n]*)"))
            {
               notes++;
               Assert.True(note.Groups["next"].Value.Trim().EndsWith(".Children.Add(ControlDeckLink.DialogRow());", StringComparison.Ordinal),
                  Path.GetFileName(path) + ": the note \"" + note.Groups["text"].Value + "\" has no Open the Control Deck button under it.");
            }
         }

         Assert.Equal(4, notes);

         string checks = ReadControlPanelFile(Path.Join("Services", "ExternalSetupChecks.cs")).Replace("\r\n", "\n");
         Match finding = Regex.Match(checks, @"item\.Add\(SetupItemState\.\w+,\s*L\(""[^""]*\bControl Deck\b[^""]*""\)\);\n(?<next>[^\n]*)");
         Assert.True(finding.Success, "No setup finding names the Control Deck.");
         Assert.Equal("item.Findings[item.Findings.Count - 1].NamesControlDeck = true;", finding.Groups["next"].Value.Trim());
         string view = ReadControlPanelFile(Path.Join("Views", "ExternalSetupView.cs"));
         Assert.Matches(new Regex(@"if \(finding\.NamesControlDeck\)\s*\{\s*FrameworkElement deck = ControlDeckLink\.Row\(L\(""Open the Control Deck""\)\);"), view);
      }

      /// <summary>A disabled button says why, and a pressed one starts the browser.</summary>
      [Fact]
      public void TheButton_SaysWhatItOpens_OrWhyNot()
      {
         string link = ReadControlPanelFile(Path.Join("Views", "ControlDeckLink.cs"));
         string update = WithheldRowTests.MemberText(link, "public static string Update(Wpf.Ui.Controls.Button button, string whyOffHere = null)");
         Assert.True(update != null, "ControlDeckLink.Update is gone.");
         Assert.Contains("button.IsEnabled = deck != null;", update, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetHelpText(button, help);", update, StringComparison.Ordinal);
         Assert.Contains("ToolTipService.SetShowOnDisabled(button, true);", link, StringComparison.Ordinal);
         Assert.Contains("new ProcessStartInfo(deck.AbsoluteUri) { UseShellExecute = true }", link, StringComparison.Ordinal);
      }

      /// <summary>It is the Control Deck in every English text: not the Web Control Deck, not the Deck alone.</summary>
      [Fact]
      public void TheDeck_HasOneName()
      {
         XDocument catalogue = XDocument.Load(ControlPanelPath(Path.Join("Resources", "Strings.resx")));
         var keys = catalogue.Root.Elements("data").Select(d => (string)d.Attribute("name")).Where(k => k != null && k.Contains("Deck", StringComparison.Ordinal)).ToList();
         Assert.NotEmpty(keys);
         Assert.All(keys, key =>
         {
            Assert.DoesNotContain("Web Control Deck", key, StringComparison.Ordinal);
            Assert.Equal(Regex.Matches(key, @"\bDeck\b").Count, Regex.Matches(key, @"\bControl Deck\b").Count);
         });
      }

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelPath(relative));

      private static string ControlPanelPath(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(directory.FullName, "ControlPanel", "Views")))
               return candidate;
         }

         throw new FileNotFoundException("The Control Panel's source was not found above " + AppContext.BaseDirectory, relative);
      }
   }
}
