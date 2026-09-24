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

Four rules:

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
              added to a listed file, a size taken from a file, are each refused;
              the same call written in a comment or a string is not.

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
      "spamd is handed a plain file by path, through MessageStore::PlainFileFor"),
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
   "Common/Util/MessageExport.cpp": (1,
      "copied out as a plain .eml through MessageStore::CopyOriginal"),
   "Common/Util/MessageUtilities.cpp": (1,
      "a copy whose row could not be written is removed through MessageStore::Remove, and named in the report"),
   "Common/Util/PersonExport.cpp": (1,
      "read through MessageStore::ReadAll into the export's mbox"),
   "Common/Util/PushNotifier.cpp": (2,
      "the note carries the name to PersistentMessage::LoadHeader, for the subject a push shows"),
   "Common/Util/RecoverableStore.cpp": (1,
      "the recover-deleted store keeps the file as it is stored, through MessageStore::CopyStored"),
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

# The doors the reasons above describe. (file, text, what it means when it is gone)
REQUIRED = [
   ("Common/Persistence/MessageStore.h", "static String WorkingFile(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message);",
    "the working-file door is gone, so the queue and delivery sites have nowhere to ask"),
   ("Common/Persistence/PersistentMessage.cpp", "if (!MessageStore::CopyOriginal(sourceFile, destinationFile))",
    "CopyToQueue no longer gives the queue the original bytes - the scanners, SpamAssassin and the event scripts read the queue file as RFC 822"),
   ("Common/Persistence/PersistentMessage.cpp", "if (!MessageStore::CopyStored(sourceFile, destinationFile))",
    "CopyToIMAPFolder no longer copies through the store, so a copy is not counted as a name of what it references"),
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


def check(sources):
   problems = []
   counts = {}

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

   for rel, (allowed, reason) in sorted(LISTED.items()):
      made = counts.get(rel, 0)
      if made > allowed:
         problems.append("%s: %d call(s) of PersistentMessage::GetFileName where %d are listed (%s) - a new one is "
                         "a decision: list it with its reason, or use the store" % (rel, made, allowed, reason))
      elif made < allowed:
         problems.append("build/check-message-store-seam.py: LISTED allows %s %d call(s) and it makes %d - "
                         "bring the number down, so the list says what is there" % (rel, allowed, made))

   for rel, text, meaning in REQUIRED:
      path = sources.get(rel)
      if path is None or text not in read(path):
         problems.append("%s: '%s' is gone: %s" % (rel, text, meaning))

   return problems, counts


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
         failures, _ = check(mutated)
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
   failures, counts = check(sources)
   failures = problems + failures

   if failures:
      print("Message store seam: %d problem(s)" % len(failures))
      for failure in failures:
         print("  " + failure)
      return 1

   working = 0
   for rel, path in sources.items():
      working += strip(read(path)).count("MessageStore::WorkingFile(")

   print("Message store seam: PersistentMessage::GetFileName is the store's and %d listed call(s) in %d file(s), each with "
         "its reason; %d working-file call(s); no size taken from a file; %d door(s) in place; and the negative controls "
         "(a call in a file not listed, one more in a listed file, a size from a file) are refused while a call in a "
         "comment or a string is not"
         % (sum(counts.values()), len(counts), working, len(REQUIRED)))
   return 0


if __name__ == "__main__":
   sys.exit(main())
