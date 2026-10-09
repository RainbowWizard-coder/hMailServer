// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.Text.RegularExpressions;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// A control's accessible name starts with the words on it (WCAG 2.5.3,
   /// Label in Name), so that what a screen reader says is what is on screen and
   /// "click Settings" in voice control reaches the button that says Settings.
   /// The words a caption shows are the caption without its Alt-key underscore -
   /// and without the (_X) a Chinese or Japanese caption carries its key in - and
   /// without the ellipsis or colon it may end in.
   ///
   /// Plain functions, without WPF, so that LabelInNameTests holds the rule and
   /// the names it makes; Views/ControlHelp is the half that sets them.
   /// </summary>
   public static class LabelInName
   {
      /// <summary>The words <paramref name="caption"/> shows.</summary>
      public static string Shown(string caption)
      {
         string text = MnemonicText.Strip(caption ?? "");
         text = Regex.Replace(text, @"\((?:\w)\)", "");
         return text.TrimEnd('.', '…', ' ', ':').Trim();
      }

      /// <summary>Whether <paramref name="name"/> starts with the words <paramref name="caption"/> shows.</summary>
      public static bool StartsWithCaption(string name, string caption)
         => StartsWithCaption(name, caption, CultureInfo.CurrentCulture);

      /// <summary>The same, with the case rules of <paramref name="culture"/>, the language the two are in.</summary>
      public static bool StartsWithCaption(string name, string caption, CultureInfo culture)
         => (name ?? "").StartsWith(Shown(caption), ignoreCase: true, culture);

      /// <summary>
      /// The name of a link among several whose captions say less than where they
      /// go - "Settings…" beside each of ten checks, "Open…" beside each of six
      /// lists: the caption, then the destination, "Settings: Open Blocked
      /// senders, which owns the Blocked senders settings" - or the destination
      /// alone when it already starts with the caption, as "Open Domains, which
      /// owns…" does. Ten links called "Settings…" were ten names a screen reader
      /// could not tell apart; ten called by their destination were ten names a
      /// voice-control user could not say.
      /// </summary>
      public static string LinkName(string caption, string destination)
      {
         if (string.IsNullOrEmpty(destination))
            return Shown(caption);

         return StartsWithCaption(destination, caption) ? destination : F("{0}: {1}", Shown(caption), destination);
      }
   }
}
