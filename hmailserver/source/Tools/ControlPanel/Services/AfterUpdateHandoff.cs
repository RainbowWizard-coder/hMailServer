// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The sign-in the Control Panel carries across an administrator's install of an
   /// update, so that the one that opens again afterwards signs in by itself and
   /// shows the outcome (docs/ServiceAccount.md, "The live update").
   ///
   /// The Control Panel closes before the installer runs - the installer replaces
   /// its files - and the Updater starts it again, unelevated, as the desktop's
   /// user, with --after-update &lt;id&gt;. Just before it closes, this one writes
   /// after-update-&lt;id&gt;.bin in %LOCALAPPDATA%\hMailServer\ControlPanel (never a
   /// roaming or redirected folder): the host, the user name and the password,
   /// protected with DPAPI for this Windows user AND with a second secret, the
   /// entropy, which the elevated stage wrote into the administrators' work folder
   /// with a Read entry for this user alone. A copy of the profile - a backup, a
   /// sync, a disk read - holds only the blob, which does not open without the
   /// entropy, and the Control Panel that opens again deletes both when it has read
   /// them. It is also read once, only for its own id, and only within thirty
   /// minutes; every start of the Control Panel removes older ones. The password's
   /// bytes are cleared as soon as they are used. With a second factor on the
   /// administrator's sign-in nothing is written at all: a one-time code cannot be
   /// carried, and the Control Panel shows its sign-in page with the outcome.
   /// </summary>
   public static class AfterUpdateHandoff
   {
      public static readonly TimeSpan Lifetime = TimeSpan.FromMinutes(30);
      private const string Prefix = "after-update-"; // no-loc: a file name

      public static string Folder => Path.Join(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "hMailServer", "ControlPanel");

      public static string PathFor(string id, string folder = null) => Path.Join(folder ?? Folder, Prefix + id + ".bin");

      /// <summary>The work folder's entropy file, which the stage wrote: 64 hexadecimal characters.</summary>
      public static byte[] ReadEntropy(string workFolder)
      {
         string text = AdministratorUpdate.ReadShared(Path.Join(workFolder, "handoff-entropy"), 1024)?.Trim();
         if (text == null || text.Length != 64)
            return null;
         try
         {
            return Convert.FromHexString(text);
         }
         catch (FormatException)
         {
            return null;
         }
      }

      /// <summary>Removes the entropy file, which its reader may delete; false when it is not there to remove.</summary>
      public static bool DeleteEntropy(string workFolder)
      {
         try
         {
            string path = Path.Join(workFolder, "handoff-entropy");
            if (!File.Exists(path))
               return false;
            File.Delete(path);
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return false;
         }
      }

      private sealed class Payload
      {
         public string Id { get; set; }
         public string Host { get; set; }
         public string User { get; set; }
         public string Password { get; set; }
         public long Utc { get; set; }
      }

      /// <summary>Writes the handoff; false with error when it could not be.</summary>
      public static bool Write(string id, string host, string user, string password, byte[] entropy, out string error, string folder = null, DateTime? now = null)
      {
         error = null;
         if (!AdministratorUpdate.IsWorkId(id) || entropy == null || entropy.Length != 32)
         {
            error = "no entropy to bind the handoff to"; // no-loc: a log line
            return false;
         }
         byte[] plain = JsonSerializer.SerializeToUtf8Bytes(new Payload
         {
            Id = id,
            Host = host ?? "",
            User = user ?? "",
            Password = password ?? "",
            Utc = (now ?? DateTime.UtcNow).Ticks
         });
         try
         {
            byte[] blob = Protect_(plain, entropy);
            if (blob == null)
            {
               error = "DPAPI could not protect it"; // no-loc: a log line
               return false;
            }
            string target = folder ?? Folder;
            Directory.CreateDirectory(target);
            File.WriteAllBytes(PathFor(id, target), blob);
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            error = ex.Message;
            return false;
         }
         finally
         {
            Array.Clear(plain, 0, plain.Length);
         }
      }

      /// <summary>
      /// Reads and deletes the handoff for id. False - and nothing to sign in with -
      /// when there is none, it is older than <see cref="Lifetime"/>, it is for
      /// another id, or it does not open with this user's DPAPI key and the entropy.
      /// </summary>
      public static bool TryRead(string id, byte[] entropy, out string host, out string user, out string password, string folder = null, DateTime? now = null)
      {
         host = user = password = null;
         if (!AdministratorUpdate.IsWorkId(id))
            return false;
         string path = PathFor(id, folder);
         byte[] blob;
         try
         {
            if (!File.Exists(path))
               return false;
            blob = File.ReadAllBytes(path);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return false;
         }
         finally
         {
            // Read once: whatever it held, it is gone now.
            try { File.Delete(path); } catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex)) { }
         }

         if (entropy == null || entropy.Length != 32)
            return false;
         byte[] plain = Unprotect_(blob, entropy);
         if (plain == null)
            return false;
         try
         {
            Payload payload = JsonSerializer.Deserialize<Payload>(plain);
            if (payload == null || payload.Id != id)
               return false;
            var written = new DateTime(payload.Utc, DateTimeKind.Utc);
            TimeSpan age = (now ?? DateTime.UtcNow) - written;
            if (age < TimeSpan.Zero || age > Lifetime)
               return false;
            host = payload.Host;
            user = payload.User;
            password = payload.Password;
            return true;
         }
         catch (JsonException)
         {
            return false;
         }
         finally
         {
            Array.Clear(plain, 0, plain.Length);
         }
      }

      /// <summary>Removes every handoff older than <see cref="Lifetime"/>; at every start.</summary>
      public static int SweepStale(string folder = null, DateTime? now = null)
      {
         int removed = 0;
         try
         {
            string target = folder ?? Folder;
            if (!Directory.Exists(target))
               return 0;
            foreach (string file in Directory.EnumerateFiles(target, Prefix + "*.bin"))
            {
               if ((now ?? DateTime.UtcNow) - File.GetLastWriteTimeUtc(file) <= Lifetime)
                  continue;
               File.Delete(file);
               removed++;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
         }
         return removed;
      }

      // ---- DPAPI, the current user's key, with entropy, no UI

      [StructLayout(LayoutKind.Sequential)]
      private struct DataBlob
      {
         public int Size;
         public IntPtr Data;
      }

      private const int CryptProtectUiForbidden = 0x1;

      [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern bool CryptProtectData(ref DataBlob input, string description, ref DataBlob entropy, IntPtr reserved, IntPtr prompt, int flags, out DataBlob output);

      [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern bool CryptUnprotectData(ref DataBlob input, IntPtr description, ref DataBlob entropy, IntPtr reserved, IntPtr prompt, int flags, out DataBlob output);

      [DllImport("kernel32.dll")]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern IntPtr LocalFree(IntPtr memory);

      private static byte[] Protect_(byte[] plain, byte[] entropy) => Call_(plain, entropy, true);

      private static byte[] Unprotect_(byte[] blob, byte[] entropy) => Call_(blob, entropy, false);

      private static byte[] Call_(byte[] data, byte[] entropy, bool protect)
      {
         var dataHandle = GCHandle.Alloc(data, GCHandleType.Pinned);
         var entropyHandle = GCHandle.Alloc(entropy, GCHandleType.Pinned);
         try
         {
            var input = new DataBlob { Size = data.Length, Data = dataHandle.AddrOfPinnedObject() };
            var extra = new DataBlob { Size = entropy.Length, Data = entropyHandle.AddrOfPinnedObject() };
            // Each branch frees what DPAPI allocated, and the plaintext's copy in
            // DPAPI's buffer is cleared before it is freed.
            if (protect)
            {
               if (!CryptProtectData(ref input, "hMailServer Control Panel", ref extra, IntPtr.Zero, IntPtr.Zero, CryptProtectUiForbidden, out DataBlob protectedBlob)) // no-loc: a label DPAPI keeps
                  return null;
               Copy_(protectedBlob, out byte[] protectedBytes);
               return protectedBytes;
            }
            if (!CryptUnprotectData(ref input, IntPtr.Zero, ref extra, IntPtr.Zero, IntPtr.Zero, CryptProtectUiForbidden, out DataBlob plainBlob))
               return null;
            Copy_(plainBlob, out byte[] plainBytes, clear: true);
            return plainBytes;
         }
         finally
         {
            dataHandle.Free();
            entropyHandle.Free();
         }
      }

      private static bool Copy_(DataBlob blob, out byte[] bytes, bool clear = false)
      {
         bytes = new byte[blob.Size];
         if (blob.Size > 0)
            Marshal.Copy(blob.Data, bytes, 0, blob.Size);
         if (clear && blob.Size > 0)
         {
            var zero = new byte[blob.Size];
            Marshal.Copy(zero, 0, blob.Data, blob.Size);
         }
         LocalFree(blob.Data);
         return true;
      }
   }
}
