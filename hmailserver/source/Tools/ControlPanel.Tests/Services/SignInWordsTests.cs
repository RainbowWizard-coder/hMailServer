// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The sign-in card names its act with one verb, as the page is named (review
   /// findings F028 and F316, the owner's choice of 9 October 2026: the heading and
   /// the button both say Sign in). The card said "Connect to server" over "Sign in
   /// with the hMailServer administrator credential." and a "Connect" button, then
   /// "Signing in…" while it worked, and German and Japanese repeated the split.
   /// It also said who signs in as if the Administrator alone did: an account that
   /// administers a domain or holds a role signs in on the same card with its own
   /// password. Every test here fails against that card.
   /// </summary>
   public class SignInWordsTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";

      [Fact]
      public void TheHeadingTheButtonAndTheCardsName_SaySignIn()
      {
         XElement page = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "ConnectView.xaml"))).Root;
         XElement card = page.Descendants(Scaffold + "Card").First();
         Assert.Equal("{loc:L 'Sign in'}", (string)card.Attribute("AutomationProperties.Name"));

         XElement heading = card.Descendants(Wpf + "TextBlock").First(t => (string)t.Attribute("Style") == "{StaticResource TextSubtitle}");
         Assert.Equal("{loc:L 'Sign in'}", (string)heading.Attribute("Text"));

         XElement button = card.Descendants(Ui + "Button").Single(b => (string)b.Attribute(X + "Name") == "ConnectButton");
         Assert.Equal("Primary", (string)button.Attribute("Appearance"));
         Assert.Equal("Sign in", LabelInName.Shown(Unmark((string)button.Attribute("Content"))));

         // The busy text before an attempt's own words replace it.
         XElement busy = card.Descendants(Wpf + "TextBlock").Single(t => (string)t.Attribute(X + "Name") == "BusyText");
         Assert.Equal("{loc:L 'Signing in…'}", (string)busy.Attribute("Text"));
      }

      [Fact]
      public void TheCardNamesNoOtherVerbForTheAct()
      {
         // Connected and Not connected stay the pill's words for the link; the
         // card's own words are Sign in.
         string xaml = ReadControlPanelFile(Path.Join("Views", "ConnectView.xaml"));
         string code = ReadControlPanelFile(Path.Join("Views", "ConnectView.xaml.cs"));
         foreach (string text in new[] { "'Connect to server'", "'_Connect'", "'Connecting…'", "administrator credential" })
            Assert.DoesNotContain(text, xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Connecting…\")", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSubtitleAndTheHint_SayWhoSignsInHere()
      {
         string xaml = ReadControlPanelFile(Path.Join("Views", "ConnectView.xaml"));
         Assert.Contains("{loc:L 'Sign in as the server\\'s Administrator, or with your own account if it administers a domain or holds a role.'}", xaml, StringComparison.Ordinal);
         Assert.Contains("Hint=\"{loc:L 'For Administrator, the password chosen when hMailServer was installed; for your own account, its own password.'}\"", xaml, StringComparison.Ordinal);
      }

      /// <summary>
      /// In every language the button's words are the heading's: one verb for the
      /// act in German and Japanese too, whose catalogues said Verbinden and 接続 on
      /// the button under a subtitle that said anmelden and サインイン.
      /// </summary>
      [Fact]
      public void InEveryLanguage_TheButtonSaysWhatTheHeadingSays()
      {
         string resources = ResourcesDirectory();
         var failures = new List<string>();
         foreach (string path in Directory.GetFiles(resources, "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(path);
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            if (!catalogue.TryGetValue("Sign in", out string heading) || !catalogue.TryGetValue("Sign _in", out string button))
            {
               failures.Add(tag + ": no translation of Sign in or Sign _in");
               continue;
            }

            if (!string.Equals(LabelInName.Shown(button), heading, StringComparison.Ordinal))
               failures.Add(tag + ": the button says " + LabelInName.Shown(button) + ", the heading " + heading);
         }

         Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
      }

      [Fact]
      public void TheWelcomeIntent_ProtectsTheAdministratorPassword()
      {
         // The card and the Administrative access page call it the administrator
         // password; "credential" was a word for it no page used (F316).
         WelcomeIntent intent = WelcomeIntents.Entries.Single(i => i.Page == "adminaccess");
         Assert.Equal("Protect the administrator password", intent.Heading);
      }

      private static string Unmark(string attribute)
      {
         const string start = "{loc:L '";
         Assert.StartsWith(start, attribute, StringComparison.Ordinal);
         return attribute.Substring(start.Length, attribute.Length - start.Length - 2);
      }

      private static Dictionary<string, string> ReadResx(string path)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }

      private static string ResourcesDirectory()
      {
         string dir = AppContext.BaseDirectory;
         for (int i = 0; i < 8 && dir != null; i++)
         {
            string candidate = Path.Join(dir, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            dir = Path.GetDirectoryName(dir);
         }

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
