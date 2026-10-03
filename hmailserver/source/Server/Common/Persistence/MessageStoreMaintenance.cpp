// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "MessageStoreMaintenance.h"
#include "MessageStore.h"
#include "MessageStoreObjects.h"
#include "MessageStoreBlobs.h"
#include "MessageStoreFormat.h"
#include "PersistentAccount.h"
#include "PersistentDomain.h"
#include "PersistentMessage.h"

#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"
#include "../Util/GUIDCreator.h"
#include "../Util/MailboxCrypto.h"
#include "../Util/MailboxKeys.h"
#include "../Util/Unicode.h"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>

#ifdef HM_PLATFORM_POSIX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Whether this process has more rights than the service whose database it
      // reads: on Windows a command an administrator runs, whose logger's writes
      // are forbidden (hMailServer.cpp, ForbidServiceFolderWrites); on Linux one
      // run as root. Only there is a file name the database gives held to the data
      // directory (MessageStoreMaintenance::IsInDataDirectory): the service itself
      // reads the full path an old row may hold, with its own rights, as it always
      // has, and a recount it makes after a restore counts that file too.
      bool HoldsNamesToTheDataDirectory()
      {
#ifdef HM_PLATFORM_POSIX
         return ::geteuid() == 0;
#else
         return Logger::WritesForbidden();
#endif
      }

      // How long a measurement for GET /api/v1/store is kept.
      const __int64 TotalsSeconds = 15 * 60;

      boost::mutex totals_lock;
      MessageStoreMaintenance::Totals last_totals;
      std::atomic<bool> measuring(false);

      // Set when a measurement is queued and cleared when its task goes -
      // after it ran, when it threw, or when a stopping queue dropped it
      // without running it - so that a measurement that never happened does
      // not leave the flag up and stop every later one.
      class MessageStoreMeasureTask : public Task
      {
      public:
         MessageStoreMeasureTask() : Task("MessageStoreMeasureTask") {}

         ~MessageStoreMeasureTask()
         {
            measuring = false;
         }

         virtual void DoWork()
         {
            try
            {
               MessageStoreMaintenance::MeasureNow();
            }
            catch (...)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7352, "MessageStoreMeasureTask::DoWork",
                  "The message store could not be measured; the last measurement is kept, and the next request for the totals starts another.");
            }
         }
      };

      void Print(bool error, const String &line)
      {
         AnsiString text;
         Unicode::WideToMultiByte(line, text);
         text += "\n";

#ifdef HM_PLATFORM_POSIX
         std::fputs(text.c_str(), error ? stderr : stdout);
         std::fflush(error ? stderr : stdout);
#else
         HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
         if (handle && handle != INVALID_HANDLE_VALUE)
         {
            DWORD written = 0;
            WriteFile(handle, text.c_str(), (DWORD) text.GetLength(), &written, nullptr);
         }
#endif
      }

      String Bytes(unsigned __int64 bytes)
      {
         String text;
         text.Format(_T("%I64u"), bytes);
         return text;
      }

      String Percent(unsigned __int64 part, unsigned __int64 whole)
      {
         if (whole == 0)
            return _T("0");

         String text;
         text.Format(_T("%.1f"), 100.0 * (double) part / (double) whole);
         return text;
      }

      // The files that are one file: grouped by what the backend says each is.
      struct Group
      {
         Group() : names_total(0) {}
         std::vector<String> names;
         unsigned int names_total;
      };

      void GroupByIdentity(const std::vector<MessageStoreMaintenance::StoredFile> &files, std::vector<Group> &groups, std::vector<String> &unreadable)
      {
         std::map<AnsiString, size_t> byIdentity;
         for (size_t i = 0; i < files.size(); i++)
         {
            AnsiString identity;
            unsigned int names = 0;
            if (!MessageStore::Backend().Identity(files[i].path, identity, names))
            {
               unreadable.push_back(files[i].path);
               continue;
            }

            auto known = byIdentity.find(identity);
            if (known == byIdentity.end())
            {
               Group group;
               group.names.push_back(files[i].path);
               group.names_total = names;
               byIdentity[identity] = groups.size();
               groups.push_back(group);
               continue;
            }

            // The same name met twice (two rows naming one file) is one name.
            std::vector<String> &names_found = groups[known->second].names;
            if (std::find(names_found.begin(), names_found.end(), files[i].path) == names_found.end())
               names_found.push_back(files[i].path);
         }
      }

      struct WalkCounts
      {
         WalkCounts() : rewritten(0), unchanged(0), shared_left(0), zero_access(0), encrypted(0), failed(0), before(0), after(0) {}
         int rewritten;
         int unchanged;
         int shared_left;
         int zero_access;
         int encrypted;      // left: a mailbox encrypted at rest, whose files the encrypt and decrypt verbs rewrite
         int failed;
         unsigned __int64 before;
         unsigned __int64 after;
      };

      // Whether a stored file belongs to a mailbox encrypted at rest - one with
      // a key, switched on or not - or is itself encrypted to a key. Either way
      // it is --encrypt-store's and --decrypt-store's, never the compress walk's:
      // compressing a file of such a mailbox in the clear would leave it in the
      // clear, and decompressing an encrypted one needs its key.
      bool IsEncryptedMailboxFile(const MessageStoreMaintenance::StoredFile &file)
      {
         std::string publicKey;
         if (MessageStore::IsEncrypted(file.path, publicKey))
            return true;

         return file.account_id > 0 && MailboxKeys::IsKeyed(file.account_id);
      }

      // The domain's recovery passphrase, from the environment and never from
      // the command line, which every user of the machine can read.
      AnsiString RecoveryPassphraseFromEnvironment()
      {
#ifdef HM_PLATFORM_POSIX
         const char *value = ::getenv("HMAILSERVER_RECOVERY_PASSPHRASE");
         return value ? AnsiString(value) : AnsiString();
#else
         wchar_t buffer[1024];
         const DWORD capacity = (DWORD) (sizeof(buffer) / sizeof(buffer[0]));
         const DWORD length = ::GetEnvironmentVariableW(L"HMAILSERVER_RECOVERY_PASSPHRASE", buffer, capacity);
         if (length == 0 || length >= capacity)
            return AnsiString();

         AnsiString utf8;
         Unicode::WideToMultiByte(String(buffer), utf8);
         SecureZeroMemory(buffer, sizeof(buffer));
         return utf8;
#endif
      }

      // One file, with all of its names: rewritten once beside the first name,
      // read back and compared, then every name made a name of the new file.
      bool RewriteGroup(const Group &group, bool decompress, unsigned __int64 dedupMinimum, WalkCounts &counts)
      {
         const String first = group.names[0];

         unsigned __int64 before = 0;
         MessageStore::StoredFileSize64(first, before);

         std::vector<MessageStoreReference> oldReferences;
         if (!MessageStore::GetReferences(first, oldReferences))
         {
            Print(true, "  " + first + ": its header is not one this server reads; left as it is.");
            counts.failed++;
            return false;
         }

         const bool container = MessageStore::IsContainer(first);
         if (container != decompress)
         {
            counts.unchanged++;
            counts.before += before;
            counts.after += before;
            return true;
         }

         std::string original;
         String problem;
         if (!MessageStore::ReadOriginal(first, original, problem))
         {
            Print(true, "  " + first + ": could not be read (" + problem + "); left as it is.");
            counts.failed++;
            return false;
         }

         // A plain message that begins with the magic stays a container.
         if (decompress && MessageStoreFormat::HasMagic(original.data(), original.size()))
         {
            counts.unchanged++;
            counts.before += before;
            counts.after += before;
            return true;
         }

         // Beside the first name, on its volume, so that a rename and a link can
         // reach every name.
         const String staged = first + _T(".store-") + GUIDCreator::GetGUID();

         if (decompress)
         {
            if (!MessageStore::Backend().WriteAtomically(staged, AnsiString(original.data(), original.size())))
            {
               Print(true, "  " + first + ": the plain message could not be written beside it; left as it is.");
               counts.failed++;
               return false;
            }
         }
         else
         {
            bool written = false;
            if (!MessageStore::WriteContainer(original, staged, dedupMinimum, written, problem))
            {
               Print(true, "  " + first + ": could not be compressed (" + problem + "); left as it is.");
               counts.failed++;
               return false;
            }

            if (!written)
            {
               counts.unchanged++;
               counts.before += before;
               counts.after += before;
               return true;
            }
         }

         // Read back through the store and compared with the message, byte for
         // byte - the size and the SHA-256 with it - before any name moves.
         std::string check;
         if (!MessageStore::ReadOriginal(staged, check, problem) || check != original)
         {
            MessageStore::Remove(staged);
            Print(true, "  " + first + ": what was written did not read back as the message; left as it is.");
            counts.failed++;
            return false;
         }

         unsigned __int64 after = 0;
         MessageStore::StoredFileSize64(staged, after);

         bool allMoved = true;
         if (group.names.size() == 1)
         {
            // One name: the new file takes it, and with it the counts the staged
            // name holds.
            if (!MessageStore::Rename(staged, first))
            {
               MessageStore::Remove(staged);
               allMoved = false;
            }
            else if (decompress)
               MessageStoreBlobs::Release(oldReferences);
         }
         else
         {
            for (size_t i = 0; i < group.names.size(); i++)
            {
               if (!MessageStore::Relink(staged, group.names[i]))
               {
                  allMoved = false;
                  continue;
               }

               // A name that was the container no longer references its parts.
               if (decompress)
                  MessageStoreBlobs::Release(oldReferences);
            }

            MessageStore::Remove(staged);
         }

         if (!allMoved)
         {
            Print(true, "  " + first + ": the rewritten file could not be given every name; the names it has are readable, as are the ones left.");
            counts.failed++;
            return false;
         }

         counts.rewritten++;
         counts.before += before;
         counts.after += after;
         return true;
      }

      bool OpenForCommand(const String &verb)
      {
         IniFileSettings::Instance()->LoadSettings();
         if (!IniFileSettings::Instance()->GetDatabaseSettingsExists())
         {
            Print(true, Formatter::Format("[Database] in {0} does not name a database, so there is no store to {1}. Nothing was changed.",
               IniFileSettings::GetInitializationFile(), verb));
            return false;
         }

         String error;
         if (!Application::Instance()->InitInstance(error))
         {
            if (error.IsEmpty())
               error = Application::Instance()->GetLastErrorMessage();

            Print(true, Formatter::Format("The configuration could not be read and the database opened: {0} Nothing was changed.", error));
            Application::Instance()->ExitInstance();
            return false;
         }

         return true;
      }

      int Measure(const std::vector<MessageStoreMaintenance::StoredFile> &files, const String &scope)
      {
         const unsigned __int64 dedupMinimum = (unsigned __int64) Configuration::Instance()->GetMessageStoreDedupMinKB() * 1024;

         std::vector<Group> groups;
         std::vector<String> unreadable;
         GroupByIdentity(files, groups, unreadable);

         unsigned __int64 logical = 0;
         unsigned __int64 storedNow = 0;
         unsigned __int64 wouldBe = 0;
         unsigned __int64 duplicateParts = 0;
         unsigned __int64 heldOnce = 0;
         int containers = 0;
         int names = 0;

         // Every part the walk would hold once, and every part a compressed file
         // holds once already - which is on disk now, once, however many files
         // name it.
         std::set<AnsiString> seenParts;
         std::set<AnsiString> blobsNow;

         for (size_t i = 0; i < groups.size(); i++)
         {
            const String &file = groups[i].names[0];
            names += (int) groups[i].names.size();

            unsigned __int64 size = 0;
            MessageStore::StoredFileSize64(file, size);
            storedNow += size;

            std::vector<MessageStoreReference> references;
            if (MessageStore::IsContainer(file) && MessageStore::GetReferences(file, references))
            {
               // Already stored compressed: it would take what it takes now, and
               // so would the blobs it names - on both sides of the sum, each
               // part once, so that a store compressed already shows the saving
               // still to be had, which is none, and not a loss.
               containers++;
               unsigned __int64 original = 0;
               MessageStore::FileSize64(file, original);
               logical += original;
               wouldBe += size;
               for (size_t r = 0; r < references.size(); r++)
               {
                  if (blobsNow.insert(references[r].sha256).second)
                     storedNow += references[r].length;

                  if (seenParts.insert(references[r].sha256).second)
                     heldOnce += references[r].length;
                  else
                     duplicateParts += references[r].length;
               }
               continue;
            }

            std::string original;
            String problem;
            if (!MessageStore::ReadOriginal(file, original, problem))
            {
               unreadable.push_back(file);
               continue;
            }

            logical += (unsigned __int64) original.size();

            std::vector<MessageStoreReference> parts;
            if (dedupMinimum > 0)
               MessageStoreFormat::FindLargeParts(original, dedupMinimum, parts);

            std::string container;
            if (!MessageStoreFormat::Encode(original, parts, true, container, problem) ||
                (parts.empty() && container.size() >= original.size()))
            {
               wouldBe += (unsigned __int64) original.size();
               continue;
            }

            wouldBe += (unsigned __int64) container.size();
            for (size_t r = 0; r < parts.size(); r++)
            {
               if (seenParts.insert(parts[r].sha256).second)
                  heldOnce += parts[r].length;
               else
                  duplicateParts += parts[r].length;
            }
         }

         const unsigned __int64 total = wouldBe + heldOnce;
         const unsigned __int64 saved = storedNow > total ? storedNow - total : 0;

         Print(false, Formatter::Format("Measured {0}: {1} stored files ({2} names), {3} of them compressed already; nothing was changed.",
            scope, (int) groups.size(), names, containers));
         Print(false, "  The messages:                          " + Bytes(logical) + " bytes, what IMAP and POP3 say they are.");
         Print(false, "  Stored now:                            " + Bytes(storedNow) + " bytes of message files and of the blobs the compressed ones name (a file with several names counted once).");
         Print(false, "  Compressed, parts held once:           " + Bytes(wouldBe) + " bytes of message files and " + Bytes(heldOnce) + " of blobs, " + Bytes(total) + " in all.");
         Print(false, "  Parts met more than once:              " + Bytes(duplicateParts) + " bytes that would be stored once.");
         Print(false, "  Would save:                            " + Bytes(saved) + " bytes more, " + Percent(saved, storedNow) + "% of what is stored now.");
         Print(false, Formatter::Format("  Parts held once from {0} KB (MessageStoreDedupMinKB); compression is {1} for new mail (MessageStoreCompression).",
            Configuration::Instance()->GetMessageStoreDedupMinKB(), Configuration::Instance()->GetMessageStoreCompression() ? String(_T("on")) : String(_T("off"))));

         for (size_t i = 0; i < unreadable.size(); i++)
            Print(true, "  Not read: " + unreadable[i]);

         return unreadable.empty() ? 0 : 1;
      }

      int Rewrite(const std::vector<MessageStoreMaintenance::StoredFile> &files, const String &scope, bool decompress)
      {
         const unsigned __int64 dedupMinimum = (unsigned __int64) Configuration::Instance()->GetMessageStoreDedupMinKB() * 1024;

         WalkCounts counts;

         std::vector<MessageStoreMaintenance::StoredFile> walked;
         for (size_t i = 0; i < files.size(); i++)
         {
            // A zero-access mailbox's delivered mail is ciphertext: nothing to
            // compress, no part to hold once, so compressing leaves it. It is
            // not only ciphertext, though - an APPEND is stored as the client
            // sent it, and mail stored before the switch was thrown is not
            // sealed afterwards - so --decompress walks it like any other and
            // puts back every container it finds.
            if (files[i].zero_access && !decompress)
            {
               counts.zero_access++;
               continue;
            }

            // A mailbox encrypted at rest: --encrypt-store and --decrypt-store
            // are its walk, and this one leaves it as it is.
            if (IsEncryptedMailboxFile(files[i]))
            {
               counts.encrypted++;
               continue;
            }

            walked.push_back(files[i]);
         }

         std::vector<Group> groups;
         std::vector<String> unreadable;
         GroupByIdentity(walked, groups, unreadable);

         for (size_t i = 0; i < groups.size(); i++)
         {
            // A file with a name outside the walk: rewriting it would give the
            // names here a new file and leave the others the old one, which is
            // one copy per name. Left, and counted.
            if (groups[i].names_total > (unsigned int) groups[i].names.size())
            {
               counts.shared_left++;
               continue;
            }

            RewriteGroup(groups[i], decompress, dedupMinimum, counts);
         }

         counts.failed += (int) unreadable.size();
         for (size_t i = 0; i < unreadable.size(); i++)
            Print(true, "  Not read: " + unreadable[i]);

         const String done = decompress ? String(_T("Decompressed")) : String(_T("Compressed"));
         // Two calls: Formatter::Format takes five arguments at most.
         Print(false, Formatter::Format("{0} {1} of {2} stored files in {3}: ", done, counts.rewritten, (int) groups.size(), scope) +
            Formatter::Format("{0} bytes before, {1} after.", Bytes(counts.before), Bytes(counts.after)));
         Print(false, Formatter::Format("  {0} left as they were ({1}), {2} shared with names outside the walk and left, {3} in zero-access mailboxes left, {4} failed.",
            counts.unchanged, decompress ? String(_T("plain already")) : String(_T("compressed already, or not made smaller")),
            counts.shared_left, counts.zero_access, counts.failed));
         if (counts.encrypted > 0)
            Print(false, Formatter::Format("  {0} in mailboxes encrypted at rest left: --encrypt-store and --decrypt-store rewrite those.", counts.encrypted));

         return counts.failed == 0 ? 0 : 1;
      }

      // One name of a stored file rewritten on its own: written beside itself,
      // checked, and renamed over the name. Another name of the same file - a
      // DeliveryHardLinks copy in another mailbox - keeps the file it had,
      // which is what a file several accounts share has to do when one of them
      // is encrypted. What the old file referenced is let go of for this name.
      bool RewriteName(const String &name, bool encrypt, const std::string &publicKey, WalkCounts &counts)
      {
         unsigned __int64 before = 0;
         MessageStore::StoredFileSize64(name, before);

         std::vector<MessageStoreReference> oldReferences;
         if (!MessageStore::GetReferences(name, oldReferences))
         {
            Print(true, "  " + name + ": its header is not one this server reads; left as it is.");
            counts.failed++;
            return false;
         }

         std::string original;
         String problem;
         if (!MessageStore::ReadOriginal(name, original, problem))
         {
            Print(true, "  " + name + ": could not be read (" + problem + "); left as it is.");
            counts.failed++;
            return false;
         }

         const String staged = name + _T(".store-") + GUIDCreator::GetGUID();

         bool written = false;
         if (encrypt)
            written = MessageStore::WriteEncryptedContainer(original, staged, publicKey, problem);
         else
         {
            // Back in the clear, and read back through the store and compared.
            std::string check;
            written = MessageStore::Backend().WriteAtomically(staged, AnsiString(original.data(), original.size())) &&
                      MessageStore::ReadOriginal(staged, check, problem) && check == original;
            if (!written && problem.IsEmpty())
               problem = _T("the plain message could not be written beside it and read back");
            MailboxCrypto::Forget(check);
         }

         MailboxCrypto::Forget(original);

         if (!written)
         {
            MessageStore::DiscardWorkingFile(staged);
            Print(true, "  " + name + ": could not be rewritten (" + problem + "); left as it is.");
            counts.failed++;
            return false;
         }

         unsigned __int64 after = 0;
         MessageStore::StoredFileSize64(staged, after);

         if (!MessageStore::Rename(staged, name))
         {
            MessageStore::DiscardWorkingFile(staged);
            Print(true, "  " + name + ": the rewritten file could not be renamed over its name; left as it is.");
            counts.failed++;
            return false;
         }

         // This name no longer references what the old file held once; the
         // new file references nothing.
         if (!oldReferences.empty())
            MessageStoreBlobs::Release(oldReferences);

         counts.rewritten++;
         counts.before += before;
         counts.after += after;
         return true;
      }

      int Encrypt(const std::vector<MessageStoreMaintenance::StoredFile> &files, const String &scope)
      {
         WalkCounts counts;
         int notKeyed = 0;

         std::map<__int64, std::string> keys;
         for (size_t i = 0; i < files.size(); i++)
         {
            const MessageStoreMaintenance::StoredFile &file = files[i];

            // A public folder is nobody's mailbox, and a zero-access mailbox's
            // mail is under the person's own OpenPGP key.
            if (file.account_id <= 0 || file.zero_access)
            {
               notKeyed++;
               continue;
            }

            std::map<__int64, std::string>::iterator known = keys.find(file.account_id);
            if (known == keys.end())
            {
               MailboxKeys::Row row;
               keys[file.account_id] = MailboxKeys::GetRow(file.account_id, row) ? row.public_key : std::string();
               known = keys.find(file.account_id);
            }

            if (known->second.empty())
            {
               notKeyed++;
               continue;
            }

            std::string encryptedTo;
            if (MessageStore::IsEncrypted(file.path, encryptedTo))
            {
               if (encryptedTo == known->second)
               {
                  counts.unchanged++;
                  continue;
               }

               Print(true, "  " + file.path + ": encrypted to a key that is not the account's; left as it is.");
               counts.failed++;
               continue;
            }

            RewriteName(file.path, true, known->second, counts);
         }

         // The rows of the walked accounts' deleted items: no subject or sender
         // kept in the clear beside copies that are encrypted now - as
         // RecoverableStore::Keep keeps none for a keyed mailbox.
         for (std::map<__int64, std::string>::const_iterator key = keys.begin(); key != keys.end(); ++key)
         {
            if (key->second.empty())
               continue;

            SQLCommand blank("update hm_recoverable set rcsubject = @SUBJECT, rcfrom = @SENDER where rcaccountid = @ACCOUNTID");
            blank.AddParameter("@SUBJECT", String());
            blank.AddParameter("@SENDER", String());
            blank.AddParameter("@ACCOUNTID", key->first);
            if (!Application::Instance()->GetDBManager()->Execute(blank))
               Print(true, "  The subjects and senders of the deleted items of an account could not be emptied; they stay in the database.");
         }

         Print(false, Formatter::Format("Encrypted {0} stored files in {1}: ", counts.rewritten, scope) +
            Formatter::Format("{0} bytes before, {1} after.", Bytes(counts.before), Bytes(counts.after)));
         Print(false, Formatter::Format("  {0} encrypted already, {1} not in a mailbox with a key (a public folder, a zero-access mailbox, or no key), {2} failed.",
            counts.unchanged, notKeyed, counts.failed));

         return counts.failed == 0 ? 0 : 1;
      }

      int Decrypt(const std::vector<MessageStoreMaintenance::StoredFile> &files, const String &scope, const AnsiString &passphrase)
      {
         WalkCounts counts;

         // Each account's key opened once, with the domain's passphrase, in this
         // process only; a passphrase that opens none of them changes nothing.
         std::map<__int64, std::string> opened;
         std::map<__int64, bool> refused;
         for (size_t i = 0; i < files.size(); i++)
         {
            const __int64 accountId = files[i].account_id;
            if (accountId <= 0 || opened.find(accountId) != opened.end() || refused.find(accountId) != refused.end())
               continue;

            if (!MailboxKeys::IsKeyed(accountId))
               continue;

            std::string publicKey;
            std::string privateKey;
            MailboxCrypto::ForgetOnExit forgetPrivateKey(privateKey);
            String problem;
            if (!MailboxKeys::OpenPrivateWithRecovery(accountId, passphrase, publicKey, privateKey, problem))
            {
               Print(true, "  " + files[i].account + ": its key could not be opened (" + problem + "); its files are left as they are.");
               refused[accountId] = true;
               continue;
            }

            MailboxKeys::OpenFor(accountId, publicKey, privateKey);
            MailboxCrypto::Forget(privateKey);
            opened[accountId] = publicKey;
         }

         int plain = 0;
         for (size_t i = 0; i < files.size(); i++)
         {
            std::string encryptedTo;
            if (!MessageStore::IsEncrypted(files[i].path, encryptedTo))
            {
               plain++;
               continue;
            }

            std::map<__int64, std::string>::const_iterator key = opened.find(files[i].account_id);
            if (key == opened.end() || key->second != encryptedTo)
            {
               Print(true, "  " + files[i].path + ": encrypted to a key that was not opened; left as it is.");
               counts.failed++;
               continue;
            }

            RewriteName(files[i].path, false, std::string(), counts);
         }

         Print(false, Formatter::Format("Decrypted {0} stored files in {1}: ", counts.rewritten, scope) +
            Formatter::Format("{0} bytes before, {1} after.", Bytes(counts.before), Bytes(counts.after)));
         Print(false, Formatter::Format("  {0} were not encrypted, {1} failed. Switch the mailbox's encryption off before the server starts again, or new mail is encrypted to the key once more; once nothing is encrypted to it, the key can be removed.",
            plain, counts.failed + (int) refused.size()));

         return counts.failed == 0 && refused.empty() ? 0 : 1;
      }
   }

   namespace
   {
      bool IsBelow_(const String &path, String data);

#ifndef HM_PLATFORM_POSIX
      // The data directory where it really is - through a junction or a mount
      // point an administrator made to keep it on another drive, which
      // hMailServer.exe's commands go through only when the administrators alone
      // control it (DataFolderHoldsNoLink) - as Windows names it, or empty when
      // that is where the configuration says. Asked once.
      String FinalDataDirectory_()
      {
         static bool asked = false;
         static String final_path;
         if (asked)
            return final_path;
         asked = true;

         const String data = IniFileSettings::Instance()->GetDataDirectory();
         HANDLE folder = CreateFileW(data.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS, nullptr);
         if (folder == INVALID_HANDLE_VALUE)
            return final_path;

         std::vector<wchar_t> buffer(4096);
         const DWORD length = GetFinalPathNameByHandleW(folder, &buffer[0], (DWORD) buffer.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
         CloseHandle(folder);
         if (length == 0 || length >= buffer.size())
            return final_path;

         String found(&buffer[0]);
         if (found.Left(8).CompareNoCase(_T("\\\\?\\UNC\\")) == 0)
            found = _T("\\\\") + found.Mid(8);
         else if (found.Left(4) == _T("\\\\?\\"))
            found = found.Mid(4);

         String configured = data;
         while (configured.GetLength() > 3 && (configured.Right(1) == _T("\\") || configured.Right(1) == _T("/")))
            configured = configured.Left(configured.GetLength() - 1);
         if (found.CompareNoCase(configured) != 0)
            final_path = found;
         return final_path;
      }
#endif
   }

   bool
   MessageStoreMaintenance::IsInDataDirectory(const String &path)
   {
      if (IsBelow_(path, IniFileSettings::Instance()->GetDataDirectory()))
         return true;

#ifndef HM_PLATFORM_POSIX
      // A row an older server wrote with the full path it had then, which named
      // the data directory where it really is - E:\Mail\... - before the
      // configuration named it through an administrator's junction: the same
      // files.
      const String final_path = FinalDataDirectory_();
      if (!final_path.IsEmpty() && IsBelow_(path, final_path))
         return true;
#endif

      return false;
   }

   namespace
   {
      // Whether path is below data: data, a separator, and no part below it that
      // leads out (MessageStoreMaintenance::IsInDataDirectory).
      bool IsBelow_(const String &path, String data)
      {
         while (data.GetLength() > 1 && (data.Right(1) == _T("\\") || data.Right(1) == _T("/")))
            data = data.Left(data.GetLength() - 1);

         if (data.IsEmpty() || path.GetLength() <= data.GetLength() + 1)
            return false;

#ifdef HM_PLATFORM_POSIX
         if (path.Left(data.GetLength()) != data)
            return false;
#else
         if (path.Left(data.GetLength()).CompareNoCase(data) != 0)
            return false;
#endif

         const int separator = (int) path[data.GetLength()];
         if (separator != '\\' && separator != '/')
            return false;

         // Each part below the directory, between separators.
         const String below = path.Mid(data.GetLength() + 1);
         String part;
         for (int i = 0; i <= below.GetLength(); i++)
         {
            const int c = i < below.GetLength() ? (int) below[i] : '/';
            if (c != '\\' && c != '/')
            {
               part += below.Mid(i, 1);
               continue;
            }

            if (part == _T(".."))
               return false;

#ifndef HM_PLATFORM_POSIX
            // Windows takes the dots and spaces off the end of a part: "... " is
            // "..", and a colon names a stream or a drive.
            if (part.Find(_T(":")) >= 0)
               return false;

            bool dotsAndSpaces = !part.IsEmpty();
            for (int j = 0; j < part.GetLength() && dotsAndSpaces; j++)
               dotsAndSpaces = part[j] == '.' || part[j] == ' ';
            if (dotsAndSpaces)
               return false;
#endif

            part.Empty();
         }

         return true;
      }
   }

   bool
   MessageStoreMaintenance::ListStoredFiles(const String &accountAddress, const String &domainName, std::vector<StoredFile> &files, String &problem)
   {
      files.clear();

      std::shared_ptr<DatabaseConnectionManager> db = Application::Instance()->GetDBManager();
      if (!db)
      {
         problem = _T("the database is not open");
         return false;
      }

      // The account or the domain found as a sign-in finds it - through
      // SQLStatement's where clause, which compares without regard to case on
      // PostgreSQL as the other databases do by their collation - and the walk
      // then filtered by its id rather than by the text given.
      __int64 accountId = 0;
      __int64 domainId = 0;

      if (!accountAddress.IsEmpty())
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
         if (!PersistentAccount::ReadObject(account, accountAddress))
         {
            problem = "there is no account " + accountAddress;
            return false;
         }
         accountId = account->GetID();
      }
      else if (!domainName.IsEmpty())
      {
         std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
         if (!PersistentDomain::ReadObject(domain, domainName))
         {
            problem = "there is no domain " + domainName;
            return false;
         }
         domainId = domain->GetID();
      }

      String sql = _T("select m.messageid, m.messagefilename, m.messageaccountid, m.messagefolderid, m.messagesize, a.accountaddress, a.accountzeroaccess ")
                   _T("from hm_messages m left join hm_accounts a on m.messageaccountid = a.accountid where m.messagetype = 2");
      if (accountId > 0)
         sql += _T(" and m.messageaccountid = @ACCOUNTID");
      else if (domainId > 0)
         sql += _T(" and a.accountdomainid = @DOMAINID");

      SQLCommand command(sql);
      if (accountId > 0)
         command.AddParameter("@ACCOUNTID", accountId);
      else if (domainId > 0)
         command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(command);
      if (!recordset)
      {
         problem = _T("the messages could not be read");
         return false;
      }

      const bool confined = HoldsNamesToTheDataDirectory();

      while (!recordset->IsEOF())
      {
         const String partial = recordset->GetStringValue("messagefilename");
         if (!partial.IsEmpty())
         {
            // false: a message only to name a file with, given no new name.
            std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message(false));
            message->SetPartialFileName(partial);
            message->SetAccountID(recordset->GetLongValue("messageaccountid"));
            message->SetFolderID(recordset->GetLongValue("messagefolderid"));

            StoredFile file;
            file.message_id = recordset->GetInt64Value("messageid");
            file.size = recordset->GetInt64Value("messagesize");
            file.account = recordset->GetStringValue("accountaddress");
            file.zero_access = recordset->GetLongValue("accountzeroaccess") == 1;
            file.account_id = recordset->GetInt64Value("messageaccountid");
            file.path = PersistentMessage::GetFileName(file.account, message);

            // The file name is the row's, which the service's account may write: one
            // that leads out of the data directory - a full path, or a part that is
            // ".." - is never read, rewritten, removed or created by a walk that runs
            // with more rights than the service (IsInDataDirectory).
            if (confined && !IsInDataDirectory(file.path))
            {
               files.clear();
               // A full path a server before 5.4 wrote, naming where the data
               // directory was then, is the usual one: the Data Directory
               // Synchronizer turns each into a name below the data directory.
               problem = Formatter::Format(_T("message {0} names the file {1}, which is not in the data directory {2}; a walk run with more rights than the service changes nothing outside it, since the service's account may write that name. ")
                  _T("If it is a full path an older server wrote (5.3 and before stored them), put DataFolder back to the folder it names, run the Data Directory Synchronizer (Addons\\DataDirectorySynchronizer, or the diagnostic Test message file locations to find them) there, which turns each full path into a name below the data directory, and then move the data as RelocatingAnInstallation.md says; otherwise delete the message"),
                  file.message_id, file.path, IniFileSettings::Instance()->GetDataDirectory());
               return false;
            }

            files.push_back(file);
         }

         recordset->MoveNext();
      }

      return true;
   }

   bool
   MessageStoreMaintenance::ListKeptFilesOf(const String &accountAddress, const String &domainName, std::vector<StoredFile> &files, String &problem)
   {
      files.clear();

      std::shared_ptr<DatabaseConnectionManager> db = Application::Instance()->GetDBManager();
      if (!db)
      {
         problem = _T("the database is not open");
         return false;
      }

      __int64 accountId = 0;
      __int64 domainId = 0;

      if (!accountAddress.IsEmpty())
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
         if (!PersistentAccount::ReadObject(account, accountAddress))
         {
            problem = "there is no account " + accountAddress;
            return false;
         }
         accountId = account->GetID();
      }
      else if (!domainName.IsEmpty())
      {
         std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
         if (!PersistentDomain::ReadObject(domain, domainName))
         {
            problem = "there is no domain " + domainName;
            return false;
         }
         domainId = domain->GetID();
      }

      String sql = _T("select r.rcaccountid, r.rcfile, r.rcsize, a.accountaddress, a.accountzeroaccess ")
                   _T("from hm_recoverable r inner join hm_accounts a on r.rcaccountid = a.accountid");
      if (accountId > 0)
         sql += _T(" where r.rcaccountid = @ACCOUNTID");
      else if (domainId > 0)
         sql += _T(" where a.accountdomainid = @DOMAINID");

      SQLCommand command(sql);
      if (accountId > 0)
         command.AddParameter("@ACCOUNTID", accountId);
      else if (domainId > 0)
         command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(command);
      if (!recordset)
      {
         problem = _T("the deleted items could not be read");
         return false;
      }

      const String root = FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Recoverable"));
      const bool confined = HoldsNamesToTheDataDirectory();

      while (!recordset->IsEOF())
      {
         const String relative = recordset->GetStringValue("rcfile");
         if (!relative.IsEmpty())
         {
            StoredFile file;
            file.size = recordset->GetInt64Value("rcsize");
            file.account = recordset->GetStringValue("accountaddress");
            file.zero_access = recordset->GetLongValue("accountzeroaccess") == 1;
            file.account_id = recordset->GetInt64Value("rcaccountid");
            file.path = FileUtilities::Combine(root, relative);

            // As ListStoredFiles: the row's name, never one out of the data
            // directory.
            if (confined && !IsInDataDirectory(file.path))
            {
               files.clear();
               problem = Formatter::Format(_T("a deleted message kept for recovery names the file {0}, which is not in the data directory; a walk changes nothing outside it"), file.path);
               return false;
            }

            // Asked of the store: a kept copy an object store holds has no file
            // (docs/ObjectStorage.md). One the store could not be asked about
            // is not taken for gone.
            MessageStore::ClearUnavailable();
            if (MessageStore::Exists(file.path) || MessageStore::Unavailable())
               files.push_back(file);
         }

         recordset->MoveNext();
      }

      return true;
   }

   bool
   MessageStoreMaintenance::ListKeptFiles(std::vector<String> &files)
   {
      files.clear();

      const String root = FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Recoverable"));

      // The files under the root, and on an object store the objects too
      // (docs/ObjectStorage.md).
      return MessageStore::Backend().List(root, files);
   }

   bool
   MessageStoreMaintenance::Recount(String &summary, bool exact)
   {
      // A store that has never held a part once has nothing to count - no row,
      // and no Blobs directory a stored file could name - so the walk a restore
      // ends with, which reads the start of every stored file, is not made: a
      // restore on a server that never turned compression on costs what it cost
      // before this row. The command line's recount always walks: it is also
      // how a store upgraded from before schema 6098 has every file that begins
      // with the magic counted (docs/MessageStore.md).
      if (!exact)
      {
         std::map<AnsiString, __int64> rows;
         if (MessageStoreBlobs::List(rows) && rows.empty() && !FileUtilities::DirectoryExists(MessageStoreBlobs::Root()))
         {
            summary = _T("the store holds no part once, so there was nothing to count.");
            return true;
         }
      }

      std::vector<StoredFile> files;
      if (!ListStoredFiles(String(), String(), files, summary))
         return false;

      std::vector<String> names;
      for (size_t i = 0; i < files.size(); i++)
         names.push_back(files[i].path);

      // A count made without every name is a count too low, and an exact one
      // then lowers and removes the blobs of the names it did not see: a store
      // that did not answer for any name ends the recount with nothing changed
      // (docs/ObjectStorage.md).
      std::vector<String> kept;
      if (!ListKeptFiles(kept))
      {
         summary = MessageStoreObjects::Active()
            ? String(_T("the deleted messages kept for recovery could not all be listed - the object store did not answer - so nothing was counted or changed. Run the recount again when the store answers."))
            : String(_T("the deleted messages kept for recovery (the Recoverable directory) could not all be read, so nothing was counted or changed."));
         return false;
      }
      names.insert(names.end(), kept.begin(), kept.end());

      // Every name that references a blob counts it once: a file with several
      // names is several names, which is what a count is of.
      std::map<AnsiString, std::pair<unsigned __int64, __int64> > counts;
      for (size_t i = 0; i < names.size(); i++)
      {
         MessageStore::ClearUnavailable();
         std::vector<MessageStoreReference> references;
         if (!MessageStore::GetReferences(names[i], references))
         {
            if (MessageStore::Unavailable())
            {
               summary = _T("the object store did not answer for ") + names[i] + _T(", so nothing was counted or changed. Run the recount again when the store answers.");
               return false;
            }
            continue;
         }

         for (size_t r = 0; r < references.size(); r++)
         {
            std::pair<unsigned __int64, __int64> &count = counts[references[r].sha256];
            count.first = references[r].length;
            count.second++;
         }
      }

      return MessageStoreBlobs::SetCounts(counts, exact, summary);
   }

   void
   MessageStoreMaintenance::FindBlobProblems(std::vector<String> &missing, std::vector<AnsiString> &unreferenced)
   {
      missing.clear();
      unreferenced.clear();

      // A store that has never held a part once has nothing to look at, and
      // the check costs what it cost before this row.
      std::map<AnsiString, __int64> rows;
      MessageStoreBlobs::List(rows);
      const String root = MessageStoreBlobs::Root();
      if (rows.empty() && !FileUtilities::DirectoryExists(root))
         return;

      std::vector<StoredFile> files;
      String problem;
      if (!ListStoredFiles(String(), String(), files, problem))
         return;

      std::set<AnsiString> referenced;

      // A name the object store could not answer for leaves what it
      // references unknown: no blob is then listed as unreferenced, and none
      // as missing on the store's silence (docs/ObjectStorage.md).
      bool complete = true;

      for (size_t i = 0; i < files.size(); i++)
      {
         MessageStore::ClearUnavailable();
         std::vector<MessageStoreReference> references;
         if (!MessageStore::GetReferences(files[i].path, references))
         {
            if (MessageStore::Unavailable())
               complete = false;
            continue;
         }

         for (size_t r = 0; r < references.size(); r++)
         {
            referenced.insert(references[r].sha256);

            MessageStore::ClearUnavailable();
            if (!MessageStoreBlobs::Exists(references[r].sha256))
            {
               if (MessageStore::Unavailable())
               {
                  complete = false;
                  continue;
               }

               String line;
               line.Format(_T("%I64d\t%s\t%s (blob %s missing)"), files[i].message_id, files[i].account.c_str(),
                  files[i].path.c_str(), String(references[r].sha256).c_str());
               missing.push_back(line);
               break;
            }
         }
      }

      std::vector<String> kept;
      if (!ListKeptFiles(kept))
         complete = false;
      for (size_t i = 0; i < kept.size(); i++)
      {
         MessageStore::ClearUnavailable();
         std::vector<MessageStoreReference> references;
         if (MessageStore::GetReferences(kept[i], references))
         {
            for (size_t r = 0; r < references.size(); r++)
               referenced.insert(references[r].sha256);
         }
         else if (MessageStore::Unavailable())
            complete = false;
      }

      if (!complete)
         return;

      // The blobs on the disk, and on an object store in it too
      // (docs/ObjectStorage.md): the file name is the blob's SHA-256.
      std::vector<String> blobFiles;
      if (!MessageStore::Backend().List(root, blobFiles))
         return;
      for (size_t i = 0; i < blobFiles.size(); i++)
      {
         const AnsiString name = AnsiString(FileUtilities::GetFileNameFromFullPath(blobFiles[i]));
         if (name.size() == 64 && referenced.find(name) == referenced.end())
            unreferenced.push_back(name);
      }
   }

   MessageStoreMaintenance::Totals
   MessageStoreMaintenance::GetTotals(bool &running, MeasureWhen when)
   {
      Totals totals;
      {
         boost::mutex::scoped_lock lock(totals_lock);
         totals = last_totals;
      }

      const __int64 now = (__int64) time(nullptr);
      bool start = when == MeasureImmediately ||
                   totals.measured == 0 || now - totals.measured > TotalsSeconds || now < totals.measured;

      // The dashboard's question, asked on every refresh: a walk of the whole
      // store only when there is something in it to measure. On the shipped
      // default - compression off, no part held once, and no container seen -
      // nothing is walked, and the answer is the last measurement, or none.
      if (start && when == MeasureIfDueWhileInUse)
         start = Configuration::Instance()->GetMessageStoreCompression() || totals.containers > 0 || MessageStoreBlobs::Any();

      if (start && !measuring.exchange(true))
      {
         std::shared_ptr<WorkQueue> queue = Application::Instance()->GetAsyncWorkQueue();
         if (queue)
            queue->AddTask(std::shared_ptr<Task>(new MessageStoreMeasureTask()));
         else
            measuring = false;
      }

      running = measuring.load();
      return totals;
   }

   void
   MessageStoreMaintenance::MeasureNow()
   {
      Totals totals;

      std::vector<StoredFile> files;
      String problem;
      if (!ListStoredFiles(String(), String(), files, problem))
         return;

      // One stat per name, one read of a header per file: what a store of this
      // size takes to look at, on the asynchronous work queue. A row whose file
      // cannot be sized - missing, or expunged while the walk ran - is counted
      // on neither side, so that it never reads as a saving; the consistency
      // check is what reports it.
      std::set<AnsiString> seen;
      for (size_t i = 0; i < files.size(); i++)
      {
         unsigned __int64 stored = 0;
         if (!MessageStore::StoredFileSize64(files[i].path, stored))
            continue;

         totals.messages++;
         totals.logical_bytes += files[i].size;

         AnsiString identity;
         unsigned int names = 0;
         if (MessageStore::Backend().Identity(files[i].path, identity, names) && !seen.insert(identity).second)
            continue;

         totals.stored_bytes += (__int64) stored;

         if (MessageStore::IsContainer(files[i].path))
            totals.containers++;
      }

      MessageStoreBlobs::Totals blobs;
      if (MessageStoreBlobs::GetTotals(blobs))
      {
         totals.blobs = blobs.blobs;
         totals.blob_bytes = blobs.bytes;
         totals.blob_references = blobs.references;
      }

      totals.measured = (__int64) time(nullptr);

      boost::mutex::scoped_lock lock(totals_lock);
      last_totals = totals;
   }

   AnsiString
   MessageStoreMaintenance::TotalsJson(MeasureWhen when)
   {
      bool running = false;
      const Totals totals = GetTotals(running, when);

      // What is saved: the messages' own size against what their files and the
      // blobs they share take. Signed: a store of tiny messages wrapped only to
      // be read exactly could take a few bytes more.
      const __int64 saved = totals.logical_bytes - totals.stored_bytes - totals.blob_bytes;
      const double percent = totals.logical_bytes > 0 ? 100.0 * (double) saved / (double) totals.logical_bytes : 0.0;

      AnsiString json;
      json.Format("{\"measured\":%I64d,\"measuring\":%s,\"messages\":%I64d,\"logical_bytes\":%I64d,\"stored_bytes\":%I64d,"
                  "\"compressed_files\":%I64d,\"blobs\":%I64d,\"blob_bytes\":%I64d,\"blob_references\":%I64d,"
                  "\"saved_bytes\":%I64d,\"saved_percent\":%.1f,\"compression\":%s,\"dedup_min_kb\":%d}",
         totals.measured, running ? "true" : "false", totals.messages, totals.logical_bytes, totals.stored_bytes,
         totals.containers, totals.blobs, totals.blob_bytes, totals.blob_references,
         saved, percent, Configuration::Instance()->GetMessageStoreCompression() ? "true" : "false",
         Configuration::Instance()->GetMessageStoreDedupMinKB());

      return json;
   }

   int
   MessageStoreMaintenance::TakeStoreLock(bool exclusive, String &problem)
   {
#ifdef HM_PLATFORM_POSIX
      // Held for the life of the process: the descriptor is never closed, and
      // the kernel lets go of the lock when the process ends, however it ends.
      static int held = -1;
      if (held >= 0)
         return 0;

      const String path = FileUtilities::Combine(IniFileSettings::Instance()->GetLogDirectory(), _T("hmailserver-store.lock"));
      const AnsiString narrow = path.c_str();

      const int descriptor = ::open(narrow.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0640);
      if (descriptor < 0)
      {
         problem = "the lock file " + path + " could not be opened: " + String(AnsiString(std::strerror(errno)));
         return 2;
      }

      if (::flock(descriptor, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
      {
         ::close(descriptor);
         problem = exclusive
            ? "the server is running (it holds " + path + ")"
            : "a message store walk is running (hmailserver --compress-store or --recount-store holds " + path + ")";
         return 1;
      }

      held = descriptor;
      return 0;
#else
      // The service controller is asked instead (hMailServer.cpp).
      (void) exclusive;
      (void) problem;
      return 0;
#endif
   }

   int
   MessageStoreMaintenance::RunCommand(Verb verb, const String &accountAddress, const String &domainName)
   {
      const String verbName = verb == VerbMeasure ? String(_T("measure")) : (verb == VerbRecount ? String(_T("recount")) : String(_T("rewrite")));

      // The domain's passphrase, before anything is opened: --decrypt-store
      // covers one account or one domain, whose recovery key the passphrase
      // opens, and reads it from the environment only.
      AnsiString passphrase;
      if (verb == VerbDecrypt)
      {
         if (accountAddress.IsEmpty() && domainName.IsEmpty())
         {
            Print(true, "--decrypt-store covers one account (--account <address>) or one domain (--domain <name>): a recovery passphrase opens one domain's keys. Nothing was changed.");
            return 2;
         }

         passphrase = RecoveryPassphraseFromEnvironment();
         if (passphrase.IsEmpty())
         {
            Print(true, "--decrypt-store needs the domain's recovery passphrase in the environment variable HMAILSERVER_RECOVERY_PASSPHRASE - never on the command line, where every user of the machine can read it. Nothing was changed.");
            return 2;
         }
      }

      if (!OpenForCommand(verbName))
         return 2;

      // The rewrite and the recount write what a running server reads, writes
      // and counts, in a process neither can lock; measuring changes nothing and
      // runs beside it. Windows asked the service controller before this.
      if (verb != VerbMeasure)
      {
         String problem;
         const int taken = TakeStoreLock(true, problem);
         if (taken != 0)
         {
            Print(true, taken == 1
               ? "The hMailServer server is running: " + problem + ". Stop it (systemctl stop hmailserver) to rewrite the message store or recount its blobs, then start it again; --measure-store runs beside it. Nothing was changed."
               : "Whether the server is running could not be asked: " + problem + ". Run this as the user the server runs as, with the server stopped. Nothing was changed.");
            Application::Instance()->ExitInstance();
            return 4;
         }
      }

      int result = 0;

      do
      {
         if (verb == VerbRecount)
         {
            String summary;
            if (!MessageStoreMaintenance::Recount(summary, true))
            {
               Print(true, "The blob counts could not be rebuilt: " + summary);
               result = 1;
               break;
            }

            Print(false, "Recounted: " + summary);
            break;
         }

         std::vector<StoredFile> files;
         String problem;
         if (!ListStoredFiles(accountAddress, domainName, files, problem))
         {
            Print(true, "The stored files could not be listed: " + problem + ". Nothing was changed.");
            result = accountAddress.IsEmpty() && domainName.IsEmpty() ? 1 : 2;
            break;
         }

         // A mailbox encrypted at rest has its deleted items - the copies the
         // recover-deleted store keeps - encrypted and decrypted with its mail,
         // so that a copy kept before the switch is not left in the clear.
         if (verb == VerbEncrypt || verb == VerbDecrypt)
         {
            std::vector<StoredFile> kept;
            if (!ListKeptFilesOf(accountAddress, domainName, kept, problem))
            {
               Print(true, "The deleted items could not be listed: " + problem + ". Nothing was changed.");
               result = 1;
               break;
            }

            files.insert(files.end(), kept.begin(), kept.end());
         }

         const String scope = !accountAddress.IsEmpty() ? accountAddress :
                              (!domainName.IsEmpty() ? String(_T("the domain ")) + domainName : String(_T("the whole store")));

         if (verb == VerbMeasure)
            result = Measure(files, scope);
         else if (verb == VerbEncrypt)
            result = Encrypt(files, scope);
         else if (verb == VerbDecrypt)
            result = Decrypt(files, scope, passphrase);
         else
            result = Rewrite(files, scope, verb == VerbDecompress);
      } while (false);

      Application::Instance()->ExitInstance();
      return result;
   }
}
