// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The server's refusals the Control Panel shows in the reader's language.
   ///
   /// A sentence here is only translated while it is character for character what
   /// the server writes: <c>L(message)</c> finds a catalogue key or returns the
   /// English. So the first test reads the server's source, where a reworded
   /// sentence would otherwise pass every build and quietly show English to a
   /// Japanese administrator - and the second holds every complete language to
   /// having translated it, since a key with an empty translation is English too.
   /// </summary>
   public class ServerSentencesTests
   {
      private static readonly string[] Sentences =
      {
         ServerSentences.PasswordBreached,
         ServerSentences.PasswordBreachCheckUnavailable
      };

      private static readonly string[] Complete =
      {
         "cs", "da", "de", "es", "fi", "fr", "it", "ja", "nb", "nl", "pl", "pt-BR", "ru", "sv", "tr", "uk", "zh-Hans"
      };

      [Fact]
      public void EverySentence_IsWrittenByTheServerWordForWord()
      {
         string source = File.ReadAllText(ServerFile("PasswordBreachCheck.cpp"));

         foreach (string sentence in Sentences)
         {
            Assert.True(source.Contains("L\"" + sentence + "\"", StringComparison.Ordinal),
               "PasswordBreachCheck.cpp no longer writes, word for word: " + sentence);
         }
      }

      [Fact]
      public void EveryCompleteLanguage_TranslatesEverySentence()
      {
         string resources = ResourcesDirectory();
         Assert.NotNull(resources);

         foreach (string tag in Complete)
         {
            Dictionary<string, string> catalogue = ReadResx(Path.Join(resources, "Strings." + tag + ".resx"));
            foreach (string sentence in Sentences)
            {
               Assert.True(catalogue.TryGetValue(sentence, out string translated) && !string.IsNullOrWhiteSpace(translated),
                  "Strings." + tag + ".resx has no translation of: " + sentence);
               Assert.NotEqual(sentence, translated);
            }
         }
      }

      [Fact]
      public void MessageOf_ReachesTheServersWordsThroughReflection()
      {
         var server = new COMException(ServerSentences.PasswordBreached);
         var wrapped = new TargetInvocationException(new TargetInvocationException(server));

         Assert.Equal(ServerSentences.PasswordBreached, ServerSentences.MessageOf(wrapped));
         Assert.Equal("plain", ServerSentences.MessageOf(new InvalidOperationException("plain")));
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
         return null;
      }

      private static string ServerFile(string name)
      {
         string dir = AppContext.BaseDirectory;
         for (int i = 0; i < 10 && dir != null; i++)
         {
            string candidate = Path.Join(dir, "Server", "Common", "Util", name);
            if (File.Exists(candidate))
               return candidate;
            dir = Path.GetDirectoryName(dir);
         }
         throw new FileNotFoundException("The server source was not found above the test binary.", name);
      }
   }
}
