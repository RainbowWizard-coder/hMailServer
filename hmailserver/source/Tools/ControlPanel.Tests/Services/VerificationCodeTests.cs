// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F240: the server-enforced second factor's code box takes digits
   /// alone. A space typed got in - a TextBox takes the space bar itself, without the
   /// PreviewTextInput the box's filter listened to - so a code typed as the
   /// authenticator shows it, "123 456", became "123 45" in the box's six characters
   /// and did not match; and what a paste put in the box had no test.
   /// Services/VerificationCode is the rule; the dialog's wiring is read from its
   /// source in TwoFactorStateTests.
   /// </summary>
   public class VerificationCodeTests
   {
      [Theory]
      [InlineData("123 456", "123456")]       // as authenticator apps copy a code
      [InlineData(" 123456\r\n", "123456")]   // with the line it was copied from
      [InlineData("1234567", "123456")]       // no more than the box holds
      [InlineData("123\t456", "123456")]
      [InlineData("42", "42")]
      public void APasteOfDigitsAndSpaces_IsItsDigits_UpToTheBoxsLength(string pasted, string expected)
         => Assert.Equal(expected, VerificationCode.FromPaste(pasted, 6));

      [Theory]
      [InlineData("12a456")]
      [InlineData("123-456")]
      [InlineData("１２３４５６")]   // full-width digits are not the ASCII the check counts
      [InlineData("")]
      [InlineData("   ")]
      [InlineData(null)]
      public void APasteWithAnythingElse_OrNoDigit_IsRefused(string pasted)
         => Assert.Null(VerificationCode.FromPaste(pasted, 6));

      [Theory]
      [InlineData("1", true)]
      [InlineData("09", true)]
      [InlineData(" ", false)]
      [InlineData("a", false)]
      [InlineData("", false)]
      public void TypedText_GoesInOnlyAsDigits(string typed, bool taken)
         => Assert.Equal(taken, VerificationCode.IsTyped(typed));

      [Theory]
      [InlineData("123 45", "12345")]
      [InlineData(" 123456 ", "123456")]
      [InlineData("", "")]
      [InlineData(null, "")]
      public void TheCodeChecked_IsTheBoxsDigits(string box, string checkedCode)
         => Assert.Equal(checkedCode, VerificationCode.Digits(box));
   }
}
