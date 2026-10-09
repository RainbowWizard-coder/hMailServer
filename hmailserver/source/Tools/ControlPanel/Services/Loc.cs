// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Control Panel's translation lookup. The English text is the key: a
   /// caption is written once, in English, where it is used - <c>L("_Save changes")</c>
   /// in code, <c>{loc:L '_Save changes'}</c> in XAML - and the catalogue for the
   /// current language (Resources/Strings.&lt;culture&gt;.resx) maps that text to
   /// its translation. Anything the catalogue does not hold comes back in English,
   /// so a partly translated language degrades to English words rather than to
   /// blank controls or resource-key names, and the English catalogue itself
   /// (Resources/Strings.resx) is generated from the source rather than written by
   /// hand: build/check-localisation.py keeps it equal to the set of marked texts
   /// and fails the build when it is not.
   ///
   /// Three markers, so that a scan of the source finds every translatable text:
   /// <see cref="L"/> translates now; <see cref="F"/> translates and formats;
   /// <see cref="N"/> only marks, for a table whose English text has to survive
   /// (the navigation map is compared against the documentation by its English
   /// titles) and is translated where it is shown.
   ///
   /// Nothing here touches WPF or the registry. The application chooses the
   /// culture and the catalogue at start-up (<see cref="Use"/>); the tests hand
   /// in a dictionary.
   /// </summary>
   public static class Loc
   {
      /// <summary>A language the Control Panel can show, as offered to the user.</summary>
      public sealed class Language
      {
         internal Language(string tag, string nativeName)
         {
            Tag = tag;
            NativeName = nativeName;
         }

         /// <summary>BCP 47 tag, or empty for "whatever Windows displays in".</summary>
         public string Tag { get; }

         /// <summary>The language's name in itself, which is how a language picker is readable to the person who needs it.</summary>
         public string NativeName { get; }
      }

      /// <summary>
      /// The languages a catalogue exists for, plus the empty tag for following
      /// Windows. Adding a language is a new Strings.&lt;tag&gt;.resx and a row here.
      /// </summary>
      public static readonly IReadOnlyList<Language> Languages = new[]
      {
         new Language("", N("Windows display language")),
         new Language("en", "English"),   // no-loc: a language is named in itself
         new Language("cs", "Čeština"),   // no-loc
         new Language("da", "Dansk"),   // no-loc
         new Language("de", "Deutsch"),   // no-loc
         new Language("es", "Español"),   // no-loc
         new Language("fi", "Suomi"),   // no-loc
         new Language("fr", "Français"),   // no-loc
         new Language("it", "Italiano"),   // no-loc
         new Language("ja", "日本語"),   // no-loc
         new Language("nb", "Norsk bokmål"),   // no-loc
         new Language("nl", "Nederlands"),   // no-loc
         new Language("pl", "Polski"),   // no-loc
         new Language("pt-BR", "Português (Brasil)"),   // no-loc
         new Language("ru", "Русский"),   // no-loc
         new Language("sv", "Svenska"),   // no-loc
         new Language("tr", "Türkçe"),   // no-loc
         new Language("uk", "Українська"),   // no-loc
         new Language("zh-Hans", "简体中文"),   // no-loc
      };

      private static Func<string, CultureInfo, string> lookup_;
      private static CultureInfo culture_;
      private static bool english_ = true;

      /// <summary>The UI culture in force, or null when nothing has been chosen (English).</summary>
      public static CultureInfo Culture => culture_;

      /// <summary>
      /// Installs the catalogue and the culture. <paramref name="lookup"/> takes
      /// the English text and the culture and returns the translation, or null
      /// or empty when it has none; the application passes ResourceManager's
      /// GetString. A null lookup, or an English culture, makes every call an
      /// identity - the common case costs nothing.
      /// </summary>
      public static void Use(Func<string, CultureInfo, string> lookup, CultureInfo culture)
      {
         lookup_ = lookup;
         culture_ = culture;
         english_ = lookup == null || culture == null || IsEnglish(culture);
      }

      /// <summary>Back to untranslated English; the tests use it between cases.</summary>
      public static void Reset() => Use(null, null);

      /// <summary>
      /// The culture a stored language choice resolves to: the tag itself when
      /// one is stored, the Windows display language when the choice is empty,
      /// and English when the tag is not one Windows knows.
      /// </summary>
      public static CultureInfo Resolve(string tag)
      {
         if (string.IsNullOrWhiteSpace(tag))
            return CultureInfo.InstalledUICulture;

         try
         {
            return CultureInfo.GetCultureInfo(tag.Trim());
         }
         catch (CultureNotFoundException)
         {
            return CultureInfo.GetCultureInfo("en");
         }
      }

      /// <summary>True for English of any region and for the invariant culture.</summary>
      public static bool IsEnglish(CultureInfo culture)
         => culture == null
            || culture.Equals(CultureInfo.InvariantCulture)
            || string.Equals(culture.TwoLetterISOLanguageName, "en", StringComparison.OrdinalIgnoreCase);

      /// <summary>
      /// True when <paramref name="culture"/> is one of <see cref="Languages"/> other than
      /// English, itself or through a parent (sv-SE is sv, zh-CN is zh-Hans): a language
      /// the catalogue translates into. False for English and for a language it has no
      /// catalogue for (Korean, say), in which the Control Panel's own words are English.
      /// </summary>
      public static bool HasCatalogue(CultureInfo culture)
      {
         for (CultureInfo current = culture; current != null && !current.Equals(CultureInfo.InvariantCulture); current = current.Parent)
         {
            if (IsEnglish(current))
               return false;

            foreach (Language language in Languages)
            {
               if (language.Tag.Length > 0 && string.Equals(language.Tag, current.Name, StringComparison.OrdinalIgnoreCase))
                  return true;
            }
         }

         return false;
      }

      /// <summary>
      /// True when the words <see cref="L"/> gives are translated: the culture in force
      /// has a catalogue (<see cref="HasCatalogue"/>) and it could be read.
      /// </summary>
      public static bool Translated => !english_ && HasCatalogue(culture_);

      /// <summary>The translation of <paramref name="english"/>, or the English itself when there is none.</summary>
      public static string L(string english)
      {
         if (english_ || string.IsNullOrEmpty(english))
            return english;

         string translated;
         try
         {
            translated = lookup_(english, culture_);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // A catalogue that cannot be read (a satellite assembly missing from
            // an installation, a corrupt resource) is worth an English caption,
            // not an exception in the middle of building a page - and not one
            // exception per caption, so stop asking.
            english_ = true;
            return english;
         }

         return string.IsNullOrEmpty(translated) ? english : translated;
      }

      /// <summary>
      /// Translates and then formats, with the user's own number and date
      /// conventions. The placeholders must be the same set in every
      /// translation; the checker enforces that, because a missing {0} is a
      /// sentence with no subject and an extra one is FormatException.
      /// </summary>
      public static string F(string english, params object[] args)
         => string.Format(CultureInfo.CurrentCulture, L(english), args);

      /// <summary>
      /// Marks a text for the catalogue without translating it here: the value
      /// stays English for the table it sits in and is passed through
      /// <see cref="L"/> at the point it is displayed.
      /// </summary>
      public static string N(string english) => english;

      /// <summary>
      /// A sentence about a count, in the form the language in force uses for
      /// that number, with the count as {0}: <c>Plural(n, "{0} message held.",
      /// "{0} messages held.")</c>. Both forms are catalogue keys and both carry
      /// the count, because "one" in a language's plural rules is not always the
      /// number 1 - Russian and Ukrainian say 21 the way they say 1.
      ///
      /// <paramref name="one"/> is the English for one thing, <paramref name="other"/>
      /// for any other number. The language's CLDR plural category of the count
      /// chooses: "one" takes the translation of <paramref name="one"/>; "few"
      /// and "many" take the catalogue's entry for <paramref name="other"/>
      /// followed by "|few" or "|many" where the catalogue has one, and the
      /// translation of <paramref name="other"/> where it has not; anything else
      /// takes the translation of <paramref name="other"/>. Czech, Polish,
      /// Russian and Ukrainian hold a "|few" form, for two to four of a thing
      /// (and 22 to 24 in all but Czech); in Polish, Russian and Ukrainian the
      /// form of <paramref name="other"/> itself is the one for five and more.
      /// A text with two counts is two sentences, or a label form that needs
      /// no plural in any language ("Rules: {0}").
      ///
      /// Pairs written as <c>n == 1 ? L(one) : F(other, n)</c> gave Polish,
      /// Czech and Ukrainian one plural for 2 and for 5, and a "(s)" ending
      /// gave every language an English plural to translate (review finding
      /// F282); build/check-localisation.py refuses both now, reads these
      /// calls' two literals as catalogue keys and holds the catalogues of the
      /// languages that need it to a "|few" form of each.
      /// </summary>
      public static string Plural(long count, string one, string other)
         => string.Format(CultureInfo.CurrentCulture, PluralForm(count, one, other), count);

      /// <summary>
      /// <see cref="Plural"/> for a sentence that carries more than its count:
      /// the count chooses the form, and <paramref name="args"/> fill it, the
      /// count among them where the sentence shows it.
      /// </summary>
      public static string PluralF(long count, string one, string other, params object[] args)
         => string.Format(CultureInfo.CurrentCulture, PluralForm(count, one, other), args);

      /// <summary>The translated form of <see cref="Plural"/> for <paramref name="count"/>, unformatted.</summary>
      public static string PluralForm(long count, string one, string other)
      {
         string category = PluralCategory(PluralLanguage(), count);
         if (category == "one")
            return L(one);
         if (category != "other" && !english_)
         {
            string form = Lookup_(other + "|" + category);
            if (!string.IsNullOrEmpty(form))
               return form;
         }
         return L(other);
      }

      /// <summary>
      /// The CLDR plural category - "one", "few", "many" or "other" - of a whole
      /// number in a language of <see cref="Languages"/> (unicode.org's plural
      /// rules, for integers: the visible fraction digits are none). A language
      /// with no rule here counts as English does.
      /// </summary>
      public static string PluralCategory(string language, long count)
      {
         long n = count < 0 ? -count : count;
         long mod10 = n % 10, mod100 = n % 100;
         switch (language)
         {
            case "ja":
            case "zh-Hans":
               return "other";
            case "fr":
            case "pt-BR":
               // one: i = 0,1; many: e = 0 and i != 0 and i % 1000000 = 0
               if (n <= 1)
                  return "one";
               return n % 1000000 == 0 ? "many" : "other";
            case "es":
            case "it":
               if (n == 1)
                  return "one";
               return n != 0 && n % 1000000 == 0 ? "many" : "other";
            case "cs":
               // one: i = 1; few: i = 2..4; many: fractions only
               if (n == 1)
                  return "one";
               return n >= 2 && n <= 4 ? "few" : "other";
            case "pl":
               // one: i = 1; few: i % 10 = 2..4 and i % 100 != 12..14; many: the rest of the integers
               if (n == 1)
                  return "one";
               return mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14) ? "few" : "many";
            case "ru":
            case "uk":
               // one: i % 10 = 1 and i % 100 != 11; few: i % 10 = 2..4 and i % 100 != 12..14; many: the rest
               if (mod10 == 1 && mod100 != 11)
                  return "one";
               return mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14) ? "few" : "many";
            default:
               // English, German, Dutch, the Nordic languages, Finnish and Turkish: one: i = 1
               return n == 1 ? "one" : "other";
         }
      }

      // The tag of Languages whose words L gives: the culture in force or a parent
      // of it, or English when nothing is translated.
      private static string PluralLanguage()
      {
         if (english_)
            return "en";
         for (CultureInfo current = culture_; current != null && !current.Equals(CultureInfo.InvariantCulture); current = current.Parent)
         {
            foreach (Language language in Languages)
            {
               if (language.Tag.Length > 0 && string.Equals(language.Tag, current.Name, StringComparison.OrdinalIgnoreCase))
                  return language.Tag;
            }
         }
         return "en";
      }

      // The catalogue's own entry for a key, or null: no English in its place.
      private static string Lookup_(string key)
      {
         try
         {
            return lookup_?.Invoke(key, culture_);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            english_ = true;
            return null;
         }
      }
   }
}
