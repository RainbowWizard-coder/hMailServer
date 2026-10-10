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

      /// <summary>A disabled button says why, and a pressed one starts the browser.</summary>
      [Fact]
      public void TheButton_SaysWhatItOpens_OrWhyNot()
      {
         string link = ReadControlPanelFile(Path.Join("Views", "ControlDeckLink.cs"));
         string update = WithheldRowTests.MemberText(link, "public static string Update(Wpf.Ui.Controls.Button button)");
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
