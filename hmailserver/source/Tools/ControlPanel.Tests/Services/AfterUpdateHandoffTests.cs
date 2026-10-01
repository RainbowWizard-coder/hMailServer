// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Security.Cryptography;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The sign-in carried across an administrator's install of an update
   /// (AfterUpdateHandoff): read back once, for its own id, within thirty minutes,
   /// and only with the entropy the elevated stage wrote into the administrators'
   /// work folder - a copy of the blob alone opens nothing.
   /// </summary>
   public class AfterUpdateHandoffTests : IDisposable
   {
      private readonly string folder_ = Path.Combine(Path.GetTempPath(), "hm-handoff-" + Guid.NewGuid().ToString("N"));
      private const string Id = "0123456789abcdef0123456789abcdef";
      private readonly byte[] entropy_ = RandomNumberGenerator.GetBytes(32);

      public void Dispose()
      {
         if (Directory.Exists(folder_))
            Directory.Delete(folder_, true);
      }

      [Fact]
      public void ItIsReadBackOnceWithItsEntropy()
      {
         Assert.True(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "a p\"ass word", entropy_, out string error, folder_), error);
         Assert.True(AfterUpdateHandoff.TryRead(Id, entropy_, out string host, out string user, out string password, folder_));
         Assert.Equal("localhost", host);
         Assert.Equal("Administrator", user);
         Assert.Equal("a p\"ass word", password);

         // Read once: the file is gone, and a second read has nothing.
         Assert.False(File.Exists(AfterUpdateHandoff.PathFor(Id, folder_)));
         Assert.False(AfterUpdateHandoff.TryRead(Id, entropy_, out _, out _, out _, folder_));
      }

      [Fact]
      public void TheBlobDoesNotOpenWithoutItsEntropy()
      {
         Assert.True(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", entropy_, out _, folder_));
         // The blob as a copy of the profile would hold it: this user's DPAPI key,
         // and some other entropy - or none.
         byte[] other = RandomNumberGenerator.GetBytes(32);
         Assert.False(AfterUpdateHandoff.TryRead(Id, other, out _, out _, out string password, folder_));
         Assert.Null(password);
         Assert.True(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", entropy_, out _, folder_));
         Assert.False(AfterUpdateHandoff.TryRead(Id, null, out _, out _, out _, folder_));
         // Either way the file was read once and removed.
         Assert.False(File.Exists(AfterUpdateHandoff.PathFor(Id, folder_)));
      }

      [Fact]
      public void OneOlderThanThirtyMinutesIsRefusedAndRemoved()
      {
         DateTime written = DateTime.UtcNow.AddMinutes(-31);
         Assert.True(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", entropy_, out _, folder_, written));
         Assert.False(AfterUpdateHandoff.TryRead(Id, entropy_, out _, out _, out _, folder_));
         Assert.False(File.Exists(AfterUpdateHandoff.PathFor(Id, folder_)));

         // Twenty-nine minutes is still good.
         Assert.True(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", entropy_, out _, folder_, DateTime.UtcNow.AddMinutes(-29)));
         Assert.True(AfterUpdateHandoff.TryRead(Id, entropy_, out _, out _, out _, folder_));
      }

      [Fact]
      public void OneForAnotherIdIsNotReadAsThisOne()
      {
         const string other = "fedcba9876543210fedcba9876543210";
         Assert.True(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", entropy_, out _, folder_));
         // Renamed to another id's name: its payload still names its own.
         File.Move(AfterUpdateHandoff.PathFor(Id, folder_), AfterUpdateHandoff.PathFor(other, folder_));
         Assert.False(AfterUpdateHandoff.TryRead(other, entropy_, out _, out _, out _, folder_));
         Assert.False(AfterUpdateHandoff.TryRead("not-an-id", entropy_, out _, out _, out _, folder_));
      }

      [Fact]
      public void ACorruptBlobIsRefused()
      {
         Assert.True(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", entropy_, out _, folder_));
         byte[] blob = File.ReadAllBytes(AfterUpdateHandoff.PathFor(Id, folder_));
         blob[blob.Length / 2] ^= 0x01;
         File.WriteAllBytes(AfterUpdateHandoff.PathFor(Id, folder_), blob);
         Assert.False(AfterUpdateHandoff.TryRead(Id, entropy_, out _, out _, out _, folder_));
      }

      [Fact]
      public void NothingIsWrittenWithoutEntropyOrForAMalformedId()
      {
         Assert.False(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", null, out _, folder_));
         Assert.False(AfterUpdateHandoff.Write(Id, "localhost", "Administrator", "secret", new byte[16], out _, folder_));
         Assert.False(AfterUpdateHandoff.Write("..\\x", "localhost", "Administrator", "secret", entropy_, out _, folder_));
         Assert.False(Directory.Exists(folder_) && Directory.GetFiles(folder_).Length > 0);
      }

      [Fact]
      public void StaleOnesAreSweptAtStartAndFreshOnesKept()
      {
         Directory.CreateDirectory(folder_);
         string stale = AfterUpdateHandoff.PathFor("11111111111111111111111111111111", folder_);
         string fresh = AfterUpdateHandoff.PathFor("22222222222222222222222222222222", folder_);
         string unrelated = Path.Combine(folder_, "control-panel-errors.log");
         File.WriteAllText(stale, "x");
         File.WriteAllText(fresh, "x");
         File.WriteAllText(unrelated, "x");
         File.SetLastWriteTimeUtc(stale, DateTime.UtcNow.AddHours(-2));
         Assert.Equal(1, AfterUpdateHandoff.SweepStale(folder_));
         Assert.False(File.Exists(stale));
         Assert.True(File.Exists(fresh));
         Assert.True(File.Exists(unrelated));
      }

      [Fact]
      public void TheEntropyFileIsReadAsTheStageWritesItAndRemoved()
      {
         Directory.CreateDirectory(folder_);
         string hex = Convert.ToHexString(entropy_).ToLowerInvariant();
         File.WriteAllText(Path.Combine(folder_, "handoff-entropy"), hex);
         Assert.Equal(entropy_, AfterUpdateHandoff.ReadEntropy(folder_));
         Assert.True(AfterUpdateHandoff.DeleteEntropy(folder_));
         Assert.Null(AfterUpdateHandoff.ReadEntropy(folder_));
         Assert.False(AfterUpdateHandoff.DeleteEntropy(folder_));

         File.WriteAllText(Path.Combine(folder_, "handoff-entropy"), "not hex at all, but sixty-four characters long, just to see it fail");
         Assert.Null(AfterUpdateHandoff.ReadEntropy(folder_));
      }

      [Fact]
      public void TheFolderIsTheLocalOneNeverTheRoamingOne()
      {
         Assert.StartsWith(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), AfterUpdateHandoff.Folder, StringComparison.OrdinalIgnoreCase);
         Assert.False(AfterUpdateHandoff.Folder.StartsWith(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), StringComparison.OrdinalIgnoreCase));
      }
   }
}
