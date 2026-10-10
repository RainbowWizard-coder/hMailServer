// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The quarantine digest's two server-wide settings - the address the release
   /// links point at and how long a link stays usable - written as one from the
   /// digest settings dialog, which writes with itself open and stays open on a
   /// refusal (review finding F209). A refusal leaves the server holding what it
   /// held: the hours are checked against the server's own bounds before anything
   /// is written, and where the server refuses the hours after it took the
   /// address, the address it held is written back. The address went first and
   /// the hours after it, unchecked, so that hours of 0 or 1000 were refused on
   /// their field after the new address had been stored, and Cancel then closed a
   /// dialog that read as "nothing changed" over an address that had. Without
   /// WPF, so the tests can hold the order.
   /// </summary>
   public static class QuarantineDigestSettingsWrite
   {
      /// <summary>The fewest hours a release link lives, as the server holds them (InterfaceSettings::put_QuarantineDigestLinkHours).</summary>
      public const int MinLinkHours = 1;

      /// <summary>The most hours a release link lives, as the server holds them: thirty days.</summary>
      public const int MaxLinkHours = 720;

      /// <summary>What a write ended in.</summary>
      public enum Outcome
      {
         /// <summary>Both values are the server's.</summary>
         Saved,

         /// <summary>The hours are outside the server's bounds; nothing was written.</summary>
         LinkHoursOutOfRange,

         /// <summary>The server refused the address; nothing was written.</summary>
         AddressRefused,

         /// <summary>The server refused the hours; the address it held was written back.</summary>
         LinkHoursRefused
      }

      /// <summary>Whether the server keeps a release link usable for this many hours.</summary>
      public static bool LinkHoursInRange(int hours) => hours >= MinLinkHours && hours <= MaxLinkHours;

      /// <summary>
      /// Checks the hours, then writes the address and the hours, in that order -
      /// the address is what the server refuses most, an address that is no
      /// absolute http or https URL - and, where the hours are refused after the
      /// address was taken, writes <paramref name="addressBefore"/> back.
      /// <paramref name="error"/> is the server's refusal, or null.
      /// </summary>
      public static Outcome Write(string address, int hours, string addressBefore,
                                  Action<string> writeAddress, Action<int> writeHours, out Exception error)
      {
         error = null;
         if (!LinkHoursInRange(hours))
            return Outcome.LinkHoursOutOfRange;

         try
         {
            writeAddress(address);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            error = ex;
            return Outcome.AddressRefused;
         }

         try
         {
            writeHours(hours);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            error = ex;
            if (!string.Equals(address, addressBefore, StringComparison.Ordinal))
            {
               try
               {
                  writeAddress(addressBefore);
               }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
               {
                  /* Deliberately ignored: the hours' refusal is what is said, and a link that fails between the two writes fails this one too, and the dialog reads the address the server holds the next time it opens. */
               }
            }

            return Outcome.LinkHoursRefused;
         }

         return Outcome.Saved;
      }
   }
}
