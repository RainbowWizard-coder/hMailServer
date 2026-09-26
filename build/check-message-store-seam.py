#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A message's bytes are reached through the message store, and nowhere else.

The store (Common/Persistence/MessageStore.h) is the one door to a message's
file: where it is, and every way it is read, sized, copied, moved and deleted.
A file the store keeps can hold something other than the message as it arrived
- compressed, with a large part held once elsewhere, encrypted to its owner's
key - and a caller that reads the path with File, a std::ifstream or
FileUtilities reads garbage and sizes it wrongly, silently, for every message
the store has changed and none of the others. That is the defect this refuses,
at the one point a line checker can see: where the path comes from.

Five rules:

  name        PersistentMessage::GetFileName is called by the store unit
              (Common/Persistence/MessageStore*.cpp and PersistentMessage.cpp,
              which defines it) and by the files in LISTED below, each with the
              number of calls it makes and the reason it must have the stored
              file's name itself. Everything else asks MessageStore::WorkingFile
              for a working file - a queue file, a copy being delivered, a file
              for a row not written yet, which the store never changes - or uses
              the store's doors by message. A call in a file not listed, or one
              more call than a listed file is allowed, is refused: a new site is
              a decision, and the list is where it is made.

  working     MessageStore::WorkingFile(account, message), the form that names
              an account's folder - where a stored message is, or is about to be
              - is the same path under another name, so it is held to the same
              rule: called from the files in LISTED_WORKING only, each with its
              count and its reason. The one-argument form names the queue (or a
              public folder, for a message that has one and no account), which a
              reader of the queue needs by path. A new two-argument call is a
              decision about a stored message's file, made in the list.

  size        A message's size is never taken from its file with
              FileUtilities::FileSize: SetSize(FileUtilities::FileSize(...)) is
              refused anywhere. MessageStore::FileSize answers the message's
              size, which is what IMAP's RFC822.SIZE, POP3's octet counts and
              the quota promise; the file's own size is what the disk holds, and
              the two differ for every message the store has changed.

  doors       The doors the listed sites were moved onto must still be there, or
              the list's reasons describe code that no longer exists: the queue
              gets a message's original bytes (CopyToQueue), POP3 and IMAP FETCH
              stream through the store's reader and ranges, and the others in
              REQUIRED.

  controls    The checker proves its own negative controls on every run, in
              temporary copies: a call added to a file not listed, a second call
              added to a listed file, a two-argument WorkingFile call added to a
              file not listed, a size taken from a file, are each refused; the
              same call written in a comment or a string is not.

Run from the repository root: python3 build/check-message-store-seam.py
"""
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")

SYMBOL = "PersistentMessage::GetFileName("

# The store unit: every call it likes.
STORE_UNIT_PREFIX = "Common/Persistence/MessageStore"
STORE_UNIT = {
   "Common/Persistence/PersistentMessage.cpp",   # defines GetFileName, and uses it for its own copies, moves and deletes, through the store's doors
   "Common/Persistence/PersistentMessage.h",
}

# file -> (the calls it makes, why it must have the stored file's name).
LISTED = {
   "COM/InterfaceMessage.cpp": (2,
      "Message.Filename answers the stored file's path, which is a container when the store compresses "
      "(docs/MessageStore.md says so); Message.Save rewrites the message through MessageData::Write"),
   "Common/AntiSpam/SpamAssassin/SpamAssassinLearner.cpp": (1,
      "spamd is handed a plain file by path, through MessageStore::PlainFileFor - which writes none for a message of a "
      "mailbox encrypted at rest, so such a move teaches spamd nothing"),
   "Common/BO/MessageData.cpp": (1,
      "MessageData::LoadFromMessage(account, message) is the store's MIME loader: it resolves the pair and reads "
      "through MessageStore"),
   "Common/Persistence/PersistentMessageIndex.cpp": (1,
      "the full-text indexer carries the name in IndexableMessage to MessageData::LoadFromMessage and "
      "PersistentMessage::LoadHeader, which read through the store"),
   "Common/Persistence/PersistentMessageMetaData.cpp": (1,
      "the metadata indexer carries the name in MessageInfo to PersistentMessage::LoadHeader"),
   "Common/Rules/RuleGuard.cpp": (1,
      "names in its report the file MessageData::LoadFromMessage(account, message) read - the same derivation, so "
      "the two agree - and sizes it through MessageStore::FileSize"),
   "Common/Util/MailImporter.cpp": (1,
      "a key for the messages already in a folder, from PersistentMessage::LoadHeader and MessageStore::FileSize64; "
      "never opened otherwise"),
   "Common/Scripting/ScriptBinding.cpp": (2,
      "the JavaScript twin of COM InterfaceMessage: Message.Filename answers the stored file's path and "
      "Message.Save rewrites the message through MessageData::Write, as COM's do"),
   "Common/Util/DiscoverySearch.cpp": (2,
      "a discovery search and its export name each message by its stored file and read it only through "
      "MessageStore (Exists, FileSize64, ReadAll), PersistentMessage::LoadHeader and MessageData::LoadFromMessage"),
   "Common/Util/MessageExport.cpp": (1,
      "copied out as a plain .eml through MessageStore::CopyOriginal"),
   "Common/Util/MessageUtilities.cpp": (1,
      "a copy whose row could not be written is removed through MessageStore::Remove, and named in the report"),
   "Common/Util/PersonExport.cpp": (1,
      "read through MessageStore::ReadAll into the export's mbox"),
   "Common/Util/PushNotifier.cpp": (2,
      "the note carries the name to PersistentMessage::LoadHeader, for the subject a push shows"),
   "Common/Util/RecoverableStore.cpp": (1,
      "the recover-deleted store keeps the file through MessageStore::CopyIntoAccount: as it is stored, or encrypted to "
      "the account's key when its mailbox is encrypted at rest"),
   "Common/Util/RestApiAccountResources.cpp": (2,
      "the message resource reports the stored path as COM's Message.Filename does, and reads the header and the "
      "original bytes through PersistentMessage::LoadHeader and MessageStore::ReadAll"),
   "Common/Util/RestApiMessageRecall.cpp": (1,
      "MessageRecall::MessageIdOf reads the header through PersistentMessage::LoadHeader"),
   "Common/Util/RestApiServer.cpp": (2,
      "RestApiServer::MessageFile_, the webmail's name for a stored message, read only through the store's readers "
      "(PersistentMessage::LoadHeader, MessageData::LoadFromMessage, MessageStore::ReadAll, Imip::FindInFile)"),
   "IMAP/IMAPCommandSearch.cpp": (1,
      "SEARCH threads the name to MessageData::LoadFromMessage and PersistentMessage::LoadHeader"),
   "IMAP/IMAPFetch.cpp": (2,
      "FETCH reads the header, the MIME tree and BODY[] through PersistentMessage::LoadHeader, "
      "MessageStore::LoadMime and MessageStore::ReadRange"),
   "POP3/POP3Connection.cpp": (2,
      "RETR and TOP stream the message through MessageStoreReader"),
   "SMTP/LocalDelivery.cpp": (2,
      "after the save, MessageRecall::Delivered and Imip::FileOnDelivery read the placed copy through "
      "PersistentMessage::LoadHeader, MessageStore::ReadAll and MessageStore::LoadMime"),
}

WORKING_SYMBOL = "MessageStore::WorkingFile("

# file -> (the two-argument WorkingFile calls it makes, why it holds an account
# folder's path by name). What the next rows - an object store, encryption at
# rest - audit: each is a place a key-based backend hands a local file to.
LISTED_WORKING = {
   "Common/Application/MailboxRestore.cpp": (2,
      "the name a restored message is copied to from the archive, through MessageStore::CopyIntoAccount, which counts "
      "what it references and encrypts it to the account's key when its mailbox is encrypted at rest - and a fresh "
      "name when that one is taken"),
   "Common/BO/MessageData.cpp": (1,
      "MessageDataTester writes its self-test message for a row it then saves"),
   "Common/Util/ConfidentialMessages.cpp": (1,
      "the owner's copy of a kept confidential message, made through MessageStore::CopyIntoAccount - encrypted to the "
      "owner's key when the mailbox is encrypted at rest"),
   "Common/Util/ExternalSenderTagger.cpp": (1,
      "tags the delivery copy by path before it is placed"),
   "Common/Util/ImpersonationCheck.cpp": (1,
      "reads the delivery copy's header by path before it is placed"),
   "Common/Util/MailImporter.cpp": (1,
      "the account folder a migrated message is moved into, and placed there through MessageStore::PlaceWorkingFileFor, "
      "encrypted to the account's key when its mailbox is encrypted at rest"),
   "Common/Util/RecoverableStore.cpp": (1,
      "the name a recovered message is put back under, through MessageStore::CopyIntoAccount - encrypted to the "
      "account's key when its mailbox is encrypted at rest"),
   "Common/Util/RestApiAccountResources.cpp": (1,
      "a message written into a folder over REST for a new row, its line ends made CRLF - so no bare LF, and so never "
      "the container's magic, whose last byte is one - through MessageStore::WriteForAccount, which encrypts it in "
      "memory when the mailbox is encrypted at rest"),
   "Common/Util/RestApiScheduled.cpp": (1,
      "an .eml put into a folder over REST for a new row, its line ends made CRLF as above, and written as above"),
   "Common/Util/RestApiServer.cpp": (1,
      "the webmail's draft, written by MessageData::WriteToMailbox, which never writes a message that begins with the "
      "magic, and encrypts it in memory when the mailbox is encrypted at rest"),
   "Common/Util/ZeroAccess.cpp": (1,
      "a zero-access mailbox's Sent copy, sealed by path by this server's own writer"),
   "ExternalFetcher/IMAPClientConnection.cpp": (1,
      "the account folder the IMAP mirror files a collected message into, placed there through "
      "MessageStore::PlaceWorkingFileFor"),
   "IMAP/IMAPCommandAppend.cpp": (1,
      "the file APPEND writes the literal into, placed through MessageStore::PlaceWorkingFileFor when it is complete"),
   "SMTP/LocalDelivery.cpp": (16,
      "the delivery copy, read and rewritten by path by the rules, Sieve, the forward and the seal, placed through "
      "PlaceCopy_ and discarded through MessageStore::DiscardWorkingFile when it is not"),
   "SMTP/RuleApplier.cpp": (3,
      "rules rewrite the delivery copy by path before it is placed, and name a copy for the queue"),
   "SMTP/SMTPForwarding.cpp": (2,
      "the forward reads the delivery copy before it is placed"),
   "SMTP/SMTPVacationMessageCreator.cpp": (1,
      "the auto-reply reads the delivery copy's header before it is placed"),
}

# The doors the reasons above describe. (file, text, what it means when it is gone)
REQUIRED = [
   ("Common/Persistence/MessageStore.h", "static String WorkingFile(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);",
    "the working-file door is gone, so the queue and delivery sites have nowhere to ask"),
   ("Common/Persistence/PersistentMessage.cpp", "if (!MessageStore::CopyOriginal(sourceFile, destinationFile))",
    "CopyToQueue no longer gives the queue the original bytes - the scanners, SpamAssassin and the event scripts read the queue file as RFC 822"),
   ("Common/Persistence/PersistentMessage.cpp", "if (!MessageStore::CopyIntoAccount(sourceFile, destinationFile, destinationAccount))",
    "CopyToIMAPFolder no longer copies through the store, so a copy is not counted as a name of what it references - "
    "and a copy between a mailbox encrypted at rest and another is left under a key its new owner cannot open, or in "
    "the clear in an encrypted mailbox"),
   ("Common/Persistence/PersistentMessage.cpp", "bool bResult = MessageStore::Remove(messageFile);",
    "a message's file is deleted around the store, so what it alone referenced is never let go of"),
   ("POP3/POP3Connection.h", "MessageStoreReader current_file_;",
    "POP3 RETR no longer streams through the store's reader"),
   ("POP3/POP3Connection.cpp", "MessageStoreReader file;",
    "POP3 TOP no longer reads through the store's reader"),
   ("IMAP/IMAPFetch.cpp", "MessageStore::ReadRange(messageFileName, &buffer[0], iByteStart, iByteCount);",
    "IMAP FETCH BODY[] no longer reads through the store"),
   ("IMAP/IMAPFetch.cpp", "MessageStore::LoadMime(fileName, *pMimeBody)",
    "IMAP FETCH no longer parses the message through the store"),
   ("Common/Util/Imip.cpp", "MessageStore::LoadMime(Utf8Of(fileName), *body)",
    "a calendar reply filed at delivery is no longer read through the store"),
   ("SMTP/LocalDelivery.cpp", "accountLevelMessage->SetSize(MessageStore::Size(account, accountLevelMessage));",
    "a delivered copy's size is no longer the message's size"),
   ("SMTP/LocalDelivery.cpp", "if (!PlaceCopy_(account, accountLevelMessage, refusal))",
    "a delivered copy is no longer stored in the store's form - so compression and de-duplication never happen, and a "
    "copy that begins with the magic is kept to be read as a container"),
   ("SMTP/LocalDelivery.cpp", "if (!MessageStore::PlaceWorkingFile(copy, changed))",
    "the first DeliveryHardLinks copy is no longer placed where it is, so a container is written beside the queue again"),
   ("SMTP/LocalDelivery.cpp", "MessageStore::DiscardWorkingFile(MessageStore::WorkingFile(account, accountLevelMessage));",
    "a delivery copy that was never placed is removed through Remove, which lets go of what its first bytes claim"),
   ("IMAP/IMAPCommandAppend.cpp", "if (!MessageStore::PlaceWorkingFileFor(messageOwner, message_file_name_, false, refusal))",
    "an appended message is no longer stored in the store's form, and a literal that begins with the magic is kept to "
    "be read as a container - naming, and on EXPUNGE letting go of, blobs other accounts share; and one appended to a "
    "mailbox encrypted at rest is kept in the clear"),
   ("IMAP/IMAPCommandAppend.cpp", "MessageStore::DiscardWorkingFile(message_file_name_);",
    "an APPEND the store refused is removed through Remove, which lets go of what its first bytes claim"),
   ("ExternalFetcher/IMAPClientConnection.cpp", "if (!MessageStore::PlaceWorkingFileFor(owner, fileName))",
    "the IMAP mirror files a remote server's literal without placing it, so one that begins with the magic is read as "
    "a container, and one for a mailbox encrypted at rest is kept in the clear"),
   ("Common/Util/MailImporter.cpp", "if (!MessageStore::PlaceWorkingFileFor(owner, destination))",
    "a migrated message is filed without placing it, so one that begins with the magic is read as a container"),
   ("Common/Util/MailImporter.cpp", "if (!MessageStore::PlaceWorkingFileAs(owner, newFullPath, encryption, publicKey, true, refusal))",
    "an imported file is adopted without placing it"),
   ("Common/Util/MailImporter.cpp", "if (!MessageStore::LocalBackend().WriteAtomically(newFullPath, original))",
    "an import whose row could not be saved leaves its file in the stored form, which every later import refuses (HM7354) "
    "- for a mailbox encrypted at rest, a message nobody can read back to import; and on the local disk, where the "
    "next import looks for it, not handed to an object store's committer, which would take it off the disk"),
   # Mailbox encryption at rest (docs/EncryptionAtRest.md): the doors that keep
   # an encrypted mailbox's messages out of the clear, and its words out of the
   # index.
   ("SMTP/LocalDelivery.cpp", "if (MailboxKeys::EncryptionFor(account, encryptionKey) != MailboxKeys::EncryptionNone)",
    "a delivery shares one hard-linked file between a mailbox encrypted at rest and others, and the copy that has to be "
    "under one account's key is everybody's file"),
   ("Common/BO/MessageData.cpp", "if (!WriteEncrypted_(fileName, source_->GetEncryptedTo(), MailboxKeys::CodeReEncrypt))",
    "a rewrite of an encrypted mailbox's message - COM's Message.Save, a rule's - is written in the clear"),
   ("Common/BO/MessageData.cpp", "? mime_mail_->LoadFromMemory(source_->GetOriginal(), message_file_name_, source_->GetEncryptedTo().empty())",
    "MessageData parses a container from a file, which for a mailbox encrypted at rest is a plaintext copy on the disk "
    "for every search, preview, webmail view, rule and script that reads it"),
   ("Common/Persistence/MessageStore.cpp", "if (read && !MessageStoreFormat::EncryptedTo(header).empty())",
    "a plain copy of a message of a mailbox encrypted at rest is written to the temporary directory for another process"),
   ("Common/Util/RestApiServer.cpp", "if (!messageData.WriteToMailbox(fileName, account, encryptionUnknown, keyDamaged))",
    "a webmail draft of a mailbox encrypted at rest is written in the clear"),
   ("Common/Util/RestApiServer.cpp", "const bool added = messageData.GetAttachments()->AddFromMemory(name, bytes, type);",
    "a webmail draft's or Sent copy's attachments are written to the temporary directory in the clear on their way into "
    "a mailbox encrypted at rest"),
   ("COM/InterfaceMessage.cpp", "GetMessageData_()->WriteToMailbox(fileName, account)",
    "a message COM adds to a folder of a mailbox encrypted at rest (Messages.Add, then Save) is written in the clear"),
   ("Common/Scripting/ScriptBinding.cpp", "holder->Data()->WriteToMailbox(fileName, account)",
    "a message an event script adds to a folder of a mailbox encrypted at rest is written in the clear"),
   ("Common/Util/RestApiAccountResources.cpp", "if (!MessageStore::WriteForAccount(account, message, text, refusal))",
    "a message put into a folder of a mailbox encrypted at rest over REST is written in the clear"),
   ("Common/Util/RestApiScheduled.cpp", "if (!MessageStore::WriteForAccount(account, message, text, refusal))",
    "an .eml put into a folder of a mailbox encrypted at rest over REST is written in the clear"),
   ("Common/Application/MailboxRestore.cpp", "if (!MessageStore::CopyIntoAccount(source, destination, account))",
    "a mailbox restore puts a message archived before the switch-on back in the clear into a mailbox encrypted at rest"),
   ("Common/Util/RecoverableStore.cpp", "if (!MessageStore::CopyIntoAccount(source, target, account))",
    "a deleted item of a mailbox encrypted at rest - one a legal hold keeps, above all - is kept in the clear"),
   ("Common/Util/RecoverableStore.cpp", "if (!MessageStore::CopyIntoAccount(kept, fileName, account))",
    "a deleted item is put back in the clear into a mailbox encrypted at rest"),
   ("Common/Application/MessageIndexer.cpp", "const MailboxKeys::Keyed keyed = MailboxKeys::KeyedState(messageToIndex.AccountID);",
    "the full-text index stores the words of a mailbox encrypted at rest, in the clear, beside the files that hide them"),
   ("Common/Application/MessageIndexer.cpp", "if (MailboxKeys::KeyedState(accountId) == MailboxKeys::KeyedYes || MailboxKeys::KeyedState(accountId) == MailboxKeys::KeyedDamaged)",
    "an indexer pass that read no key just before a switch-on leaves standing the words it wrote after it"),
   ("SMTP/LocalDelivery.cpp", "held_for_keys_.push_back(pRecipient);",
    "a delivery to a mailbox whose encryption at rest is not known is made on a guess, or refused, rather than held in "
    "the queue for a later attempt"),
   ("SMTP/SMTPDeliverer.cpp", "messageRescheduled = LocalDelivery::HoldForRetry(pMessage, localDeliverer.GetHeldForKeys(), messageRescheduled, saErrorMessages);",
    "local recipients held because their mailbox's encryption at rest is not known are dropped with the queue row "
    "rather than tried again"),
   ("Common/Persistence/MessageStore.cpp", "const String fileName = String(entry->path().filename().wstring());",
    "the start's sweep reads the whole path for its marks, and zeroes or removes every message of a keyed mailbox whose "
    "name, domain or data directory carries one"),
   ("SMTP/LocalDelivery.cpp", "if (encryption == MailboxKeys::EncryptionDamaged)",
    "a delivery to a mailbox whose key row holds no key waits through every retry, answered as if the database had "
    "not answered, rather than being refused at once with the way out"),
   ("Common/Application/Application.cpp", "MailboxKeys::Start();",
    "the keyed set is not read at the start, and a plaintext an encryption cut short by a crash moved aside stays "
    "beside a keyed mailbox's file for good"),
   ("Common/Persistence/PersistentMessageMetaData.cpp", "NOT IN (SELECT akaccountid FROM hm_accountkeys)",
    "the sort's copy of the headers - From, To, Cc, Subject - is written for a mailbox encrypted at rest, in the clear"),
   ("Common/Util/MailImporter.cpp", "MessageStoreFormat::HasMagic(start.data(), start.size())",
    "an imported file that begins with the magic is adopted, and read as a container nobody counted"),
   ("Common/BO/MessageData.cpp", "if (MessageStoreFormat::HasMagic(start.c_str(), (size_t) start.GetLength()))",
    "MessageData writes a message whose first bytes are the magic - a rewritten draft, a scheduled send - into a "
    "stored file or the queue"),
   ("Common/Persistence/MessageStore.cpp", "MessageStoreFormat::HasMagic(start->GetCharBuffer(), start->GetSize()) || !MessageStore::MayHoldContainer(name)",
    "a queue file that begins with the magic is read, sized and let go of as a container"),
   ("Common/Persistence/PersistentMessage.cpp", "MessageStoreFormat::HasMagic((const char *) buf, (size_t) nbytes) && MessageStore::MayHoldContainer(fileName)",
    "LoadHeader and LoadBody read a queue file that begins with the magic as a container"),
   ("Common/Persistence/PersistentMessage.cpp", "return MessageStore::ReadHeaderOfContainer(fileName, reportError);",
    "a compressed message's header is read as the container's bytes - the rules, the sort and the search see garbage"),
   ("Common/Persistence/PersistentMessage.cpp", "return MessageStore::ReadBodyOfContainer(fileName);",
    "a compressed message's body is read as the container's bytes"),
   ("Common/BO/MessageData.cpp", "source_ = std::shared_ptr<MessageStoreParseSource>(new MessageStoreParseSource(message_file_name_));",
    "MessageData parses a compressed message's container as if it were RFC 822"),
   ("Common/Application/BackupExecuter.cpp", "MessageStoreBlobs::DeletionHold keepBlobs(true);",
    "a restore that deletes every domain lets the blobs the restored messages reference be deleted under them"),
   ("Common/Application/MailboxRestore.cpp", "MessageStoreBlobs::DeletionHold keepBlobs(false);",
    "a replace that empties a mailbox deletes a blob only its live copies named, and then cannot put back the "
    "archived copies that need it"),
   # Object storage (docs/ObjectStorage.md): the doors that ask the store, not
   # the disk, whether a stored message is there - a message an object store
   # keeps has no file - and that never take a store that did not answer for a
   # message that is gone.
   ("Common/Persistence/PersistentMessage.cpp", "if (MessageStore::Exists(sFileName) || MessageStore::Unavailable())",
    "a message an object store keeps is taken for missing and replaced by a placeholder - and so is one the store "
    "could not be asked about"),
   ("Common/Persistence/PersistentMessage.cpp", "MessageStoreObjects::ReportMissing(sFileName);",
    "a message an object store answered 404 for is replaced by a placeholder, which the row's save hands to the "
    "committer, which puts it over the message's own key"),
   ("Common/Persistence/PersistentMessage.cpp", "MessageStore::Commit(GetFileName(owner, pMessage));",
    "a delivered message - placed on the local disk, or written by path by COM's Message.Save, a script, a webmail "
    "draft - is never handed to the object store's committer and stays on the local disk"),
   ("Common/Persistence/PersistentMessage.cpp", "return HeaderOfObject(fileName, reportError);",
    "LoadHeader opens a message an object store keeps as a file, finds none, and answers an empty header"),
   ("Common/Util/RecoverableStore.cpp", "if (!MessageStore::Exists(source) && !MessageStore::Unavailable())",
    "a deleted message an object store keeps is deleted without its kept copy - under a legal hold too"),
   ("Common/Util/RecoverableStore.cpp", "if (MessageStore::Unavailable())",
    "a kept copy the object store could not be asked about is purged as gone"),
   ("Common/Util/RecoverableStore.cpp", "if (!held && MessageStore::Unavailable())",
    "a message deleted while the object store does not answer is deleted without its recover-deleted copy"),
   ("Common/Persistence/MessageStore.cpp", "MessageStoreObjects::Placing placing;",
    "a new name is handed to the object store's committer as it is written, before its row is saved - a placing "
    "that is then refused leaves an object no row names"),
   ("Common/Persistence/MessageStoreMaintenance.cpp", "if (!ListKeptFiles(kept))",
    "an exact recount made while the object store does not answer counts too few and lowers the counts of blobs "
    "still in use, which a later release then deletes"),
   ("Common/Persistence/PersistentDomain.cpp", "MessageStoreObjects::SweepLateArrivals(directories.first, directories.second, lateMoved, lateProblem)",
    "mail delivered under a domain's old name while it was renamed is left under the old keys, unreadable"),
   ("IMAP/IMAPFetch.cpp", "if (willReadMessageFile && MessageStore::Unavailable())",
    "a FETCH whose message the object store stopped giving after the existence check answers OK with an empty "
    "ENVELOPE or BODYSTRUCTURE, which the client caches"),
   ("Common/Application/BackupExecuter.cpp", "MessageStoreObjects::StageInto(sDataBackupDir, fetched, objectProblem)",
    "a backup with messages is taken without the messages an object store keeps, and a restore of it deletes them"),
   ("Common/Persistence/MailboxMove.cpp", "MessageStore::MoveTree(oldDirectory, newDirectory, objectsMoved, objectProblem)",
    "a mailbox moved to another address leaves the messages an object store keeps under the old one, unreadable"),
   ("Common/Persistence/NameChanger.cpp", "MessageStore::MoveTree(oldDirectoryName, newDirectoryName, objectsMoved, errorMessage)",
    "an account renamed leaves the messages an object store keeps under its old name, unreadable"),
   ("Common/Persistence/MessageStore.cpp", "if (!isContainer && Backend().IsLocal(messageFile) && !MessageStoreObjects::Active())",
    "a plain message an object store keeps - or one its committer may take off the disk before SpamAssassin opens "
    "it - is handed over by a path that has no file"),
   ("Common/Persistence/MessageStore.cpp", "if (whole || !MessageStore::Backend().IsLocal(messageFile))",
    "a plain message the committer takes off the disk between the question and a parse by path is taken for one "
    "being composed, and a COM Save writes it back empty"),
   ("Common/Persistence/MessageStoreObjects.cpp", "if (LocalStampOf(aside, false) != beforeIdentity)",
    "the file set aside is compared with its change time, which rename(2) sets on Linux: every commit is taken for "
    "a changed file and put again for ever"),
   ("Common/Persistence/MessageStoreObjects.cpp", "else if (!RenameQuietly(name, aside))",
    "the committer deletes a file where it is, and a rewrite renamed over the name in the moment before is deleted "
    "with it while the store keeps the old bytes"),
   ("Common/Persistence/MessageStoreObjects.cpp", "in_flight_.find(item->first) != in_flight_.end()",
    "two committer passes put the same name, and the second one's undo removes the object the first just committed"),
   ("Common/Persistence/MailboxMove.cpp", "MessageStoreObjects::HoldTree hold(oldDirectory);",
    "a late arrival the committer puts under its old name after the old address's objects are listed is stranded "
    "there, unreadable"),
]

SIZE_FROM_FILE = re.compile(r"SetSize\s*\([^;]*FileUtilities::FileSize(64)?\s*\(")


def read(path):
   with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
      return handle.read()


def strip(text):
   """Comments and the contents of string and character literals blanked,
   line breaks kept, so a line number found in the result is the file's."""
   out = []
   i = 0
   n = len(text)
   while i < n:
      c = text[i]
      if text.startswith("//", i):
         j = text.find("\n", i)
         j = n if j < 0 else j
         out.append(" " * (j - i))
         i = j
      elif text.startswith("/*", i):
         j = text.find("*/", i + 2)
         j = n if j < 0 else j + 2
         out.append(re.sub(r"[^\n]", " ", text[i:j]))
         i = j
      elif text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
         # A raw string: R"delimiter( ... )delimiter".
         open_paren = text.find("(", i + 2)
         if open_paren < 0:
            out.append(c)
            i += 1
            continue
         delimiter = text[i + 2:open_paren]
         close = text.find(")" + delimiter + '"', open_paren + 1)
         j = n if close < 0 else close + len(delimiter) + 2
         out.append(re.sub(r"[^\n]", " ", text[i:j]))
         i = j
      elif c == '"':
         j = i + 1
         while j < n and text[j] != '"' and text[j] != "\n":
            j += 2 if text[j] == "\\" else 1
         out.append('"' + " " * (min(j, n) - i - 1) + '"')
         i = j + 1
      elif c == "'":
         j = i + 1
         while j < n and text[j] != "'" and text[j] != "\n":
            j += 2 if text[j] == "\\" else 1
         out.append("'" + " " * (min(j, n) - i - 1) + "'")
         i = j + 1
      else:
         out.append(c)
         i += 1
   return "".join(out)


def relative(path, root):
   return os.path.relpath(path, root).replace(os.sep, "/")


def load_sources(root):
   sources = {}
   for base, _, names in os.walk(root):
      for name in names:
         if name.endswith((".cpp", ".h")):
            path = os.path.join(base, name)
            sources[relative(path, root)] = path
   return sources


def in_store_unit(rel):
   return rel in STORE_UNIT or rel.startswith(STORE_UNIT_PREFIX)


def two_argument_calls(code, symbol):
   """The offsets of the calls of symbol in (stripped) code that pass two or
   more arguments: a comma at the top level of the parentheses."""
   offsets = []
   at = code.find(symbol)
   while at >= 0:
      i = at + len(symbol)
      depth = 1
      commas = 0
      while i < len(code) and depth > 0:
         c = code[i]
         if c in "([{":
            depth += 1
         elif c in ")]}":
            depth -= 1
         elif c == "," and depth == 1:
            commas += 1
         i += 1
      if commas >= 1:
         offsets.append(at)
      at = code.find(symbol, at + len(symbol))
   return offsets


def check(sources):
   problems = []
   counts = {}
   working = {}

   for rel, path in sorted(sources.items()):
      code = strip(read(path))
      for number, line in enumerate(code.split("\n"), 1):
         if SIZE_FROM_FILE.search(line):
            problems.append("%s:%d: a message's size taken from its file with FileUtilities::FileSize - "
                            "MessageStore::FileSize answers the message's size (MessageStore.h)" % (rel, number))

         found = line.count(SYMBOL)
         if not found or in_store_unit(rel):
            continue

         counts[rel] = counts.get(rel, 0) + found
         if rel not in LISTED:
            problems.append("%s:%d: PersistentMessage::GetFileName outside the message store - ask "
                            "MessageStore::WorkingFile for a working file, use the store's doors for a stored one, "
                            "or list the file in build/check-message-store-seam.py with the reason" % (rel, number))

      if in_store_unit(rel):
         continue

      for offset in two_argument_calls(code, WORKING_SYMBOL):
         working[rel] = working.get(rel, 0) + 1
         if rel not in LISTED_WORKING:
            problems.append("%s:%d: MessageStore::WorkingFile(account, message) - an account folder's path - outside "
                            "the files LISTED_WORKING names: read a stored message through the store's doors, or list "
                            "the file in build/check-message-store-seam.py with the reason it holds the path"
                            % (rel, code.count("\n", 0, offset) + 1))

   for rel, (allowed, reason) in sorted(LISTED.items()):
      made = counts.get(rel, 0)
      if made > allowed:
         problems.append("%s: %d call(s) of PersistentMessage::GetFileName where %d are listed (%s) - a new one is "
                         "a decision: list it with its reason, or use the store" % (rel, made, allowed, reason))
      elif made < allowed:
         problems.append("build/check-message-store-seam.py: LISTED allows %s %d call(s) and it makes %d - "
                         "bring the number down, so the list says what is there" % (rel, allowed, made))

   for rel, (allowed, reason) in sorted(LISTED_WORKING.items()):
      made = working.get(rel, 0)
      if made > allowed:
         problems.append("%s: %d two-argument call(s) of MessageStore::WorkingFile where %d are listed (%s) - a new "
                         "one is a decision: list it with its reason, or use the store" % (rel, made, allowed, reason))
      elif made < allowed:
         problems.append("build/check-message-store-seam.py: LISTED_WORKING allows %s %d call(s) and it makes %d - "
                         "bring the number down, so the list says what is there" % (rel, allowed, made))

   for rel, text, meaning in REQUIRED:
      path = sources.get(rel)
      if path is None or text not in read(path):
         problems.append("%s: '%s' is gone: %s" % (rel, text, meaning))

   return problems, counts, working


def self_test(sources):
   """The negative controls, each in a temporary copy of the file it mutates."""
   problems = []
   temp = tempfile.mkdtemp(prefix="check-message-store-seam-")
   try:
      def mutated_with(rel, appended, sub):
         copy = os.path.join(temp, sub, rel.replace("/", os.sep))
         os.makedirs(os.path.dirname(copy), exist_ok=True)
         shutil.copyfile(sources[rel], copy)
         with open(copy, "a", encoding="utf-8") as handle:
            handle.write(appended)
         mutated = dict(sources)
         mutated[rel] = copy
         failures, _, _ = check(mutated)
         return failures

      call = ("\nnamespace HM\n{\n   String\n   NegativeControl_(std::shared_ptr<Message> message)\n   {\n"
              "      return PersistentMessage::GetFileName(message);\n   }\n}\n")

      # A call in a file that is not listed.
      failures = mutated_with("SMTP/QuotaWarner.cpp", call, "unlisted")
      if not any(f.startswith("SMTP/QuotaWarner.cpp:") and "outside the message store" in f for f in failures):
         problems.append("self-test: a PersistentMessage::GetFileName call added to a file not listed was not refused (%r)" % failures[:3])

      # One more call than a listed file is allowed.
      failures = mutated_with("IMAP/IMAPFetch.cpp", call, "listed")
      if not any(f.startswith("IMAP/IMAPFetch.cpp:") and "where 2 are listed" in f for f in failures):
         problems.append("self-test: a second PersistentMessage::GetFileName call in a listed file was not refused (%r)" % failures[:3])

      # An account folder's path asked for in a file not listed, and one more in
      # a listed file; the one-argument form, the queue's, is not refused.
      working = ("\nnamespace HM\n{\n   String\n   NegativeControlWorking_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message)\n   {\n"
                 "      return MessageStore::WorkingFile(account, message);\n   }\n}\n")
      failures = mutated_with("SMTP/QuotaWarner.cpp", working, "working")
      if not any(f.startswith("SMTP/QuotaWarner.cpp:") and "LISTED_WORKING" in f for f in failures):
         problems.append("self-test: a two-argument MessageStore::WorkingFile call added to a file not listed was not refused (%r)" % failures[:3])

      failures = mutated_with("IMAP/IMAPCommandAppend.cpp", working, "working-listed")
      if not any(f.startswith("IMAP/IMAPCommandAppend.cpp:") and "two-argument call(s)" in f and "where 1 are listed" in f for f in failures):
         problems.append("self-test: a second two-argument MessageStore::WorkingFile call in a listed file was not refused (%r)" % failures[:3])

      queue = ("\nnamespace HM\n{\n   String\n   NegativeControlQueue_(std::shared_ptr<Message> message)\n   {\n"
               "      return MessageStore::WorkingFile(message);\n   }\n}\n")
      failures = mutated_with("SMTP/QuotaWarner.cpp", queue, "queue")
      if any(f.startswith("SMTP/QuotaWarner.cpp:") for f in failures):
         problems.append("self-test: a one-argument MessageStore::WorkingFile call - the queue's - was refused (%r)" % failures[:3])

      # A size taken from a file.
      size = "\nnamespace HM\n{\n   void\n   NegativeControlSize_(std::shared_ptr<Message> message, const String &fileName)\n   {\n" \
             "      message->SetSize(FileUtilities::FileSize(fileName));\n   }\n}\n"
      failures = mutated_with("SMTP/QuotaWarner.cpp", size, "size")
      if not any(f.startswith("SMTP/QuotaWarner.cpp:") and "size taken from its file" in f for f in failures):
         problems.append("self-test: a message's size taken from its file was not refused (%r)" % failures[:3])

      # The same call in a comment and in a string is no call.
      quiet = "\n// PersistentMessage::GetFileName(message) in a comment\n" \
              "/* PersistentMessage::GetFileName(message) in a block */\n" \
              "static const char *NegativeControlText_ = \"PersistentMessage::GetFileName(message)\";\n"
      failures = mutated_with("SMTP/QuotaWarner.cpp", quiet, "quiet")
      if any(f.startswith("SMTP/QuotaWarner.cpp:") for f in failures):
         problems.append("self-test: a call written in a comment or a string was refused (%r)" % failures[:3])
   finally:
      shutil.rmtree(temp, ignore_errors=True)
   return problems


def main():
   sources = load_sources(SERVER)
   problems = self_test(sources)
   failures, counts, working = check(sources)
   failures = problems + failures

   if failures:
      print("Message store seam: %d problem(s)" % len(failures))
      for failure in failures:
         print("  " + failure)
      return 1

   queue = 0
   for rel, path in sources.items():
      if not in_store_unit(rel):
         queue += strip(read(path)).count(WORKING_SYMBOL)
   queue -= sum(working.values())

   print("Message store seam: PersistentMessage::GetFileName is the store's and %d listed call(s) in %d file(s), each with "
         "its reason; %d account-folder working-file call(s) in %d listed file(s), each with its reason, and %d of the "
         "queue's; no size taken from a file; %d door(s) in place; and the negative controls (a call in a file not "
         "listed, one more in a listed file, an account folder's working file asked for in a file not listed and one "
         "more in a listed file, a size from a file) are refused while a call in a comment or a string is not"
         % (sum(counts.values()), len(counts), sum(working.values()), len(working), queue, len(REQUIRED)))
   return 0


if __name__ == "__main__":
   sys.exit(main())
