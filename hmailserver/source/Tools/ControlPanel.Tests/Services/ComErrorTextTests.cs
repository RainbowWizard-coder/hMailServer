// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What a failed call to the server was, and the level a page shows it at. A
   /// lost link is one event: the shell's pill calls it Critical, so every page
   /// says it at Critical; a server that answered and could not give a value is a
   /// Warning. Before, the same lost link was a Warning on the setup checklist and
   /// DNS records, Critical on Setting profiles and a grey caption on the Dashboard.
   /// </summary>
   public class ComErrorTextTests
   {
      [Theory]
      [InlineData(0x800706BAu)] // RPC_S_SERVER_UNAVAILABLE
      [InlineData(0x800706BEu)] // RPC_S_CALL_FAILED
      [InlineData(0x800706BFu)] // RPC_S_CALL_FAILED_DNE
      [InlineData(0x800706B5u)] // RPC_S_UNKNOWN_IF
      [InlineData(0x80010108u)] // RPC_E_DISCONNECTED
      [InlineData(0x80010105u)] // RPC_E_SERVERFAULT
      [InlineData(0x800401FDu)] // CO_E_OBJNOTCONNECTED
      [InlineData(0x80080005u)] // CO_E_SERVER_EXEC_FAILURE
      public void ALostLink_IsATransportFailure_AndCritical(uint code)
      {
         var lost = new COMException("The link went.", unchecked((int)code));

         Assert.True(ComErrorText.IsTransportFailure(lost));
         Assert.Equal(StatusLevel.Critical, ComErrorText.LevelFor(lost));

         // Wrapped, as a call made by reflection wraps it.
         var wrapped = new System.Reflection.TargetInvocationException(lost);
         Assert.True(ComErrorText.IsTransportFailure(wrapped));
         Assert.Equal(StatusLevel.Critical, ComErrorText.LevelFor(wrapped));
      }

      [Fact]
      public void AReleasedObject_IsATransportFailure()
      {
         Assert.True(ComErrorText.IsTransportFailure(new InvalidComObjectException("released")));
      }

      [Theory]
      [InlineData(0x80004005u)] // E_FAIL: the server refused, the link is there
      [InlineData(0x80020006u)] // DISP_E_UNKNOWNNAME: an older server
      [InlineData(0x80070005u)] // E_ACCESSDENIED
      public void AServerThatAnswered_IsAWarning(uint code)
      {
         var refused = new COMException("The server said no.", unchecked((int)code));

         Assert.False(ComErrorText.IsTransportFailure(refused));
         Assert.Equal(StatusLevel.Warning, ComErrorText.LevelFor(refused));
      }

      [Fact]
      public void AnAnswerThatCouldNotBeRead_IsAWarning()
      {
         Assert.Equal(StatusLevel.Warning, ComErrorText.LevelFor(new FormatException("not a number")));
         Assert.False(ComErrorText.IsTransportFailure(null));
      }

      [Fact]
      public void ACountedPage_TakesTheLevelOfWhetherTheLinkWent()
      {
         Assert.Equal(StatusLevel.Critical, ComErrorText.LevelFor(linkLost: true));
         Assert.Equal(StatusLevel.Warning, ComErrorText.LevelFor(linkLost: false));
      }

      // ---- what is said for a failure that is not the server's own words ----------

      /// <summary>The failures a test names, made here: a theory's data stays strings.</summary>
      private static Exception Make(string kind) => kind switch
      {
         "format" => new FormatException("The input string 'max_message_size_kb' was not in a correct format."),
         "overflow" => new OverflowException("Value was either too large or too small for an Int32."),
         "cast" => new InvalidCastException("Unable to cast object of type 'System.String' to type 'System.Int32'."),
         "key" => new System.Collections.Generic.KeyNotFoundException("The given key 'samples' was not present in the dictionary."),
         "json" => Caught(() => System.Text.Json.JsonDocument.Parse("<html>")),
         "json-kind" => Caught(() => System.Text.Json.JsonDocument.Parse("1").RootElement.GetString()),
         "binder-type" => new Microsoft.CSharp.RuntimeBinder.RuntimeBinderException("Cannot convert type 'int' to 'string'"),
         "wrapped-format" => new System.Reflection.TargetInvocationException(new FormatException("wrapped")),
         "unknown-name" => new COMException("Unknown name.", unchecked((int)0x80020006)),
         "member-not-found" => new COMException("Member not found.", unchecked((int)0x80020003)),
         "missing-method" => new MissingMethodException("Method 'GreylistingBypassDkim' not found."),
         "binder-definition" => new Microsoft.CSharp.RuntimeBinder.RuntimeBinderException("'System.__ComObject' does not contain a definition for 'QuarantineEnabled'"),
         "wrapped-unknown-name" => new System.Reflection.TargetInvocationException(new COMException("Unknown name.", unchecked((int)0x80020006))),
         _ => throw new ArgumentOutOfRangeException(nameof(kind), kind, null)
      };

      /// <summary>
      /// An answer this Control Panel could not read is said as such, with the
      /// runtime's text in brackets - shown bare it was "The input string ... was
      /// not in a correct format." in bold on the Levels tab and a JSON parser's
      /// position as a statistic.
      /// </summary>
      [Theory]
      [InlineData("format")]
      [InlineData("overflow")]
      [InlineData("cast")]
      [InlineData("key")]
      [InlineData("json")]
      [InlineData("json-kind")]
      [InlineData("binder-type")]
      [InlineData("wrapped-format")]
      public void AnAnswerThatCouldNotBeRead_IsSaidAsSuch(string kind)
      {
         Exception ex = Make(kind);
         Assert.True(ComErrorText.IsUnreadableAnswer(ex), ex.GetType().Name + " is not taken for an unreadable answer.");
         Assert.False(ComErrorText.IsMissingMember(ex));
         Assert.Equal("the server answered something this Control Panel could not read (detail)", ComErrorText.NotTheServersWords(ex, "detail"));
      }

      /// <summary>A member the server does not have is an older server, and said so.</summary>
      [Theory]
      [InlineData("unknown-name")]
      [InlineData("member-not-found")]
      [InlineData("missing-method")]
      [InlineData("binder-definition")]
      [InlineData("wrapped-unknown-name")]
      public void AMemberTheServerDoesNotHave_IsSaidToBeAnOlderServer(string kind)
      {
         Exception ex = Make(kind);
         Assert.True(ComErrorText.IsMissingMember(ex));
         Assert.Equal("the server has no such setting or action - it may be older than this Control Panel (detail)", ComErrorText.NotTheServersWords(ex, "detail"));
      }

      /// <summary>
      /// The server's own refusals, and anything else, are left to their own words:
      /// the server's sentence is the sentence.
      /// </summary>
      [Fact]
      public void TheServersOwnWords_AreLeftAsTheyAre()
      {
         Assert.Null(ComErrorText.NotTheServersWords(new COMException("The domain name is already in use.", unchecked((int)0x80004005)), "x"));
         Assert.Null(ComErrorText.NotTheServersWords(new InvalidOperationException("Settings are stored in the server's database."), "x"));
         Assert.Null(ComErrorText.NotTheServersWords(new UnauthorizedAccessException("Access is denied."), "x"));
         Assert.Null(ComErrorText.NotTheServersWords(null, "x"));
      }

      /// <summary>The code a failure came with stays in reach for a tool tip: type, HRESULT and text, innermost.</summary>
      [Fact]
      public void TheTechnicalDetail_IsTheInnermostTypeCodeAndText()
      {
         var lost = new COMException("The RPC server is unavailable.", unchecked((int)0x800706BA));
         Assert.Equal("COMException 0x800706BA: The RPC server is unavailable.",
            ComErrorText.TechnicalDetail(new System.Reflection.TargetInvocationException(lost)));
         Assert.Null(ComErrorText.TechnicalDetail(null));
      }

      private static Exception Caught(Action action)
      {
         try
         {
            action();
         }
         catch (Exception ex)
         {
            return ex;
         }

         throw new InvalidOperationException("The action did not throw.");
      }
   }
}
