// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the server-enforced second factor's code box takes (Views/
   /// AdministratorTwoFactorDialog), without the window, so that VerificationCodeTests
   /// can hold it. A verification code is digits: typed, the box takes digits alone; a
   /// paste of digits and spaces - authenticator apps show and copy a code as "123 456"
   /// - is taken as its digits, up to the box's length, and a paste with anything else
   /// in it is refused; and the code checked is the box's digits, whatever else came
   /// to be in it. Review finding F240: a space typed got in, the box's six characters
   /// then held "123 45", and the check said that the code did not match.
   /// </summary>
   public static class VerificationCode
   {
      /// <summary>The digits of a text, in their order; anything else left out.</summary>
      public static string Digits(string text)
         => new string((text ?? "").Where(char.IsAsciiDigit).ToArray());

      /// <summary>Whether typed text may go in the box: one or more digits and nothing else.</summary>
      public static bool IsTyped(string text)
         => !string.IsNullOrEmpty(text) && text.All(char.IsAsciiDigit);

      /// <summary>
      /// What a paste puts in the box: its digits, the first <paramref name="max"/> of
      /// them, where it holds digits and white space alone; null, the paste refused,
      /// where it holds anything else or no digit at all.
      /// </summary>
      public static string FromPaste(string pasted, int max)
      {
         if (string.IsNullOrEmpty(pasted) || pasted.Any(c => !char.IsAsciiDigit(c) && !char.IsWhiteSpace(c)))
            return null;

         string digits = Digits(pasted);
         if (digits.Length == 0)
            return null;

         return max > 0 && digits.Length > max ? digits.Substring(0, max) : digits;
      }
   }
}
