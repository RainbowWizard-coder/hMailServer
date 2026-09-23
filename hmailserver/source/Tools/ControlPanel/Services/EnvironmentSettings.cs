// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The settings the server's environment sets (docs/EnvironmentConfiguration.md),
   /// in the words the pages show beside such a field and above the page.
   ///
   /// A container's entrypoint runs `hmailserver --apply-environment` at every
   /// start, which stores every HM_SETTING_&lt;key&gt; and HM_PROPERTY_&lt;name&gt;
   /// variable and records their names; the server then refuses a change to one of
   /// them from any interface, because the next start would undo it. This program
   /// learns the names from Settings.GetChangedFromDefault - the document it already
   /// asks for, whose environment array lists every managed setting as {variable,
   /// store, name, source} - and never a value. A field kept in the settings store
   /// is known by its key; a field of the COM settings tree is known when its
   /// property carries the hm_settings row's own name, which is how most of them are
   /// named, and every managed setting is named in the page's notice whatever its
   /// field is called. The server refuses the change in any case, with a sentence
   /// the page shows.
   ///
   /// Plain data and pure functions, no WPF and no COM, so ControlPanel.Tests can
   /// hold the words and the parse.
   /// </summary>
   public static class EnvironmentSettings
   {
      public const string Variable = "variable";   // no-loc
      public const string File = "file";           // no-loc
      public const string StoreIni = "ini";        // no-loc
      public const string StoreDatabase = "database"; // no-loc

      /// <summary>One setting the environment sets: never its value.</summary>
      public sealed class Entry
      {
         public string VariableName { get; init; }
         public string Store { get; init; }
         public string Name { get; init; }
         public string Source { get; init; }
      }

      /// <summary>
      /// The environment array of Settings.GetChangedFromDefault's document. Empty for
      /// null, blank, or a document from a server older than the array; a
      /// JsonException for text that is not a JSON object, which the caller treats as a
      /// failed read.
      /// </summary>
      public static IReadOnlyList<Entry> Parse(string changedDocument)
      {
         var entries = new List<Entry>();
         if (string.IsNullOrWhiteSpace(changedDocument))
            return entries;

         using JsonDocument document = JsonDocument.Parse(changedDocument);
         if (document.RootElement.ValueKind != JsonValueKind.Object)
            throw new JsonException("The document is not a JSON object."); // no-loc

         if (!document.RootElement.TryGetProperty("environment", out JsonElement list) || list.ValueKind != JsonValueKind.Array) // no-loc
            return entries;

         foreach (JsonElement item in list.EnumerateArray())
         {
            if (item.ValueKind != JsonValueKind.Object)
               continue;

            string Text(string member)
               => item.TryGetProperty(member, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

            string name = Text("name");       // no-loc
            string store = Text("store");     // no-loc
            if (string.IsNullOrEmpty(name) || (store != StoreIni && store != StoreDatabase))
               continue;

            entries.Add(new Entry
            {
               VariableName = Text("variable") ?? "", // no-loc
               Store = store,
               Name = name,
               Source = Text("source") == File ? File : Variable // no-loc
            });
         }

         return entries;
      }

      /// <summary>One setting whose value a file of the environment supplied: never its value.</summary>
      public sealed class Withheld
      {
         public string Store { get; init; }
         public string Name { get; init; }
      }

      /// <summary>
      /// The from_file array of Settings.GetChangedFromDefault's document: every
      /// setting whose value a file of the server's environment supplied - now, or
      /// before and not rewritten since - which the server withholds from every
      /// answer and this program does not show in a field. Empty for null, blank, or
      /// a document from a server older than the array; a JsonException for text
      /// that is not a JSON object.
      /// </summary>
      public static IReadOnlyList<Withheld> ParseFromFile(string changedDocument)
      {
         var entries = new List<Withheld>();
         if (string.IsNullOrWhiteSpace(changedDocument))
            return entries;

         using JsonDocument document = JsonDocument.Parse(changedDocument);
         if (document.RootElement.ValueKind != JsonValueKind.Object)
            throw new JsonException("The document is not a JSON object."); // no-loc

         if (!document.RootElement.TryGetProperty("from_file", out JsonElement list) || list.ValueKind != JsonValueKind.Array) // no-loc
            return entries;

         foreach (JsonElement item in list.EnumerateArray())
         {
            if (item.ValueKind != JsonValueKind.Object)
               continue;

            string Text(string member)
               => item.TryGetProperty(member, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

            string name = Text("name");       // no-loc
            string store = Text("store");     // no-loc
            if (string.IsNullOrEmpty(name) || (store != StoreIni && store != StoreDatabase))
               continue;

            entries.Add(new Withheld { Store = store, Name = name });
         }

         return entries;
      }

      /// <summary>Whether a setting is one of the withheld ones, matched without case.</summary>
      public static bool IsWithheld(IReadOnlyList<Withheld> entries, string store, string name)
         => entries != null && !string.IsNullOrEmpty(name) &&
            entries.Any(e => e.Store == store && string.Equals(e.Name, name, StringComparison.OrdinalIgnoreCase));

      /// <summary>
      /// What an empty field says when the value it would show came from a file of
      /// the server's environment: the field is left empty, and a save writes nothing
      /// unless something is typed.
      /// </summary>
      public static string WithheldPlaceholder()
         => L("Set from a file of the server's environment, so not shown. Type a new value to replace it.");

      /// <summary>
      /// The mark beside a field the environment sets: "Set by the environment", or
      /// "... from a file" for a value a Secret supplied. Null for anything else, so
      /// that an unmanaged field shows its own mark.
      /// </summary>
      public static string Caption(string source)
      {
         switch (source)
         {
            case Variable:
               return L("Set by the environment");
            case File:
               return L("Set by the environment, from a file");
            default:
               return null;
         }
      }

      /// <summary>
      /// The entry for a field, or null: a field kept in the settings store by its key,
      /// a field of the COM settings tree by the last part of its path when that is the
      /// property's own name. Matched without case, as the server matches both.
      /// </summary>
      public static Entry Find(IReadOnlyList<Entry> entries, string store, string name)
      {
         if (entries == null || string.IsNullOrEmpty(name))
            return null;

         return entries.FirstOrDefault(e => e.Store == store && string.Equals(e.Name, name, StringComparison.OrdinalIgnoreCase));
      }

      /// <summary>The property a COM settings path names: the part after the last dot.</summary>
      public static string PropertyOf(string comPath)
      {
         if (string.IsNullOrEmpty(comPath))
            return null;

         int dot = comPath.LastIndexOf('.');
         return dot >= 0 ? comPath.Substring(dot + 1) : comPath;
      }

      /// <summary>
      /// The sentence above a settings page when the environment sets anything, naming
      /// every managed setting by its variable - so that a field this program cannot tie
      /// to its setting by name is still named on the page. Null when there is none.
      /// </summary>
      public static string Notice(IReadOnlyList<Entry> entries)
      {
         if (entries == null || entries.Count == 0)
            return null;

         string names = string.Join(", ", entries.Select(e => string.IsNullOrEmpty(e.VariableName) ? e.Name : e.VariableName)); // no-loc
         return F("Set by this server's environment, which stores them again at every start, so a change made here is refused: {0}. Change the variable and restart to change one.", names);
      }
   }
}
