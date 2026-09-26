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
//               LoadHeader, and MessageData::LoadFromMessage through
//               MessageStoreParseSource, which parses a container from memory
//               and never writes its message to a file), and copied,
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
// bytes live - the local file system, MessageStoreBackend below, and an object
// store, S3-compatible or Azure Blob Storage (MessageStoreObjects.h,
// docs/ObjectStorage.md). The stages are what is done to a message's bytes on
// the way to the backend and undone on the way back; see docs/MessageStore.md
// for the chain and the container that names it. A working file is a local path
// on either backend: an object store places every message on the local disk as
// the file system does, reads a name there before it asks the store, and puts
// it into the store afterwards (Commit, the committer), so every listed caller
// that hands a working file to something by path goes on doing so; a stored
// message a listed site must hand over by path is written out to a temporary
// file (PlainFileFor).

#pragma once

#include <string>

#include "MessageStoreFormat.h"
#include "../Util/MailboxKeys.h"

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

      // ---- What an object store answers differently (MessageStoreObjects.h) --
      //
      // The local file system answers these as they are written here. An
      // object store keeps a working file on the local disk until it is
      // committed, and reads a name there before it asks the store.

      // Whether the name is a file on the local disk, readable by path.
      virtual bool IsLocal(const String &) { return true; }

      // A stored message's file on the local disk, whose row is saved, handed
      // on to be made a stored one: for an object store, queued for the
      // committer, which puts it into the store and then removes it from the
      // disk. Never waits for the store, and never fails the caller: a file the
      // store does not take stays on the disk, where it is read from.
      virtual void Commit(const String &) {}

      // Every name under a directory, however deep: its files, and on an
      // object store its objects too. False when they could not all be listed.
      virtual bool List(const String &directory, std::vector<String> &names);
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

   // What MessageData parses: the stored file itself, by its path, when it is
   // plain (or not there yet, a message being composed); else the container's
   // original bytes, decoded into memory and held for as long as this lives -
   // never written to a file, so that reading a message of a mailbox
   // encrypted at rest (docs/EncryptionAtRest.md) puts nothing of it on the
   // disk. MessageData's fast rewrite, which copies an unchanged body from
   // what it parsed, copies it from these bytes.
   class MessageStoreParseSource
   {
   public:
      explicit MessageStoreParseSource(const String &messageFile);

      MessageStoreParseSource(const MessageStoreParseSource &) = delete;
      MessageStoreParseSource &operator=(const MessageStoreParseSource &) = delete;

      // False when the stored file is a container that could not be read; the
      // reason is in the error log (HM7350), or in the debug log for a
      // mailbox whose key is closed.
      bool IsReady() const { return ready_; }

      // A container's original bytes; null for a plain file, which is parsed
      // from its path.
      const std::shared_ptr<const std::string> &GetOriginal() const { return original_; }
      bool InMemory() const { return original_ != nullptr; }

      // The stored file has been written over: what the container referenced
      // is let go of. Once.
      void StoredFileReplaced();

      // The public key the stored file was encrypted to, when it was: a
      // rewrite is encrypted to it again, in memory, and never written in the
      // clear (MessageData::Write).
      const std::string &GetEncryptedTo() const { return encrypted_to_; }

   private:
      bool ready_;
      std::shared_ptr<const std::string> original_;
      std::vector<MessageStoreReference> references_;
      std::string encrypted_to_;
   };

   class MessageStore
   {
   public:

      // The backend the stored bytes live in: the object store while one is
      // configured (MessageStoreObjects::Active), else the local file system.
      static MessageStoreBackend &Backend();

      // The local file system, whichever backend holds the stored messages:
      // what the object store's backend reads a working file with.
      static MessageStoreBackend &LocalBackend();

      // ---- An object store (docs/ObjectStorage.md) ------------------------------
      //
      // None of these does anything on the local file system, the default.

      // A stored message's file, whose row is saved, handed to the object
      // store's committer when that is where new mail goes: every placed
      // message stays on the local disk - where it is read from - until the
      // committer has put it into the store, a minute or so later, retrying
      // with a backoff for as long as the store does not take it (reported once
      // an hour, HM7473). PersistentMessage::SaveObject asks it for every
      // delivered message it saves; placing never waits for the store.
      static void Commit(const String &messageFile);

      // The stored messages under one directory moved under another: an
      // account renamed, or moved to another domain (MessageStoreObjects::
      // MoveTree). The directory on the local disk is its callers' to move.
      static bool MoveTree(const String &fromDirectory, const String &toDirectory, unsigned int &moved, String &problem);

      // Whether a door failed on this thread because the object store could
      // not answer for a message - not reached, this server's credentials
      // refused, or any answer but the object or a confirmed 404 - a condition
      // outside the message, answered as a temporary one: IMAP NO
      // [UNAVAILABLE], POP3 -ERR [SYS/TEMP], REST 503 - rather than as a
      // missing or damaged message. Never on the local file system.
      // ClearUnavailable before the doors, Unavailable after them;
      // NoteUnavailable is the backend's.
      static bool Unavailable();
      static void ClearUnavailable();
      static void NoteUnavailable();

      // Whether a door found on this thread a message the object store
      // confirms it does not have (MessageStoreObjects::ReportMissing): gone
      // for good, where Unavailable is for now. Noted with Unavailable, which
      // every door answers already; ClearUnavailable clears both. IMAP's FETCH
      // leaves such a message out of its answer and fetches the rest.
      static bool Missing();
      static void NoteMissing();

      // Whether a door on this thread refused a stored message it could not
      // read (ReportUnreadable): a container that is damaged (HM7350), or one
      // of a mailbox encrypted at rest read while the account's key is closed
      // - the second is the one UnreadableKeyClosed answers true for. A door
      // that fails that way answers as a file with nothing in it would - an
      // empty header, an empty body, a size of nought - so a caller that must
      // not hand that on as the message, as IMAP's FETCH must not, asks this
      // after its reads. ClearUnreadable before them; not cleared by
      // ClearUnavailable, which a door may call in the middle.
      static bool Unreadable();
      static bool UnreadableKeyClosed();
      static void ClearUnreadable();

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
      // had - and always for a file encrypted to a mailbox's key, whose
      // message is never written out in the clear (docs/EncryptionAtRest.md).
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

      // ---- Encryption at rest (docs/EncryptionAtRest.md, Util/MailboxKeys.h) ------
      //
      // A container encrypted to an account's key names no blob and is read
      // only while that key is open. Every door above reads it as the message
      // then, and refuses it - without calling it damage - while the key is
      // closed; its size, its header and its references (none) are read
      // without the key.

      // Whether the stored file is encrypted, and to which public key.
      static bool IsEncrypted(const String &messageFile, std::string &publicKey);

      // The original bytes, held in memory, encrypted to publicKey and written
      // beside storedFile under a temporary name - compressed first when
      // MessageStoreCompression is on - read back and compared byte for byte
      // with what was encrypted, and only then renamed over storedFile. When it
      // fails nothing is left beside it and what storedFile held is as it was.
      // The walk's door (--encrypt-store), MessageData's for a rewrite or a new
      // message, and a copy's. The plaintext never reaches the disk here.
      static bool WriteEncryptedContainer(const std::string &original, const String &storedFile, const std::string &publicKey, String &problem);

      // A working file's bytes overwritten with zeros where they are: the
      // plaintext of a message for a mailbox encrypted at rest, once it has
      // been stored encrypted or refused. What a filesystem, a snapshot or an
      // SSD keeps of blocks written over is theirs (docs/EncryptionAtRest.md).
      // A file with more than one name is left alone: the zeros would be every
      // name's.
      static void Scrub(const String &messageFile);

      // What an encryption cut short by a crash left beside the files of a
      // mailbox encrypted at rest, under each of these directories (the keyed
      // accounts' own, from MailboxKeys::Start, before anything is delivered):
      // a plaintext moved aside (<name>.plaintext-<GUID>) is overwritten with
      // zeros and removed once the name holds the message - the container
      // staged beside it (<name>.encrypting-<GUID>) renamed into the name first
      // when the name is empty - and put back under the name, never overwritten,
      // when nothing else holds the message; a staged container the name does
      // not need is removed. Only a FILE NAME is read, never the path above it
      // - a mailbox, a domain or a data directory may be named with either
      // mark - and only one whose name part is not empty and whose part after
      // the last mark is exactly a braced GUID, as GUIDCreator writes it; any
      // other file is left alone. A directory that cannot be walked is
      // reported and passed. Said in the application log when it found any.
      static void SweepEncryptionLeftovers(const std::vector<String> &directories);

      // A copy into a folder of the account destination owns (a public folder
      // when it is null): as stored when both ends have the same key or
      // neither has one, else re-encoded - the original read (with the
      // source's key, which must be open) and stored as the destination's
      // mailbox stores it. CopyToIMAPFolder's door.
      static bool CopyIntoAccount(const String &from, const String &to, std::shared_ptr<const Account> destination);

      // What was decoded a moment ago, forgotten: a key that closes takes the
      // plaintext of the messages it opened with it.
      static void ForgetDecodes();

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
      //
      // A message placed in a folder of an account whose mailbox is encrypted at
      // rest (MailboxKeys::EncryptionFor) is encrypted to the account's key,
      // compressed first when compression is on, and never holds a part once,
      // and the plaintext it was is overwritten with zeros (Scrub). One that
      // cannot be - larger than a container holds, a write that failed, or a
      // mailbox whose encryption could not be read - is REFUSED (false,
      // HM7365) rather than kept in the clear, its plaintext overwritten.
      static bool Place(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);

      // What a refused Place, PlaceWorkingFileFor or WriteForAccount was, for
      // the caller's answer: a delivery's status, an APPEND's response, a
      // route's.
      enum PlaceRefusal
      {
         PlaceAccepted,          // not refused
         PlaceRefusedContent,    // it begins with the mark of a container and could not be wrapped (HM7353): its own bytes, which another attempt will not change
         PlaceRefusedTooLarge,   // for a mailbox encrypted at rest, and larger than a container holds (64 MB): the same
         PlaceRefusedWrite,      // for a mailbox encrypted at rest, and the encrypted copy could not be written, or the file read: this server's, for now
         PlaceRefusedUnknown,    // whether the mailbox is encrypted at rest is not known (MailboxKeys::KeyedUnknown): this server's, for now
         PlaceRefusedDamaged     // the mailbox's key row holds no key (MailboxKeys::KeyedDamaged): until an administrator acts
      };

      static bool Place(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, PlaceRefusal &refusal);

      // Place, with the mailbox's encryption given rather than asked: local
      // delivery's, which read it before it made the copy and holds a
      // recipient it could not read it for (LocalDelivery::HoldForRetry), and
      // asks again at placing - falling back to what it read before only when
      // the second read cannot say.
      static bool PlaceAs(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message,
                          MailboxKeys::Encryption encryption, const std::string &publicKey, PlaceRefusal &refusal);
      static bool PlaceWorkingFile(const String &messageFile);
      static bool PlaceWorkingFile(const String &messageFile, bool &changed);

      // PlaceWorkingFile for a file in a folder the owner has - APPEND, the
      // IMAP mirror, a migration, an import - encrypted to the owner's key when
      // its mailbox is encrypted at rest, and the plaintext then overwritten
      // with zeros (Scrub) - also when it is refused, since the caller then
      // discards it - unless keepOnRefusal: the Import Tool's file, the only
      // copy there is, stays as it was for the administrator. A null owner is
      // a public folder.
      static bool PlaceWorkingFileFor(std::shared_ptr<const Account> owner, const String &messageFile);
      static bool PlaceWorkingFileFor(std::shared_ptr<const Account> owner, const String &messageFile, bool keepOnRefusal);
      static bool PlaceWorkingFileFor(std::shared_ptr<const Account> owner, const String &messageFile, bool keepOnRefusal, PlaceRefusal &refusal);

      // PlaceWorkingFileFor with the owner's encryption given rather than
      // asked (MailboxKeys::EncryptionFor): the importer's, which asks once and
      // keeps what it placed by the same answer.
      static bool PlaceWorkingFileAs(std::shared_ptr<const Account> owner, const String &messageFile, MailboxKeys::Encryption encryption,
                                     const std::string &publicKey, bool keepOnRefusal, PlaceRefusal &refusal);

      // For the writers that put a message in a mailbox without placing it -
      // a message or an .eml put into a folder over REST: the bytes written
      // under the message's working file, encrypted to the account's key in
      // memory when its mailbox is encrypted at rest - so the plaintext never
      // reaches the disk - and otherwise as they are, as those writers have
      // always written them. False when they could not be written, or must not
      // be: whether the mailbox is encrypted could not be read, or the
      // message could not be stored encrypted (HM7365, reported).
      static bool WriteForAccount(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, const AnsiString &bytes);
      static bool WriteForAccount(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, const AnsiString &bytes, PlaceRefusal &refusal);

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

      // The working file encrypted to publicKey in place, and its plaintext
      // overwritten with zeros; false, reported, when it could not be and
      // must not be kept, with the refusal - and the plaintext overwritten
      // then too unless keepOnRefusal.
      static bool PlaceEncrypted_(const String &messageFile, const std::string &publicKey, bool keepOnRefusal, PlaceRefusal &refusal);

      // HM7365 for a message whose mailbox's encryption could not be read, and
      // for one whose mailbox's key row holds no key.
      static void ReportEncryptionUnknown_(const String &messageFile, const String &where);
      static void ReportEncryptionDamaged_(const String &messageFile, const String &where);

      static bool Encode_(const std::string &original, bool compress, unsigned __int64 dedupMinimumBytes,
                          std::string &container, std::vector<MessageStoreReference> &references, String &problem);
   };
}
