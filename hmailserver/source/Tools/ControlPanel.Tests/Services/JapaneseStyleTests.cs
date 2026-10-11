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
   /// The Japanese catalogue's style note (the comment at the head of Strings.ja.resx),
   /// written for review finding F302: one word for each thing, where the catalogue
   /// said キー and 鍵 for a key, 自動ブロック in titles and BAN, 禁止 and 遮断 in the
   /// text for an auto-ban, 差出人 and 送信者 and 迷惑メール and スパム in one paragraph,
   /// ログ for the Logging page as for a log, 開始済み for when the service started,
   /// 日和見的 for opportunistic encryption, 宛先 and 送信先 for a route's target, and
   /// fifteen full-width parentheses among 1,555 half-width ones. The space before a
   /// key's suffix, "作成 (_D)", is build/check-mnemonics.py's to refuse.
   /// </summary>
   public class JapaneseStyleTests
   {
      private static readonly Dictionary<string, string> Japanese = ReadResx("Strings.ja.resx");

      [Fact]
      public void AKeyIsKii()
      {
         Assert.True(Japanese.Count > 5000, "The catalogue was not read.");
         List<string> found = Japanese.Where(pair => pair.Value.Contains('鍵', StringComparison.Ordinal)).Select(Show).ToList();
         Assert.True(found.Count == 0, "鍵 for a key, where the catalogue says キー:" + Environment.NewLine + string.Join(Environment.NewLine, found));
         Assert.Contains("秘密キー ファイル", Japanese["Browse for the private key file"], StringComparison.Ordinal);
      }

      [Fact]
      public void ParenthesesAreHalfWidth()
      {
         List<string> found = Japanese.Where(pair => pair.Value.IndexOfAny(new[] { '（', '）' }) >= 0).Select(Show).ToList();
         Assert.True(found.Count == 0, "Full-width parentheses:" + Environment.NewLine + string.Join(Environment.NewLine, found));
      }

      [Fact]
      public void AnAutoBanIsJidouBan()
      {
         Assert.Equal("自動 BAN", Japanese["Auto-ban"]);
         var ban = new Regex(@"\bban(s|ned|ning)?\b|auto-ban", RegexOptions.IgnoreCase);
         List<string> found = Japanese
            .Where(pair => pair.Value.Contains("自動ブロック", StringComparison.Ordinal)
                           || (ban.IsMatch(pair.Key) && Regex.IsMatch(pair.Value, "禁止|遮断")))
            .Select(Show)
            .ToList();
         Assert.True(found.Count == 0, "An auto-ban is 自動 BAN and a ban BAN:" + Environment.NewLine + string.Join(Environment.NewLine, found));
         Assert.Equal("BAN 期間 (分)", Japanese["Ban duration (minutes)"]);
      }

      [Fact]
      public void TheSenderIsSoushinshaAndSpamIsSupamu()
      {
         // 差出人 is the From header alone; 迷惑メール the name of a mail client's button alone.
         List<string> found = Japanese
            .Where(pair => (pair.Value.Contains("差出人", StringComparison.Ordinal) && !Regex.IsMatch(pair.Key, @"\bFrom\b|_From\b"))
                           || pair.Value.Replace("「迷惑メール」", "", StringComparison.Ordinal).Contains("迷惑メール", StringComparison.Ordinal))
            .Select(Show)
            .ToList();
         Assert.True(found.Count == 0, "差出人 for a sender or 迷惑メール for spam:" + Environment.NewLine + string.Join(Environment.NewLine, found));
         Assert.Equal("受信者に表示される送信者名", Japanese["Sender name, as recipients see it"]);
      }

      [Fact]
      public void TheNamedWordsAreTheStyleNotes()
      {
         Assert.Equal("ログ設定", Japanese["Logging"]);
         Assert.Equal("開始日時", Japanese["Started"]);
         Assert.Equal("日和見暗号化", Japanese["Opportunistic"]);
         Assert.Equal("送信先 SMTP ホスト(_T)", Japanese["_Target SMTP host"]);
         string head = File.ReadAllText(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.ja.resx")));
         Assert.Contains("F302", head[..head.IndexOf("<resheader", StringComparison.Ordinal)], StringComparison.Ordinal);
      }

      /// <summary>
      /// A key's suffix ends its caption, after a parenthesis the caption ends with -
      /// 宛先フォルダー (サーバー上)(_F), as 95 captions wrote it - where ten wrote it before
      /// the parenthesis, as the note's first wording had it. A parenthesis that holds an
      /// underscore of its own (%MACRO_MAILBOX%) would keep the key before it, since WPF
      /// takes the first underscore for the key; the warning text's caption held one until
      /// its macros moved to a hint below the box, and ends with its key as the rest do.
      /// </summary>
      [Fact]
      public void AKeysSuffixEndsTheCaption()
      {
         var before = new Regex(@"\(_[A-Z0-9]\) \([^)_]*\)$");
         List<string> found = Japanese.Where(pair => before.IsMatch(pair.Value)).Select(Show).ToList();
         Assert.True(found.Count == 0, "A key's suffix before the caption's closing parenthesis:" + Environment.NewLine + string.Join(Environment.NewLine, found));
         Assert.Equal("宛先フォルダー (サーバー上)(_F)", Japanese["Destination _folder (on the server)"]);
         Assert.Equal("警告の本文 (空 = サーバーメッセージ)(_X)", Japanese["Warning te_xt (empty = the server message)"]);
      }

      private static string Show(KeyValuePair<string, string> pair) => pair.Key + " = " + pair.Value;

      private static Dictionary<string, string> ReadResx(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", file));
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }
   }
}
