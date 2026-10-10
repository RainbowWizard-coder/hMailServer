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
   /// The Control Panel's toasts sit over the page, inside the content column and at
   /// most 560 pixels wide, and the failure a page's entry reports offers Try again
   /// (review finding F008). The presenter was a child of the window's outer grid,
   /// centred across the sidebar and the page with no width limit, so that "Server
   /// unavailable / Could not load this page: ..." was one 1,160-pixel line over the
   /// sidebar's rows and the version footer, with only a close button.
   /// </summary>
   public class ToastPlacementTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Fact]
      public void TheToasts_AreInsideTheContentColumn_AtMost560PixelsWide()
      {
         XDocument window = XDocument.Parse(Read("MainWindow.xaml"));
         XElement presenter = window.Descendants().Single(e => (string)e.Attribute(X + "Name") == "RootSnackbar");
         XElement surface = window.Descendants().Single(e => (string)e.Attribute(X + "Name") == "ContentSurface");

         Assert.Equal("SnackbarPresenter", presenter.Name.LocalName);
         Assert.Contains(presenter, surface.Descendants());
         Assert.Equal("560", (string)presenter.Attribute("MaxWidth"));
         Assert.Equal("Bottom", (string)presenter.Attribute("VerticalAlignment"));
         Assert.Equal("Center", (string)presenter.Attribute("HorizontalAlignment"));
      }

      [Fact]
      public void APageThatCouldNotBeEntered_OffersTryAgain_WhileItIsStillOnScreen()
      {
         string shell = Read("MainWindow.xaml.cs");
         int enter = shell.IndexOf("private void EnterPage(object page)", StringComparison.Ordinal);
         Assert.True(enter >= 0, "MainWindow no longer has EnterPage.");
         string body = shell.Substring(enter, shell.IndexOf("finally", enter, StringComparison.Ordinal) - enter);
         Assert.Contains("() => TryEnterPageAgain_(page));", body, StringComparison.Ordinal);

         int again = shell.IndexOf("private void TryEnterPageAgain_(object page)", StringComparison.Ordinal);
         Assert.True(again >= 0, "MainWindow no longer has TryEnterPageAgain_.");
         Assert.Matches(new Regex(@"if \(page != null && ReferenceEquals\(ContentHost\.Content, page\)\)\s*EnterPage\(page\);"), shell.Substring(again, 300));
      }

      [Fact]
      public void TheFailureWithTryAgain_IsItsOwnSnackbar_WithTheReadFailuresCaption()
      {
         string toast = Read(Path.Join("Services", "Toast.cs"));
         int start = toast.IndexOf("public static void Failure(string message, string title, Action tryAgain)", StringComparison.Ordinal);
         Assert.True(start >= 0, "Toast has no Failure with Try again.");
         string body = toast.Substring(start, toast.IndexOf("private const int FailureWithActionSeconds", start, StringComparison.Ordinal) - start);

         // The same caption as a read failure's Try again, which carries no Alt key.
         Assert.Contains("Content = L(\"Try again\")", body, StringComparison.Ordinal);
         Assert.Contains("new Snackbar(presenter_)", body, StringComparison.Ordinal);
         Assert.Contains("TextWrapping = System.Windows.TextWrapping.Wrap", body, StringComparison.Ordinal);
         // Pressed, it closes the toast and runs the action.
         Assert.Matches(new Regex(@"_ = presenter_\.HideCurrent\(\);\s*tryAgain\(\);"), body);
      }

      private static string Read(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
