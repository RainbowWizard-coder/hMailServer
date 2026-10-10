// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The sign-in page in the smallest window the Control Panel allows (760 by
   /// 520): the card is about 560 pixels tall and the content area about 420, so
   /// the card's bottom edge ran across the password box, and Connect, the failure
   /// notice, the two-factor link and the language picker were not drawn and
   /// could not be scrolled to - only Enter in the password box still signed in.
   /// Every test here fails against that page: the card is in a scroll viewer that
   /// scrolls up and down, it keeps its centring for a window it fits in, and a
   /// failed sign-in scrolls its notice into view.
   /// </summary>
   public class ConnectPageTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      [Fact]
      public void TheCard_IsInAScrollViewer_ThatScrollsUpAndDown()
      {
         XElement page = Page();
         XElement scroll = page.Elements(Wpf + "ScrollViewer").SingleOrDefault();
         Assert.True(scroll != null, "The page's root is not a ScrollViewer, so a window shorter than the card cuts it off.");
         Assert.Equal("Auto", (string)scroll.Attribute("VerticalScrollBarVisibility"));
         Assert.Equal("Disabled", (string)scroll.Attribute("HorizontalScrollBarVisibility"));
         Assert.Equal("False", (string)scroll.Attribute("Focusable"));

         XElement card = scroll.Descendants(Scaffold + "Card").First();
         Assert.Equal("Center", (string)card.Attribute("VerticalAlignment"));
         Assert.Equal("Center", (string)card.Attribute("HorizontalAlignment"));
      }

      [Fact]
      public void NothingBetweenTheCardAndTheScrollViewer_IsHeldToAHeight()
      {
         XElement page = Page();
         XElement scroll = page.Elements(Wpf + "ScrollViewer").Single();
         XElement card = scroll.Descendants(Scaffold + "Card").First();
         for (XElement e = card.Parent; e != null && e != scroll; e = e.Parent)
         {
            Assert.Null(e.Attribute("Height"));
            Assert.Null(e.Attribute("MaxHeight"));
         }
      }

      [Fact]
      public void AFailedSignIn_ScrollsItsNoticeIntoView()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "ConnectView.xaml.cs"));
         string fail = WithheldRowTests.MemberText(code, "private void Fail_(string message, StatusLevel level)");
         Assert.True(fail != null, "ConnectView.Fail_ is gone.");
         Assert.Contains("ErrorNotice.BringIntoView();", fail, StringComparison.Ordinal);
      }

      private static XElement Page() => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "ConnectView.xaml"))).Root;

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
