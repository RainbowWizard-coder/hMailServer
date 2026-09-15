// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    Just enough JSON to read a document the server wrote: objects as
   ///    dictionaries, arrays as lists, strings, numbers as doubles, true, false
   ///    and null.
   ///
   ///    Written for the DKIM key rotation's status document, which nests three
   ///    levels deep, and kept here rather than taken from System.Text.Json because
   ///    this project is .NET Framework without binding redirects, where that
   ///    package's dependencies are a load failure waiting for the first fixture to
   ///    touch it - and the Linux project links the same file.
   /// </summary>
   public static class MiniJson
   {
      public static object Parse(string text)
      {
         int position = 0;
         object value = ReadValue(text, ref position);
         SkipWhitespace(text, ref position);
         if (position != text.Length)
            throw new FormatException("Unexpected text after the JSON value at " + position + ": " + text);
         return value;
      }

      public static Dictionary<string, object> Object(string text)
      {
         var value = Parse(text) as Dictionary<string, object>;
         if (value == null)
            throw new FormatException("Not a JSON object: " + text);
         return value;
      }

      /// <summary>The member at a dotted path ("next.check.status"), or null when any step is missing or null.</summary>
      public static object At(Dictionary<string, object> root, string path)
      {
         object current = root;
         foreach (string step in path.Split('.'))
         {
            var dictionary = current as Dictionary<string, object>;
            if (dictionary == null || !dictionary.TryGetValue(step, out current))
               return null;
         }
         return current;
      }

      public static string Text(Dictionary<string, object> root, string path)
      {
         return At(root, path) as string;
      }

      public static long Number(Dictionary<string, object> root, string path)
      {
         object value = At(root, path);
         return value is double ? (long) (double) value : 0;
      }

      public static bool Flag(Dictionary<string, object> root, string path)
      {
         object value = At(root, path);
         return value is bool && (bool) value;
      }

      public static List<object> List(Dictionary<string, object> root, string path)
      {
         return At(root, path) as List<object> ?? new List<object>();
      }

      private static void SkipWhitespace(string text, ref int position)
      {
         while (position < text.Length && char.IsWhiteSpace(text[position]))
            position++;
      }

      private static object ReadValue(string text, ref int position)
      {
         SkipWhitespace(text, ref position);
         if (position >= text.Length)
            throw new FormatException("The JSON ended early: " + text);

         char c = text[position];
         if (c == '{')
            return ReadObject(text, ref position);
         if (c == '[')
            return ReadArray(text, ref position);
         if (c == '"')
            return ReadString(text, ref position);
         if (string.CompareOrdinal(text, position, "true", 0, 4) == 0)
         {
            position += 4;
            return true;
         }
         if (string.CompareOrdinal(text, position, "false", 0, 5) == 0)
         {
            position += 5;
            return false;
         }
         if (string.CompareOrdinal(text, position, "null", 0, 4) == 0)
         {
            position += 4;
            return null;
         }

         int start = position;
         while (position < text.Length && "+-0123456789.eE".IndexOf(text[position]) >= 0)
            position++;
         if (start == position)
            throw new FormatException("Unexpected character '" + c + "' at " + start + ": " + text);
         return double.Parse(text.Substring(start, position - start), NumberStyles.Float, CultureInfo.InvariantCulture);
      }

      private static Dictionary<string, object> ReadObject(string text, ref int position)
      {
         var result = new Dictionary<string, object>();
         position++;
         SkipWhitespace(text, ref position);
         if (position < text.Length && text[position] == '}')
         {
            position++;
            return result;
         }

         while (true)
         {
            SkipWhitespace(text, ref position);
            string key = ReadString(text, ref position);
            SkipWhitespace(text, ref position);
            if (position >= text.Length || text[position] != ':')
               throw new FormatException("Expected ':' at " + position + ": " + text);
            position++;
            result[key] = ReadValue(text, ref position);
            SkipWhitespace(text, ref position);
            if (position < text.Length && text[position] == ',')
            {
               position++;
               continue;
            }
            if (position < text.Length && text[position] == '}')
            {
               position++;
               return result;
            }
            throw new FormatException("Expected ',' or '}' at " + position + ": " + text);
         }
      }

      private static List<object> ReadArray(string text, ref int position)
      {
         var result = new List<object>();
         position++;
         SkipWhitespace(text, ref position);
         if (position < text.Length && text[position] == ']')
         {
            position++;
            return result;
         }

         while (true)
         {
            result.Add(ReadValue(text, ref position));
            SkipWhitespace(text, ref position);
            if (position < text.Length && text[position] == ',')
            {
               position++;
               continue;
            }
            if (position < text.Length && text[position] == ']')
            {
               position++;
               return result;
            }
            throw new FormatException("Expected ',' or ']' at " + position + ": " + text);
         }
      }

      private static string ReadString(string text, ref int position)
      {
         if (position >= text.Length || text[position] != '"')
            throw new FormatException("Expected a string at " + position + ": " + text);

         position++;
         var builder = new StringBuilder();
         while (position < text.Length)
         {
            char c = text[position++];
            if (c == '"')
               return builder.ToString();
            if (c != '\\')
            {
               builder.Append(c);
               continue;
            }

            char escape = text[position++];
            switch (escape)
            {
               case 'n': builder.Append('\n'); break;
               case 'r': builder.Append('\r'); break;
               case 't': builder.Append('\t'); break;
               case 'b': builder.Append('\b'); break;
               case 'f': builder.Append('\f'); break;
               case 'u':
                  builder.Append((char) int.Parse(text.Substring(position, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
                  position += 4;
                  break;
               default: builder.Append(escape); break;
            }
         }

         throw new FormatException("The JSON string never ended: " + text);
      }
   }
}
