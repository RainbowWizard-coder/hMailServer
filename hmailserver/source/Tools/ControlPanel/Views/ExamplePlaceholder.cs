// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The grey text in an empty box that shows an example of what goes there, said to
   /// be one: "e.g. dc01.example.local", as the IP range captions already write theirs.
   ///
   /// Shown alone, an example read as a setting: in the light theme a placeholder is a
   /// grey only a little lighter than a value, and the Directory authentication page
   /// showed "dc01.example.local" in its host name box directly under the notice that
   /// the host name was missing (review finding F138). A placeholder that shows the
   /// default an empty box stands for is not an example and does not use this; its
   /// hint says the default.
   /// </summary>
   internal static class ExamplePlaceholder
   {
      /// <summary>The example, marked as one in the reader's language.</summary>
      public static string Of(string example) => F("e.g. {0}", example);
   }
}
