// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The message store on an object store: S3-compatible storage or Azure Blob
// Storage, with the database as the index (docs/ObjectStorage.md). The second
// MessageStoreBackend; MessageStore::Backend() answers it while a store is
// configured, and the local file system otherwise.
//
// NAMES AND KEYS
//
// A name is what it always was, a path under the data directory, and its key is
// that path relative to the data directory: separators made '/', letters made
// small as StoreDirectoryName makes them on POSIX, UTF-8, after
// MessageStoreObjectPrefix. A message's key is
//
//     example.com/alice/4c/{4c0a...}.eml
//
// so a store is a copy of the tree, and switching a server between the backends
// is a copy of the tree (hmailserver --copy-store-to-object-storage) and a
// setting. A name directly in the data directory is a queue file and never has a
// key, and neither has a name staged beside another (.writing-, .encrypting-,
// .plaintext-, .linking-, .store-): the queue and every staged file stay on the
// local disk, and so does everything outside the data directory (temporary
// files, an archive being restored). There is no staging area in the store.
//
// THE LOCAL DISK COMES FIRST
//
// Every write is a local one: a message is placed on the local disk exactly as
// the file system backend places it, and a rewrite, a blob or a kept copy is
// written there too. The committer puts a file into the store afterwards and
// only then removes it from the disk - a message when its row is saved
// (MessageStore::Commit, which PersistentMessage::SaveObject calls), anything
// else written through this backend half a minute after it was written - on the
// server's maintenance queue, a minute at most after it was handed over, with a
// backoff while the store does not take it (reported once an hour, HM7473). So
// a delivery, an APPEND, a rule's copy never wait for the store and never fail
// for it, and every read looks at the local disk first: a file there is the
// newest bytes under the name. What the committer has been handed is written to
// a journal (<data directory>/_ObjectStore/pending.txt) and taken up again after
// a restart.
//
// WHICH WAY NEW MAIL GOES
//
//   MessageStoreObjectService  none (the default), s3 or azure: the store the
//                              settings below describe. While one is named it is
//                              read whenever a name is not on the local disk,
//                              whichever way new mail goes - so turning the
//                              backend back to file hides nothing.
//   MessageStoreBackend        file (the default) or object: whether the
//                              committer puts placed messages into the store.
//                              With file nothing is committed and a server that
//                              never names a service does exactly what it did
//                              before.
//
// Link answers false while new mail goes to the store: an object store has no
// second name for an object (every caller of Link already copies when it answers
// false). Identity is the object's ETag, and a name's count of names is one.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace HM
{
   class MessageStoreBackend;
   class ScheduledTask;

   class MessageStoreObjects
   {
   public:

      // The object store's backend while a service is configured, else null:
      // MessageStore::Backend's question. Settles within five seconds of a
      // setting changing.
      static MessageStoreBackend *Active();

      // Whether placed messages go to the store (MessageStoreBackend is object
      // and a store is configured).
      static bool WritesToStore();

      // Whether a name has a key while a store is configured: not a queue
      // file, not a staged one, not one outside the data directory.
      static bool Keyed(const String &name);

      // Held while a new name is being made - a message placed, copied or
      // written for a row that is saved after it: what is written under the
      // name then is not handed to the committer as it is written, but when the
      // row is saved (MessageStore::Commit), so that a placing that is refused,
      // or whose row is never saved, leaves nothing in the store. Blobs are
      // counted rows of their own, and are handed over as they are written.
      class Placing
      {
      public:
         Placing();
         ~Placing();

         Placing(const Placing &) = delete;
         Placing &operator=(const Placing &) = delete;
      };

      // Held while a directory's late arrivals are carried across under a new
      // name (MailboxMove, SweepLateArrivals): the committer takes no name
      // under it meanwhile, and the hold begins once no name under it is being
      // put - two minutes at most - so that nothing is put under an old name
      // after the objects under it were listed. Nothing while no store is named.
      class HoldTree
      {
      public:
         explicit HoldTree(const String &directory);
         ~HoldTree();

         HoldTree(const HoldTree &) = delete;
         HoldTree &operator=(const HoldTree &) = delete;

      private:
         String directory_;
         bool held_;
      };

      // The committer's minute: the names due are put into the store on the
      // maintenance queue, and the journal of a server that stopped with some
      // pending is taken up again. CommitTask is the scheduled task that asks
      // it (Application::CreateScheduledTasks_). Nothing while no store is
      // written to.
      static void CommitPending();
      static std::shared_ptr<ScheduledTask> CommitTask();

      // Said of a message the store answered a confirmed 404 for and the local
      // disk does not have (HM7476, once a minute at most): while a store is
      // named no placeholder is written in its place (PersistentMessage::
      // EnsureFileExistance) - a move under way, a store changed back and forth
      // or one that answered 404 for a moment would have it written over the
      // message - and the doors answer as for a store that did not answer.
      static void ReportMissing(const String &messageFile);

      // The objects under one directory of the store moved under another: an
      // account renamed, or moved to another domain. The local file system's
      // part - the directory itself - is its callers' as it always was, and
      // every file on the local disk under the new directory is handed to the
      // committer afterwards. A name the new directory already holds is never
      // written over: the old object goes only when the new one is the same
      // object, and is left where it is, both reported (HM7475), when it is
      // not. True when there is no store, or nothing under the directory; a
      // second call after a failure moves what the first did not.
      static bool MoveTree(const String &fromDirectory, const String &toDirectory, unsigned int &moved, String &problem);

      // MoveTree, after the files still under the old directory on the local
      // disk - mail delivered under the old name while the directory was moved,
      // which recreated it - are moved under the new one: a domain renamed,
      // once its row and the cache carry the new name. A file whose name the
      // new directory already holds is left where it is and counted in the
      // problem.
      static bool SweepLateArrivals(const String &fromDirectory, const String &toDirectory, unsigned int &moved, String &problem);

      // Every file on the local disk under a directory handed to the
      // committer: a mailbox move's late arrivals, merged into the new tree on
      // the disk (MailboxMove::SweepLateArrivals_).
      static void CommitTree(const String &directory);

      // For a backup with messages: every message and blob the database names
      // that the store holds and the local disk does not, read into the staged
      // tree under the same relative name, so that the archive holds every
      // message whichever backend holds it now. The kept copies stay out of a
      // backup, as they always have. False, with the reason, when the store
      // cannot be read: a backup without its messages is not taken.
      static bool StageInto(const String &stagingDirectory, unsigned int &fetched, String &problem);

      //   hmailserver --copy-store-to-object-storage   [--account a | --domain d] [--remove-source]
      //   hmailserver --copy-store-from-object-storage [--account a | --domain d] [--remove-source]
      //
      // Every message, kept copy and blob the database names copied to the
      // store (or back to the local disk), each verified - its size, and its
      // MD5 where the store keeps one or its ETag is one, else read back; with
      // --remove-source, an MD5 this server gave the store is not taken for
      // one, and the object is read back - and a name already there and
      // verified left as it is, so that a run that was stopped goes on where
      // it stopped. A run over one account or domain takes the blobs its
      // messages reference with it. A copy from the store never replaces a
      // local file: one that appears while the object is read is counted as
      // there already. With --remove-source the local file (or the object) is
      // removed once the copy of it is verified, and only with the server
      // stopped. The exit code: 0 done, 1 a file could not be copied or
      // verified, or a list of kept copies or blobs could not be read, 2 no
      // store is configured or no such account or domain, 4 --remove-source
      // while the server runs.
      static int RunCopyCommand(bool toStore, const String &accountAddress, const String &domainName, bool removeSource);
   };
}
