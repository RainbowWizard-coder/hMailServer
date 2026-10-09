// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using System.Security.Principal;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Whether this process's token holds a privilege enabled. A test that denies a
   /// folder to its own account asks it of SeBackupPrivilege: a directory is opened for
   /// backup to be listed, so an account with that privilege enabled - an
   /// administrator's SSH session on Windows - lists it past the deny entry, which the
   /// Control Panel, run from the desktop, cannot.
   /// </summary>
   internal static partial class TokenPrivileges
   {
      private const int TokenPrivilegesClass = 3;
      private const int Enabled = 0x2;

      // LUID_AND_ATTRIBUTES: an eight-byte LUID and four bytes of attributes, packed
      // on four, after TOKEN_PRIVILEGES' four-byte count.
      private const int EntrySize = 12;

      [LibraryImport("advapi32.dll", EntryPoint = "LookupPrivilegeValueW",
         StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
      [return: MarshalAs(UnmanagedType.Bool)]
      private static partial bool LookupPrivilegeValue(string systemName, string name, out long luid);

      [LibraryImport("advapi32.dll", SetLastError = true)]
      [return: MarshalAs(UnmanagedType.Bool)]
      private static partial bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returned);

      public static bool IsEnabled(string privilege)
      {
         if (!LookupPrivilegeValue(null, privilege, out long luid))
            return false;

         using WindowsIdentity me = WindowsIdentity.GetCurrent();
         GetTokenInformation(me.Token, TokenPrivilegesClass, IntPtr.Zero, 0, out int size);
         if (size <= 0)
            return false;

         IntPtr buffer = Marshal.AllocHGlobal(size);
         try
         {
            if (!GetTokenInformation(me.Token, TokenPrivilegesClass, buffer, size, out _))
               return false;

            int count = Marshal.ReadInt32(buffer);
            for (int i = 0; i < count; i++)
            {
               IntPtr entry = buffer + 4 + i * EntrySize;
               if (Marshal.ReadInt64(entry) == luid)
                  return (Marshal.ReadInt32(entry + 8) & Enabled) != 0;
            }

            return false;
         }
         finally
         {
            Marshal.FreeHGlobal(buffer);
         }
      }
   }
}
