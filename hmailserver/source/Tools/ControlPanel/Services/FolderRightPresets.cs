// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The three sets of rights the access-control entry dialog offers as one press
   /// each (review finding F205), over the eleven IMAP ACL rights the server keeps
   /// (eACLPermission, RFC 4314's l r s w i p k x t e a): the entry offered the
   /// eleven raw, and an administrator who wanted "read only" had to know which
   /// three of them make it. Without WPF, so the tests can hold each set.
   /// </summary>
   public static class FolderRightPresets
   {
      public const int Lookup = 1;
      public const int Read = 2;
      public const int KeepSeen = 4;
      public const int SetFlags = 8;
      public const int Insert = 16;
      public const int Post = 32;
      public const int CreateFolders = 64;
      public const int DeleteFolder = 128;
      public const int DeleteMessages = 256;
      public const int Expunge = 512;
      public const int Administer = 1024;

      /// <summary>See the folder, read its messages and keep their seen marks: RFC 4314's "lrs".</summary>
      public const int ReadOnly = Lookup | Read | KeepSeen;

      /// <summary>
      /// Read only, and change the messages: set flags, put messages in, delete
      /// them and expunge - "lrswite". Not Post, which lets the account send as the
      /// mailbox's address, nor the rights over the folder itself.
      /// </summary>
      public const int ReadAndWrite = ReadOnly | SetFlags | Insert | DeleteMessages | Expunge;

      /// <summary>Every right, Administer - managing the folder's entries - included.</summary>
      public const int FullControl = Lookup | Read | KeepSeen | SetFlags | Insert | Post | CreateFolders | DeleteFolder | DeleteMessages | Expunge | Administer;
   }
}
