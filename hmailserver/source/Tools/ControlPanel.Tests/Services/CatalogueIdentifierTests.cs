// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A name with an underscore in it - a TLS 1.3 suite, a metric, a server macro, a
   /// DNS label, a file of settings - is written in every language as the English
   /// writes it. Translation steps read such an underscore as an Alt key and moved or
   /// dropped it: the Japanese catalogue wrote TLSAES_256_GCM_SHA384,
   /// hmailservermessagestore_missing_files, &lt;selector&gt;.domainkey.&lt;domain&gt; and
   /// userprefs and ended the four texts - blurbs and notices that no access key
   /// reaches - in a "(_A)", a "(_M)", a "(_D)" and a "(_P)" drawn as they are, which
   /// told an administrator to publish the DKIM record at the wrong name; and the
   /// Danish, Finnish, Norwegian and Polish quota warning's caption wrote
   /// %MACROMAILBOX% and three more macros the server does not substitute, since
   /// WPF takes only a caption's first underscore for its key and shows the others
   /// (review finding F302's catalogue).
   /// </summary>
   public class CatalogueIdentifierTests
   {
      // TLS_AES_256_GCM_SHA384, hmailserver_messagestore_missing_files, CAP_NET_ADMIN.
      private static readonly Regex TwoUnderscores = new(@"(?<![\w%])[A-Za-z0-9]+(?:_[A-Za-z0-9]+){2,}(?!\w)");

      // %MACRO_MAILBOX%: a macro the server substitutes.
      private static readonly Regex Macro = new(@"%[A-Za-z0-9]+(?:_[A-Za-z0-9]+)+%");

      // A DNS label that starts with an underscore: after a dot, ._domainkey. and the
      // _tcp of _25._tcp.; or before one, _dmarc. and _25.
      private static readonly Regex LabelAfterDot = new(@"(?<=\.)_[A-Za-z0-9][A-Za-z0-9-]*");
      private static readonly Regex LabelBeforeDot = new(@"(?<![\w.])_[a-z0-9][a-z0-9-]*(?=\.[A-Za-z0-9_<])");

      // A lower-case snake_case name, user_prefs or view_audit: a name in a sentence,
      // or in a caption whose access key is an earlier underscore. In a caption such
      // as "Warning te_xt" the first underscore is the key.
      private static readonly Regex Snake = new(@"(?<![\w%])[a-z0-9]+(?:_[a-z0-9]+)+(?![\w%])");

      // A Japanese or Chinese caption's key suffix, "(_D)".
      private static readonly Regex KeySuffix = new(@"\(_[A-Za-z0-9]\)");

      [Fact]
      public void ANameWithUnderscores_IsWrittenAsTheEnglishWritesIt()
      {
         var found = new List<string>();
         int catalogues = 0;
         foreach ((string tag, Dictionary<string, string> values) in Catalogues())
         {
            catalogues++;
            foreach (string key in English())
            {
               if (!values.TryGetValue(key, out string value))
                  continue;
               foreach ((int Start, int End, string Text) name in Names(key))
               {
                  if (!value.Contains(name.Text, StringComparison.Ordinal))
                     found.Add(tag + ": " + name.Text + " in " + value);
               }
            }
         }

         Assert.Equal(17, catalogues);
         Assert.True(found.Count == 0, "A name with underscores written otherwise than the English writes it:" + Environment.NewLine + string.Join(Environment.NewLine, found));
      }

      /// <summary>
      /// A Japanese or Chinese text carries a key suffix only where its English has an
      /// access key - an underscore outside every such name: a suffix where each of
      /// the English's underscores is a name's is the name's underscore, moved.
      /// </summary>
      [Fact]
      public void AKeySuffix_IsNeverAnUnderscoreANameLost()
      {
         var found = new List<string>();
         foreach ((string tag, Dictionary<string, string> values) in Catalogues().Where(c => c.Tag == "ja" || c.Tag == "zh-Hans"))
         {
            foreach (string key in English())
            {
               List<(int Start, int End, string Text)> names = Names(key);
               if (names.Count == 0 || HasAccessKey(key, names) || !values.TryGetValue(key, out string value))
                  continue;
               Match suffix = KeySuffix.Match(value);
               if (suffix.Success)
                  found.Add(tag + ": " + suffix.Value + " in " + value);
            }
         }

         Assert.True(found.Count == 0, "A key suffix on a text whose every underscore is a name's:" + Environment.NewLine + string.Join(Environment.NewLine, found));
      }

      /// <summary>The rules find each kind of name, and leave a caption's access key alone.</summary>
      [Fact]
      public void TheRules_FindEachKindOfName_AndNotAnAccessKey()
      {
         static List<string> Of(string key) => Names(key).Select(n => n.Text).ToList();

         Assert.Equal(new[] { "TLS_AES_256_GCM_SHA384" }, Of("Allow TLS_AES_256_GCM_SHA384 only."));
         Assert.Equal(new[] { "%MACRO_MAILBOX%", "%MACRO_QUOTA%" }, Of("Warning te_xt (%MACRO_MAILBOX%, %MACRO_QUOTA%)"));
         Assert.Equal(new[] { "_domainkey" }, Of("Publish <selector>._domainkey.<domain> as TXT."));
         Assert.Equal(new[] { "_tcp", "_25" }, Of("Publish TLSA at _25._tcp.mail.example.com."));
         Assert.Equal(new[] { "_dmarc" }, Of("Publish _dmarc.example.com as TXT."));
         Assert.Equal(new[] { "user_prefs" }, Of("A user_prefs file holds them."));
         Assert.Equal(new[] { "view_audit" }, Of("_Scope (view_audit)"));

         Assert.Empty(Of("Warning te_xt"));
         Assert.Empty(Of("Export everythin_g..."));
         Assert.Empty(Of("_Add"));
         Assert.Empty(Of("Re_fresh"));

         Assert.False(HasAccessKey("Publish <selector>._domainkey.<domain> as TXT.", Names("Publish <selector>._domainkey.<domain> as TXT.")));
         Assert.True(HasAccessKey("Warning te_xt (%MACRO_MAILBOX%)", Names("Warning te_xt (%MACRO_MAILBOX%)")));
      }

      /// <summary>The names in an English key, each with where it starts and ends.</summary>
      private static List<(int Start, int End, string Text)> Names(string key)
      {
         var names = new List<(int Start, int End, string Text)>();
         foreach (Regex rule in new[] { TwoUnderscores, Macro, LabelAfterDot, LabelBeforeDot })
         {
            foreach (Match m in rule.Matches(key))
               names.Add((m.Index, m.Index + m.Length, m.Value));
         }

         int first = FirstSingleUnderscore(key);
         bool sentence = IsSentence(key);
         foreach (Match m in Snake.Matches(key))
         {
            bool holdsTheKey = first >= m.Index && first < m.Index + m.Length;
            if ((sentence || !holdsTheKey) && !names.Any(n => n.Start <= m.Index && m.Index + m.Length <= n.End))
               names.Add((m.Index, m.Index + m.Length, m.Value));
         }
         return names;
      }

      /// <summary>Whether the key has an underscore outside every name - the caption's access key.</summary>
      private static bool HasAccessKey(string key, List<(int Start, int End, string Text)> names)
      {
         for (int i = 0; i < key.Length; i++)
         {
            if (key[i] != '_' || (i + 1 < key.Length && key[i + 1] == '_') || (i > 0 && key[i - 1] == '_'))
               continue;
            if (!names.Any(n => n.Start <= i && i < n.End))
               return true;
         }
         return false;
      }

      private static int FirstSingleUnderscore(string key)
      {
         for (int i = 0; i < key.Length; i++)
         {
            if (key[i] == '_' && !(i + 1 < key.Length && key[i + 1] == '_') && !(i > 0 && key[i - 1] == '_'))
               return i;
         }
         return -1;
      }

      /// <summary>A key that ends as a sentence does - a full stop, not an ellipsis - is no caption with an access key.</summary>
      private static bool IsSentence(string key)
      {
         string text = key.TrimEnd();
         if (text.EndsWith("...", StringComparison.Ordinal) || text.EndsWith("…", StringComparison.Ordinal))
            return false;
         return text.EndsWith('.') || text.EndsWith('?') || text.EndsWith('!') || text.EndsWith(".)", StringComparison.Ordinal) || text.EndsWith(".\"", StringComparison.Ordinal);
      }

      private static string Resources()
         => Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));

      private static List<string> English() => Keys(Path.Join(Resources(), "Strings.resx")).Keys.ToList();

      private static IEnumerable<(string Tag, Dictionary<string, string> Values)> Catalogues()
         => Directory.GetFiles(Resources(), "Strings*.resx")
            .Where(f => !string.Equals(Path.GetFileName(f), "Strings.resx", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f, StringComparer.Ordinal)
            .Select(f => (Path.GetFileName(f)[8..^5], Keys(f)));

      private static Dictionary<string, string> Keys(string path)
         => XDocument.Load(path).Root.Elements("data")
            .ToDictionary(d => (string)d.Attribute("name"), d => d.Element("value")?.Value ?? "", StringComparer.Ordinal);
   }
}
