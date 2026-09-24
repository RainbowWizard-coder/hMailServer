// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The message store's one door: where a message's bytes are, and every way
// they are read, sized, copied, moved and deleted.
//
// WHY THERE IS A DOOR AT ALL
//
// A message's bytes are a file under the data directory, named by
// PersistentMessage::GetFileName from the account address and the partial
// name the row holds. That name used to be asked for at about 180 places in
// eighty files, and each of them then did what it wanted with the file: read
// it with File, with MimeBody::LoadFromFile, with a std::ifstream, sized it
// with FileUtilities::FileSize, copied it, moved it, deleted it, or handed
// the path to another process. A file that is not the message's bytes as they
// arrived - compressed, with its attachments held once elsewhere, encrypted to
// its owner's key - would have been read as garbage and sized wrongly by every
// one of them. So the name is asked for here, and what is done with it goes
// through the doors below.
//
// THE THREE KINDS OF CALLER
//
//   working     A queue file, a copy of a message while it is being delivered,
//               and a file being written for a row that does not exist yet (an
//               APPEND, an import, a restore). WorkingFile() names it. Its bytes
//               are the message as written, whatever they begin with: the store
//               only turns a working file into a stored one at Place(), and a
//               queue file never is one - a file directly in the data directory
//               is read as the bytes it holds, sized by its file, and lets go of
//               nothing when it goes (MayHoldContainer). So these callers read
//               and write it by path as they always have - the scanners,
//               SpamAssassin, the filter hook, the sandbox, gateway encryption
//               and the event scripts all work on the queue file, and a path is
//               what they need.
//
//   stored      A message at rest in an account's folder or a public folder.
//               Read through the doors here - LoadHeader, the reader, ReadAll,
//               ReadRange, LoadMime, FileSize - or through the two readers
//               outside this class that go through them (PersistentMessage::
//               LoadHeader and MessageData::LoadFromMessage), and copied,
//               moved and deleted through the doors here, never by path.
//
//   listed      The places that must have the stored file's name itself: COM's
//               Message.Filename, the REST resource that reports it, the
//               indexers that carry the name in a struct to MessageData, the
//               copies the recover-deleted store keeps. They call
//               PersistentMessage::GetFileName and are listed, each with its
//               reason, in build/check-message-store-seam.py, which refuses a
//               call anywhere else - and the account-folder path WorkingFile
//               answers is listed there the same way, by file, since it names
//               the file a stored message will have.
//
// ONLY THE STORE WRITES A CONTAINER
//
// A stored file that begins with the container's magic is read as a container,
// and a container names blobs every message carrying the same part shares. So
// bytes from outside the store - a client's APPEND, a remote server's message,
// a migration's, an importer's, the queue's - are never taken for one: placing
// a working file wraps one that begins with the magic (or refuses it when that
// cannot be done), MessageData refuses to write such a message at all, a queue
// file is never a container, and a working file that was never placed is
// discarded without letting go of anything. The only containers the store
// meets are the ones its own writer made, copied or linked, each counted.
//
// WHAT THE ROWS AFTER THIS ONE BUILD ON
//
// The writer is a chain of stages over a backend. The backend is where stored
// bytes live - the local file system, MessageStoreBackend below, is the one
// there is - and an object store is another. The stages are what is done to a
// message's bytes on the way to the backend and undone on the way back; see
// docs/MessageStore.md for the chain and the container that names it. The
// listed sites and the working-file paths are what those rows audit: a working
// file is a local path by construction today, and each of its listed callers
// is a place a key-based backend has to hand a temporary file to instead.

#pragma once

#include <string>

#include "MessageStoreFormat.h"

namespace HM
{
   class Account;
   class Message;
   class ByteBuffer;
   class MimeBody;
   enum class MimeLoadResult;

   // One stored object's bytes, as the backend holds them.
   class MessageStoreBackendStream
   {
   public:
      virtual ~MessageStoreBackendStream() {}

      // The size of the object as stored.
      virtual unsigned __int64 GetSize() = 0;
      virtual bool SetPosition(unsigned __int64 position) = 0;

      // Up to maxBytes from the position; an empty buffer at the end, null
      // when the read failed.
      virtual std::shared_ptr<ByteBuffer> ReadChunk(int maxBytes) = 0;
   };

   // Where stored bytes live. A name is a path under the data directory for
   // the local file system, the one backend today; an object store maps the
   // same name to a key. Every operation answers false for a failure and
   // reports nothing itself: the caller knows what was being done.
   class MessageStoreBackend
   {
   public:
      virtual ~MessageStoreBackend() {}

      virtual bool Exists(const String &name) = 0;
      virtual bool Size(const String &name, unsigned __int64 &size) = 0;
      virtual std::shared_ptr<MessageStoreBackendStream> OpenRead(const String &name) = 0;

      // The whole object, or its first maxBytes when maxBytes is not zero.
      virtual bool ReadAll(const String &name, AnsiString &bytes, unsigned __int64 maxBytes) = 0;

      // Written under a temporary name beside the object and renamed into
      // place, so the name holds the old bytes or the new ones and never a
      // part, and another name for the old object (a hard link) keeps the old
      // bytes. Flushed to the device first when MessageStoreFsync is on.
      virtual bool WriteAtomically(const String &name, const AnsiString &bytes) = 0;

      // The directory the name is in is made when it is not there.
      virtual bool Copy(const String &from, const String &to) = 0;
      virtual bool Move(const String &from, const String &to) = 0;

      // A second name for the same object, or false where the backend or
      // the volume cannot give one.
      virtual bool Link(const String &existing, const String &link) = 0;

      // True when the name is gone, including when it was never there.
      virtual bool Remove(const String &name) = 0;

      // Which object a name is, as text two names of the same object share, and
      // how many names it has (a file's hard links). False when it cannot be
      // asked.
      virtual bool Identity(const String &name, AnsiString &identity, unsigned int &names) = 0;
   };

   // A message's ORIGINAL bytes, whatever the stored file holds: the reader
   // IMAP FETCH, POP3 RETR and TOP, and every other streaming reader use. It
   // has File's shape, so a caller that read with File reads with this.
   class MessageStoreReader
   {
   public:
      MessageStoreReader();
      ~MessageStoreReader();

      // A name from WorkingFile, or a listed site's stored name.
      bool Open(const String &messageFile);
      bool Open(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);

      bool IsOpen() const;
      void Close();

      // The message's size: the RFC 822 bytes, not the file's.
      unsigned __int64 GetSize() const;

      bool SetPosition(unsigned __int64 position);

      // Up to maxBytes from the position, an empty buffer at the end. Throws
      // std::logic_error when nothing is open, as File::ReadChunk does.
      std::shared_ptr<ByteBuffer> ReadChunk(int maxBytes);

      // A line, its CRLF included, of at most 9999 bytes - File::ReadLine's
      // fgets, including that a NUL ends the text of the line. False at the
      // end.
      bool ReadLine(AnsiString &line);

      const String &GetName() const { return name_; }

      MessageStoreReader(const MessageStoreReader &) = delete;
      MessageStoreReader &operator=(const MessageStoreReader &) = delete;

   private:

      bool Fill_();

      // The next bytes, from the stream or from the decoded message.
      std::shared_ptr<ByteBuffer> Next_(int maxBytes);

      String name_;
      std::shared_ptr<MessageStoreBackendStream> stream_;

      // A container's original bytes, decoded whole and checked when it was
      // opened - or taken from the store's last decodes, when the same file
      // was decoded a moment ago (a client reading it in pieces).
      std::shared_ptr<const std::string> decoded_;
      bool in_memory_;
      unsigned __int64 size_;

      // Where the caller is, and where the next byte from the source comes
      // from: ahead of it by what ReadLine holds read ahead.
      unsigned __int64 position_;
      unsigned __int64 source_position_;

      // Read ahead for ReadLine: the bytes from pending_offset_ on are the
      // next ones at position_.
      AnsiString pending_;
      size_t pending_offset_;
      bool open_;
   };

   // A stored message as a plain file, for as long as this lives: the stored
   // file itself when it is plain, else a temporary copy of the original bytes,
   // removed when this is. What MessageData parses, so that its fast rewrite,
   // which copies the body from the file it parsed, copies the message's bytes.
   class MessageStorePlainCopy
   {
   public:
      explicit MessageStorePlainCopy(const String &messageFile);
      ~MessageStorePlainCopy();

      MessageStorePlainCopy(const MessageStorePlainCopy &) = delete;
      MessageStorePlainCopy &operator=(const MessageStorePlainCopy &) = delete;

      // False when the stored file is a container that could not be read; the
      // reason is in the error log (HM7350).
      bool IsReady() const { return ready_; }
      bool IsTemporary() const { return temporary_; }
      const String &GetPath() const { return path_; }

      // The stored file has been written over by a plain one: what the
      // container referenced is let go of. Once.
      void StoredFileReplaced();

   private:
      String path_;
      bool ready_;
      bool temporary_;
      std::vector<MessageStoreReference> references_;
   };

   class MessageStore
   {
   public:

      // The backend the stored bytes live in: the local file system.
      static MessageStoreBackend &Backend();

      // ---- The working file --------------------------------------------------
      //
      // The file of a message the store has not placed: a queue file, a copy
      // being delivered, a file written for a new row. The same path
      // PersistentMessage::GetFileName answers; see the header of this file
      // for why its callers may read and write it by path. The two-argument
      // form names an account's folder, where a stored message will be, and
      // is called only from the files build/check-message-store-seam.py lists.
      static String WorkingFile(std::shared_ptr<const Message> message);
      static String WorkingFile(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);

      // Whether a name can be a container at all. A file directly in the data
      // directory is a queue file, and a queue file is the message's bytes by
      // construction whatever they begin with: every door below reads it as
      // those bytes, sizes it by its file, and lets go of nothing when it goes.
      static bool MayHoldContainer(const String &messageFile);

      // A working file that was never placed, and so counted nothing: the name
      // removed and nothing let go of, whatever its first bytes claim. What a
      // delivery, an APPEND, the IMAP mirror or an import that gives up before
      // placing uses, rather than Remove.
      static bool DiscardWorkingFile(const String &messageFile);

      // ---- By name -----------------------------------------------------------
      //
      // A name from WorkingFile, or a listed site's stored name. Each reads the
      // ORIGINAL bytes and answers the ORIGINAL size, whatever the file holds.

      static bool Exists(const String &messageFile);

      // The message's size, saturated at LONG_MAX as FileUtilities::FileSize
      // saturates, and 0 when it cannot be read.
      static long FileSize(const String &messageFile);
      static bool FileSize64(const String &messageFile, unsigned __int64 &size);

      // What the file takes on the backend - the physical size.
      static bool StoredFileSize64(const String &messageFile, unsigned __int64 &size);

      // The whole message. False when the file is missing or cannot be read.
      static bool ReadAll(const String &messageFile, AnsiString &bytes);

      // FileUtilities::ReadFileToBuf over the original bytes: count bytes
      // from start into buffer, zeros past the end. Throws when the file
      // cannot be opened or the position cannot be reached, as that does.
      static void ReadRange(const String &messageFile, BYTE *buffer, int start, int count);

      // MimeBody::LoadFromFile over the original bytes, with its three
      // answers. The name is narrow because LoadFromFile's is.
      static MimeLoadResult LoadMime(const AnsiString &messageFile, MimeBody &body);

      // A plain RFC 822 file holding the message, for a caller that must
      // hand one to another process by path: the file itself when it is
      // plain, else a temporary copy of the original bytes, in which case
      // temporary is set and the caller deletes it. Empty when neither can be
      // had.
      static String PlainFileFor(const String &messageFile, bool &temporary);

      // A copy as the file is stored - a copy into another folder, one the
      // recover-deleted store keeps, one put back from a backup.
      static bool CopyStored(const String &from, const String &to);

      // A copy of the ORIGINAL bytes, as plain RFC 822 - to the queue, to an
      // export, to anything outside the store.
      static bool CopyOriginal(const String &from, const String &to);

      // The same file under another name. Nothing else changes.
      static bool Rename(const String &from, const String &to);

      // A second name for the file (DeliveryHardLinks), or false where the
      // volume cannot give one.
      static bool Link(const String &existing, const String &link);

      // The name gone, and whatever it alone kept with it.
      static bool Remove(const String &messageFile);

      // Whether two names are one file (two names DeliveryHardLinks gave it).
      static bool SameFile(const String &first, const String &second);

      // Whether the stored file is a container, and what it references. False
      // for a file that cannot be read, or a container whose header is not one
      // this server reads.
      static bool IsContainer(const String &messageFile);
      static bool GetReferences(const String &messageFile, std::vector<MessageStoreReference> &references);

      // The compressed container of a message's original bytes written under a
      // new name, its large parts kept as blobs and counted for that name.
      // written is false, and nothing is kept, when it would not make the
      // message smaller and holds no part once. The walk's door.
      static bool WriteContainer(const std::string &original, const String &storedFile, unsigned __int64 dedupMinimumBytes,
                                 bool &written, String &problem);

      // A name made a name of another stored file: the first recipient's placed
      // copy of a DeliveryHardLinks delivery linked to the next recipient's
      // name, which it replaces, and a file the walk rewrote linked to each of
      // its names. False, with the name left as it was, when the volume cannot
      // link them.
      static bool Relink(const String &storedFile, const String &name);

      // ---- By message ----------------------------------------------------------

      static AnsiString LoadHeader(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, bool reportError = true);
      static long Size(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);
      static __int64 StoredSize(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);
      static bool Delete(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);

      // ---- Placing: a working file becomes a stored file ------------------------
      //
      // The working file of a message put in an account's folder or a public
      // folder - delivered, appended, collected by the IMAP mirror, brought in
      // by a migration or an import - becomes its stored file: compressed with
      // its large parts held once when MessageStoreCompression is on, left as
      // it is when that is off or would not make it smaller. Its bytes came from
      // outside the store and are the message whatever they begin with, so one
      // that begins with the container's magic is wrapped - a container with no
      // stage, which the reader gives back as exactly those bytes - whatever
      // the setting says.
      //
      // True when the file may be kept: in the stored form, or as it came when
      // it is plain and could not be compressed (the error log says why,
      // HM7351). False when it must NOT be kept - it begins with the magic and
      // could not be wrapped (over 64 MB, or the write failed; HM7353) - and
      // then the caller refuses the message and removes the file with
      // DiscardWorkingFile, never Remove. changed says whether the file was
      // rewritten, which DeliveryHardLinks asks.
      static bool Place(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);
      static bool PlaceWorkingFile(const String &messageFile);
      static bool PlaceWorkingFile(const String &messageFile, bool &changed);

      // A stored file's original bytes, and the reason when they cannot be had.
      // The whole container is checked, blobs and SHA-256 included, and read
      // from the file every time - never from the last decodes.
      static bool ReadOriginal(const String &messageFile, std::string &original, String &problem);

      // HM7350: a stored message that could not be read, refused.
      static void ReportUnreadable(const String &messageFile, const String &problem);

      // For PersistentMessage::LoadHeader and LoadBody, which read a file that
      // may hold a container and find it begins with the magic: the header of
      // the message the container holds (what LoadHeader answers for a plain
      // file, the blank line's first CRLF included), read without its blobs but
      // with the rest of the container checked (MessageStoreFormat::DecodeStart),
      // and the body after the blank line, read whole and checked. Empty when
      // it cannot be read.
      static AnsiString ReadHeaderOfContainer(const String &messageFile, bool reportError);
      static AnsiString ReadBodyOfContainer(const String &messageFile);

   private:

      enum PlaceOutcome
      {
         PlaceLeft,       // left as it is: plain, and nothing to do or nothing worth doing
         PlaceWritten,    // rewritten in the stored form, compressed or wrapped
         PlaceFailed,     // plain, and could not be compressed: left as it came
         PlaceRefused     // begins with the magic and could not be wrapped: not to be kept
      };

      static PlaceOutcome PlaceFile_(const String &messageFile, bool compress, unsigned __int64 dedupMinimumBytes, String &problem);
      static bool PlaceWorkingFile_(const String &messageFile, bool compress, bool &changed);

      static bool Encode_(const std::string &original, bool compress, unsigned __int64 dedupMinimumBytes,
                          std::string &container, std::vector<MessageStoreReference> &references, String &problem);
   };
}
