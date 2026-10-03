// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using Xunit;

namespace DBSetup.Tests
{
   /// <summary>
   ///    DBSetup finishing a new installation the installer did not finish: the mark the
   ///    installer leaves, and the steps run after the database, in the installer's order,
   ///    against a machine of the test's own (<see cref="INewInstallationHost" />), since the
   ///    real one is the hMailServer service and its verbs. Until 6.3.6 DBSetup run after a
   ///    refused database step set the database up and nothing else: the service stayed on
   ///    LocalSystem, with no self-signed certificate and no stored-secrets key (the
   ///    installer smoke VM, 2 October 2026). Negative control: with the move to the
   ///    virtual account left out of Finish, seven of these fail (3 October 2026).
   /// </summary>
   public sealed class NewInstallationTests
   {
      private sealed class Machine : INewInstallationHost
      {
         public readonly List<string> Calls = new List<string>();
         public readonly Dictionary<string, int> Answers = new Dictionary<string, int>();
         public bool Stops = true;
         public bool Starts = true;
         public bool OpensDatabase = true;

         public int RunServer(string[] arguments, out string said)
         {
            string call = string.Join(" ", arguments);
            Calls.Add(call);
            said = "said by " + call;
            return Answers.TryGetValue(call, out int code) ? code : 0;
         }

         public bool StopService(out string why)
         {
            Calls.Add("stop");
            why = Stops ? null : "it would not stop";
            return Stops;
         }

         public bool StartService(out string why)
         {
            Calls.Add("start");
            why = Starts ? null : "it would not start";
            return Starts;
         }

         public bool ReadVersions(out int current, out int required, out string why)
         {
            Calls.Add("versions");
            current = OpensDatabase ? 6108 : 0;
            required = 6108;
            why = OpensDatabase ? null : "The connection to the database is not available.";
            return OpensDatabase;
         }
      }

      private static bool Finish(Machine machine, string account, string profile, bool attended, List<string> said, List<string> warned, out bool ran, bool created = true)
      {
         return NewInstallation.Finish(machine, PendingNewInstallation.FromValues(account, profile), attended, created, said.Add, warned.Add, out ran);
      }

      [Fact]
      public void TheMarkIsReadAsTheInstallerWritesIt()
      {
         Assert.Null(PendingNewInstallation.FromValues(null, "office"));
         Assert.Null(PendingNewInstallation.FromValues("  ", null));

         PendingNewInstallation virtualAccount = PendingNewInstallation.FromValues(" Virtual ", "Office");
         Assert.Equal("virtual", virtualAccount.Account);
         Assert.True(virtualAccount.MovesToVirtualAccount);
         Assert.Equal("office", virtualAccount.Profile);

         PendingNewInstallation localSystem = PendingNewInstallation.FromValues("localsystem", "");
         Assert.False(localSystem.MovesToVirtualAccount);
         Assert.Null(localSystem.Profile);

         Assert.Null(PendingNewInstallation.FromValues("virtual", "office --confirm").Profile);
      }

      [Fact]
      public void AVirtualAccountInstallationIsFinishedInTheInstallersOrder()
      {
         var machine = new Machine();
         var said = new List<string>();
         var warned = new List<string>();

         Assert.True(Finish(machine, "virtual", null, false, said, warned, out bool ran));
         Assert.True(ran);
         Assert.Empty(warned);
         Assert.Equal(new[]
         {
            "stop",
            "--apply-new-install-defaults",
            "--create-stored-secrets-key",
            "--grant-database-login",
            "/GrantServiceAccess",
            "--service-account virtual /Register",
            "start",
            "versions"
         }, machine.Calls);
      }

      [Fact]
      public void AttendedTheGrantAsksAndAProfileIsApplied()
      {
         var machine = new Machine();
         var said = new List<string>();
         var warned = new List<string>();

         Assert.True(Finish(machine, "virtual", "gateway", true, said, warned, out _));
         Assert.Contains("--apply-profile gateway --once --if-new --confirm", machine.Calls);
         Assert.Contains("--confirm /GrantServiceAccess", machine.Calls);
         Assert.True(machine.Calls.IndexOf("--apply-profile gateway --once --if-new --confirm") < machine.Calls.IndexOf("--create-stored-secrets-key"));
      }

      [Fact]
      public void LocalSystemIsKeptAsChosen()
      {
         var machine = new Machine();
         var said = new List<string>();
         var warned = new List<string>();

         Assert.True(Finish(machine, "localsystem", null, false, said, warned, out _));
         Assert.Equal(new[] { "stop", "--apply-new-install-defaults", "--create-stored-secrets-key", "start" }, machine.Calls);
      }

      [Fact]
      public void AnAttachedDatabaseKeepsWhatItHasAndThatIsNoFailure()
      {
         var machine = new Machine();
         machine.Answers["--apply-new-install-defaults"] = 5;
         var said = new List<string>();
         var warned = new List<string>();

         Assert.True(Finish(machine, "virtual", null, false, said, warned, out _));
         Assert.Empty(warned);
      }

      [Fact]
      public void NoLoginOnSqlServerMeansNoMove()
      {
         var machine = new Machine();
         machine.Answers["--grant-database-login"] = 1;
         var said = new List<string>();
         var warned = new List<string>();

         Assert.False(Finish(machine, "virtual", null, false, said, warned, out bool ran));
         Assert.True(ran);
         Assert.DoesNotContain("/GrantServiceAccess", machine.Calls);
         Assert.DoesNotContain("--service-account virtual /Register", machine.Calls);
         Assert.Contains("start", machine.Calls);
         Assert.DoesNotContain("versions", machine.Calls);
         Assert.Contains(warned, line => line.Contains("said by --grant-database-login"));
         Assert.Contains(warned, line => line.Contains("runs as LocalSystem"));
      }

      [Fact]
      public void OnADatabaseThatExistedTheMoveIsMadeAndItsOpeningDecides()
      {
         var machine = new Machine();
         machine.Answers["--grant-database-login"] = 2;
         var said = new List<string>();
         var warned = new List<string>();

         Assert.True(Finish(machine, "virtual", null, false, said, warned, out _, created: false));
         Assert.Contains("--service-account virtual /Register", machine.Calls);
         Assert.Contains("versions", machine.Calls);
         Assert.Empty(warned);

         var refused = new Machine { OpensDatabase = false };
         refused.Answers["--grant-database-login"] = 2;
         Assert.False(Finish(refused, "virtual", null, false, said, warned, out _, created: false));
         Assert.Contains("--service-account localsystem /Register", refused.Calls);
      }

      [Fact]
      public void ADatabaseThisAccountCannotOpenMeansNoMoveEither()
      {
         var machine = new Machine();
         machine.Answers["--grant-database-login"] = 2;
         var said = new List<string>();
         var warned = new List<string>();

         Assert.False(Finish(machine, "virtual", null, false, said, warned, out _));
         Assert.DoesNotContain("--service-account virtual /Register", machine.Calls);
         Assert.Contains("start", machine.Calls);
      }

      [Fact]
      public void AFolderGrantShortOfCleanLeavesLocalSystem()
      {
         var machine = new Machine();
         machine.Answers["/GrantServiceAccess"] = 4;
         var said = new List<string>();
         var warned = new List<string>();

         Assert.False(Finish(machine, "virtual", null, false, said, warned, out _));
         Assert.DoesNotContain("--service-account virtual /Register", machine.Calls);
         Assert.DoesNotContain("versions", machine.Calls);
      }

      [Fact]
      public void AMovedServiceThatDoesNotOpenItsDatabaseIsMovedBack()
      {
         var machine = new Machine { OpensDatabase = false };
         var said = new List<string>();
         var warned = new List<string>();

         Assert.False(Finish(machine, "virtual", null, false, said, warned, out _));
         int versions = machine.Calls.IndexOf("versions");
         Assert.True(versions > 0);
         Assert.Equal(new[] { "stop", "--service-account localsystem /Register", "start" }, machine.Calls.GetRange(versions + 1, 3));
         Assert.Contains(warned, line => line.Contains("did not open its database") && line.Contains("--grant-database-login"));
      }

      [Fact]
      public void AMovedServiceThatDoesNotStartIsMovedBack()
      {
         var machine = new Machine { Starts = false };
         var said = new List<string>();
         var warned = new List<string>();

         Assert.False(Finish(machine, "virtual", null, false, said, warned, out _));
         Assert.Contains("--service-account localsystem /Register", machine.Calls);
      }

      [Fact]
      public void AServiceThatWillNotStopRunsNothingAndKeepsTheMark()
      {
         var machine = new Machine { Stops = false };
         var said = new List<string>();
         var warned = new List<string>();

         Assert.False(Finish(machine, "virtual", null, false, said, warned, out bool ran));
         Assert.False(ran);
         Assert.Equal(new[] { "stop" }, machine.Calls);
      }
   }
}
