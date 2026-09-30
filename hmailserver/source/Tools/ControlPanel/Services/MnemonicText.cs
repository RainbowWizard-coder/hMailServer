// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Text;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The caption convention WPF uses for Alt-key access mnemonics, kept in one
   /// place: a single underscore marks the letter ("_Save changes" is Alt+S) and a
   /// doubled underscore is a literal one. A button or a check box reads the
   /// convention through its own template. A caption rendered by a TextBlock does
   /// not, and neither does text that arrives from data rather than from the code,
   /// so both go through here - the first to underline the letter and register the
   /// key, the second to make sure a folder called "in_tray" never grows an access
   /// key of its own.
   /// </summary>
   public static class MnemonicText
   {
      /// <summary>
      /// Splits a caption into the text to display and the index within that text
      /// of the mnemonic letter, or -1 when the caption carries none. The first
      /// single underscore that is followed by a visible character marks the key;
      /// every later single underscore, and a doubled one anywhere, is text.
      /// </summary>
      public static (string Text, int KeyIndex) Parse(string caption)
      {
         if (string.IsNullOrEmpty(caption))
            return (string.Empty, -1);

         var text = new StringBuilder(caption.Length);
         int keyIndex = -1;

         for (int i = 0; i < caption.Length; i++)
         {
            char c = caption[i];
            if (c != '_')
            {
               text.Append(c);
               continue;
            }

            bool doubled = i + 1 < caption.Length && caption[i + 1] == '_';
            if (doubled)
            {
               text.Append('_');
               i++;
               continue;
            }

            bool marksALetter = keyIndex < 0 && i + 1 < caption.Length && !char.IsWhiteSpace(caption[i + 1]);
            if (marksALetter)
               keyIndex = text.Length;
            else
               text.Append('_');
         }

         return (text.ToString(), keyIndex);
      }

      /// <summary>The caption with its markers resolved - what a person reads or a screen reader speaks.</summary>
      public static string Strip(string caption) => Parse(caption).Text;

      /// <summary>
      /// A caption as WPF's own AccessText draws it - the words of a button, a check
      /// box or a label whose template recognises access keys, and the name its
      /// automation peer gives. The first underscore followed by anything but another
      /// underscore marks the key and is not drawn, whatever follows it, and every
      /// doubled underscore is drawn as one. It differs from <see cref="Strip"/> only
      /// where an underscore comes before white space: WPF draws "odd_ one" as
      /// "odd one", the space its key, where Parse keeps the underscore as text for
      /// the captions the Control Panel draws itself.
      /// </summary>
      public static string AccessTextShown(string caption)
      {
         if (string.IsNullOrEmpty(caption))
            return string.Empty;

         for (int i = 0; i + 1 < caption.Length; i++)
         {
            if (caption[i] != '_')
               continue;

            if (caption[i + 1] != '_')
            {
               caption = caption.Remove(i, 1);
               break;
            }

            i++;   // a doubled underscore is text, never the marker
         }

         return caption.Replace("__", "_");
      }

      /// <summary>The access key as WPF names it (upper case), or null when the caption has none.</summary>
      public static string Key(string caption)
      {
         var (text, index) = Parse(caption);
         return index < 0 ? null : char.ToUpperInvariant(text[index]).ToString();
      }

      /// <summary>
      /// Text that came from data, made safe for a control whose template
      /// recognises access keys: every underscore doubled, so it displays as itself.
      /// </summary>
      public static string Escape(string text) => text?.Replace("_", "__");
   }
}
