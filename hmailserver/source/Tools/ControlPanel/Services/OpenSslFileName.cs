// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether the server, on this computer, refuses to hand a file's name to OpenSSL:
   /// the rule of FileUtilities::FileNameBytes, which FileUtilities::OpenSslPath holds
   /// every file the server opens with OpenSSL to on Windows - a TLS certificate's
   /// certificate and private key files, a port's client-certificate CA file,
   /// OAuth2PublicKeyFile, MessageStoreObjectCaFile, the ACME client's files.
   ///
   /// OpenSSL's fopen on Windows (crypto/o_fopen.c) reads a name as UTF-8 first and
   /// opens the file at that reading when there is one, falling back to the ANSI code
   /// page's reading only when there is none. So the server hands a name over in the
   /// code page, with no best-fit letter put in a character's place, and refuses one
   /// with a character the code page does not have, one whose bytes outside ASCII also
   /// read as UTF-8 (U+00C3 U+00A9 on code page 1252 are C3 A9, which is U+00E9, and a
   /// capital E with an acute accent before a closing quotation mark, U+00C9 U+2019, are
   /// C9 92, which is U+0252), and one with U+0000; under the UTF-8 code page
   /// (the system-wide "Beta: Use Unicode UTF-8" setting) it hands over the UTF-8 and
   /// refuses only U+0000 and half of a surrogate pair. A refused name is not read: the
   /// server logs HM7536 and goes on as if the file could not be loaded, so a port whose
   /// certificate it names does not start. The Control Panel's local checks read the
   /// same file by its wide name, which always works; this is what keeps them from
   /// calling such a file good. Only meaningful on the server's own machine, whose code
   /// page this is: the callers ask it where they read the files at all.
   /// </summary>
   public static partial class OpenSslFileName
   {
      /// <summary>The UTF-8 code page, CP_UTF8.</summary>
      public const uint Utf8CodePage = 65001;

      private const uint WcNoBestFitChars = 0x00000400;
      private const uint MbErrInvalidChars = 0x00000008;

      /// <summary>
      /// True when the server on this computer refuses <paramref name="path"/> (HM7536):
      /// the rule below under this computer's ANSI code page.
      /// </summary>
      public static bool IsRefusedHere(string path) => IsRefused(path, GetACP());

      /// <summary>
      /// True when FileUtilities::FileNameBytes refuses <paramref name="path"/> under the
      /// ANSI code page <paramref name="codePage"/>. An empty name is not refused, as it
      /// is not there: the callers say what an empty setting means.
      /// </summary>
      public static bool IsRefused(string path, uint codePage)
      {
         if (string.IsNullOrEmpty(path))
            return false;

         // A narrow call would cut the name short at U+0000.
         if (path.IndexOf('\0') >= 0)
            return true;

         // UTF-8 carries every character but half of a surrogate pair, which
         // Unicode::ToUtf8Exact refuses.
         if (codePage == Utf8CodePage)
            return HasLoneSurrogate_(path);

         int defaulted = 0;
         int size = WideCharToMultiByte(codePage, WcNoBestFitChars, path, path.Length, null, 0, IntPtr.Zero, ref defaulted);
         if (size <= 0 || defaulted != 0)
            return true;

         var narrow = new byte[size];
         if (WideCharToMultiByte(codePage, WcNoBestFitChars, path, path.Length, narrow, size, IntPtr.Zero, ref defaulted) != size ||
             defaulted != 0)
            return true;

         // Bytes outside ASCII that also read as UTF-8 name another file to OpenSSL.
         bool ascii = Array.TrueForAll(narrow, b => b < 0x80);
         return !ascii && MultiByteToWideChar(Utf8CodePage, MbErrInvalidChars, narrow, narrow.Length, IntPtr.Zero, 0) > 0;
      }

      /// <summary>
      /// What such a name is and does, and what to do about it: the sentence the local
      /// checks put after their own, which says what the file was for.
      /// </summary>
      public static string Explanation()
         => L("On this computer's code page its name holds a character the code page does not have, or characters whose bytes also read as another name in UTF-8, which OpenSSL would open instead, so the server does not hand it to OpenSSL (server error 7536). Give the file, and the folders above it, names in ASCII.");

      private static bool HasLoneSurrogate_(string path)
      {
         for (int i = 0; i < path.Length; i++)
         {
            if (char.IsHighSurrogate(path[i]) && i + 1 < path.Length && char.IsLowSurrogate(path[i + 1]))
               i++;
            else if (char.IsSurrogate(path[i]))
               return true;
         }

         return false;
      }

      [LibraryImport("kernel32.dll")]
      private static partial uint GetACP();

      [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16)]
      private static partial int WideCharToMultiByte(uint codePage, uint flags, string wide, int wideLength,
         byte[] narrow, int narrowLength, IntPtr defaultChar, ref int usedDefaultChar);

      // Only the length is asked for, so the wide buffer is always null.
      [LibraryImport("kernel32.dll")]
      private static partial int MultiByteToWideChar(uint codePage, uint flags, byte[] narrow, int narrowLength,
         IntPtr wide, int wideLength);
   }
}
