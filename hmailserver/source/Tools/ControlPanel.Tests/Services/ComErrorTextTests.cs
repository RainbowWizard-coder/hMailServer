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
   }
}
