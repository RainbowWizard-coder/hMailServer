// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Runtime.InteropServices;
using Xunit;
using QuickProgram = DBSetupQuick.Program;

namespace DBSetup.Tests
{
   /// <summary>
   ///    DBSetupQuick, the installer's database step, when the hMailServer service cannot be
   ///    reached through COM. Up to 6.3.5 the activation was outside any handler: the process
   ///    died of the unhandled COMException with 0xE0434352 (-532462766), which the
   ///    installer read as a database step that may have changed the database, and what had
   ///    happened was written nowhere (the installer smoke VM, 2 October 2026). From 6.3.6 the
   ///    activation and the one question asked before anything that changes a database are
   ///    reached through <see cref="QuickProgram.Reach{T}" />: a failure is said - with the
   ///    COM error's HRESULT, and that nothing was changed - into the report the installer
   ///    names (report:PATH) and copies into its log, and the process exits with
   ///    <see cref="QuickProgram.ExitServiceNotReached" />, which the installer reads as "the
   ///    database step could not run". Main itself activates the real service, which a test
   ///    on a machine with hMailServer installed must not; it is these two functions and a
   ///    COM activation's own error that make its answer.
   /// </summary>
   public sealed class QuickServiceNotReachedTests
   {
      // CO_E_SERVER_EXEC_FAILURE: what CoCreateInstance answers when the local server - the
      // hMailServer service - does not start or does not register its class in time.
      private const int ServerExecFailure = unchecked((int)0x80080005);

      [Fact]
      public void AnActivationThatFailsIsSaidAndReachesNothing()
      {
         bool asked = false;
         bool reached = QuickProgram.Reach<object>(() => throw new COMException("Server execution failed", ServerExecFailure),
                                                   _ => { asked = true; return true; },
                                                   out object application, out bool exists, out string why);

         Assert.False(reached);
         Assert.Null(application);
         Assert.False(exists);
         Assert.False(asked, "The database was asked about through a service that was not reached.");
         Assert.StartsWith("DBSetupQuick.exe could not reach the hMailServer service through COM: Server execution failed (HRESULT 0x80080005).", why);
         Assert.Contains("DBUpdater.exe was not started: neither changed the database", why);
      }

      [Fact]
      public void AServiceThatDoesNotSayWhetherADatabaseIsConfiguredIsSaid()
      {
         bool reached = QuickProgram.Reach<object>(() => new object(),
                                                   _ => throw new COMException("The remote procedure call failed.", unchecked((int)0x800706BE)),
                                                   out object application, out bool exists, out string why);

         Assert.False(reached);
         Assert.Null(application);
         Assert.StartsWith("The hMailServer service did not say whether a database is configured: The remote procedure call failed (HRESULT 0x800706BE).", why);
      }

      [Fact]
      public void AnAccessDeniedActivationIsSaidWithItsHresult()
      {
         QuickProgram.Reach<object>(() => throw new UnauthorizedAccessException("Access is denied."), _ => true, out _, out _, out string why);

         Assert.Contains("Access is denied (HRESULT 0x80070005)", why);
      }

      [Theory]
      [InlineData(true)]
      [InlineData(false)]
      public void AReachedServiceAnswersWhetherADatabaseIsConfigured(bool configured)
      {
         object service = new object();
         bool reached = QuickProgram.Reach(() => service, _ => configured, out object application, out bool exists, out string why);

         Assert.True(reached);
         Assert.Same(service, application);
         Assert.Equal(configured, exists);
         Assert.Null(why);
      }

      [Fact]
      public void AFatalExceptionIsNotTakenForAnUnreachableService()
      {
         Assert.Throws<OutOfMemoryException>(() => QuickProgram.Reach<object>(() => throw new OutOfMemoryException(), _ => true, out _, out _, out _));
      }

      [Fact]
      public void TheExitCodeIsNoneDbUpdaterAnswers()
      {
         // DBUpdater's own: 0 done, 1 failed, 2 its configuration, 3 no sign-in - which
         // DBSetupQuick passes on - and the installer's reading of 4 depends on its being
         // none of them.
         Assert.Equal(4, QuickProgram.ExitServiceNotReached);
      }

      [Fact]
      public void WhatIsSaidGoesIntoTheInstallersReport()
      {
         string directory = Path.Combine(Path.GetTempPath(), "dbsetupquick-report-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(directory);
         try
         {
            string report = Path.Combine(directory, "dbsetupquick.txt");
            QuickProgram.Say(report, "first line");
            QuickProgram.Say(report, "second line");

            Assert.Equal("first line" + Environment.NewLine + "second line" + Environment.NewLine, File.ReadAllText(report));

            // No report named, and one that cannot be written: said on standard error alone,
            // and nothing thrown.
            QuickProgram.Say(null, "nowhere");
            QuickProgram.Say(Path.Combine(directory, "missing", "x.txt"), "unwritable");
         }
         finally
         {
            Directory.Delete(directory, true);
         }
      }
   }
}
