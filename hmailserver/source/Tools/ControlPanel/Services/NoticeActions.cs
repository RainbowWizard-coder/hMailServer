// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The action a notice carries when what it says is put right on another page:
   /// a button in the notice's action slot (InlineNotice.Content) that opens that
   /// page, named for it - "Open IP ranges" - from the navigation map's own title,
   /// so that the button and the page it opens never disagree about the page's
   /// name, in any language.
   ///
   /// A warning that names another page and offers no way to it - "No public host
   /// name is configured in the SMTP settings" on the Server status page - leaves
   /// the reader to find the page in the sidebar; this is the way there from where
   /// the problem is read.
   ///
   /// It carries no Alt key, as Try again does not (Services/ReadFailure): a page
   /// can show several notices that open the same page, and no letter is free of
   /// a collision on every page that shows one. It is reached by Tab, as the one
   /// action in a notice.
   /// </summary>
   public static class NoticeActions
   {
      /// <summary>
      /// A button that opens the page with the navigation key
      /// <paramref name="page"/>, captioned "Open" and the page's title.
      /// </summary>
      public static Wpf.Ui.Controls.Button OpenPage(string page, string automationId)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = F("Open {0}", L(NavigationMap.TitleOf(page))),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            VerticalAlignment = VerticalAlignment.Center
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, automationId);
         button.Click += (_, _) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo(page);
         return button;
      }
   }
}
