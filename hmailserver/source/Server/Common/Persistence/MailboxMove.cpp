// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "MailboxMove.h"

#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/Accounts.h"
#include "../BO/Alias.h"
#include "../BO/Domain.h"
#include "../BO/DistributionList.h"
#include "../Cache/Cache.h"
#include "../Cache/CacheContainer.h"
#include "../Cache/AccountSizeCache.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../Sieve/SieveStorage.h"
#include "../Util/AuditTrail.h"
#include "../Util/DiscoveryHolds.h"
#include "../Util/MailboxKeys.h"
#include "../Util/StorePath.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"
#include "../../POP3/POP3Sessions.h"
#include "../Tracking/NotificationServer.h"

#include "PersistentAccount.h"
#include "PersistentAlias.h"
#include "PersistentDistributionList.h"
#include "PersistentDomain.h"
#include "PersistentMessage.h"
#include "PreSaveLimitationsCheck.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      DatabaseConnectionManager *Db()
      {
         return Application::Instance()->GetDBManager().get();
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      // A JSON string body, from wide text, as UTF-8. The same shape as
      // BounceTracker's: this class answers COM and REST alike, and both of them
      // hand what comes back to a page.
      AnsiString JsonText(const String &value)
      {
         const AnsiString utf8 = Utf8(value);
         AnsiString result;

         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const char c = utf8.GetAt(i);

            switch (c)
            {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
               if ((unsigned char) c < 0x20)
               {
                  char escaped[8];
                  snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char) c);
                  result += escaped;
               }
               else
                  result += c;
               break;
            }
         }

         return result;
      }

      AnsiString JsonField(const char *name, const String &value)
      {
         return AnsiString("\"") + name + "\":\"" + JsonText(value) + "\"";
      }

      AnsiString JsonNumber(const char *name, __int64 value)
      {
         AnsiString text;
         text.Format("\"%s\":%I64d", name, value);
         return text;
      }

      AnsiString JsonBool(const char *name, bool value)
      {
         return AnsiString("\"") + name + "\":" + (value ? "true" : "false");
      }

      // The quarantine names its recipients as text in one column, and there is
      // no LIKE this class may write: SQL Server Compact refuses a long nvarchar
      // parameter compared with a column, which is exactly what a search for an
      // address inside quarantinerecipients would be. So the entries are read
      // and matched here. A move is an administrative operation performed by
      // hand, not a mail path, and the quarantine is bounded by its retention;
      // this is the ceiling that keeps a pathological store from turning one
      // into a long table scan, and what is beyond it keeps the old address.
      // The ceiling is in docs/MailboxMove.md beside what the rewrite does, so
      // that an administrator with a quarantine that big is told rather than
      // left to find out.
      const int MaxQuarantineRowsExamined = 200000;

      // The journal, read back, is a page an administrator looks at.
      const int MaxJournalRows = 1000;
   }

   String
   MailboxMove::StageName(int stage)
   {
      switch (stage)
      {
      case StageStarted:              return _T("started");
      case StageMovingStore:          return _T("moving the store");
      case StageStoreMoved:           return _T("store moved");
      case StageAccountWritten:       return _T("account written");
      case StageReferencesRewritten:  return _T("references rewritten");
      case StageComplete:             return _T("complete");
      case StageRolledBack:           return _T("rolled back");
      case StageRollbackFailed:       return _T("rollback failed");
      default:                        return _T("unknown");
      }
   }

   String
   MailboxMove::InUseReason(std::shared_ptr<const Account> account)
   {
      if (!account || account->GetID() <= 0)
         return String();

      if (POP3Sessions::Instance()->IsLocked(account->GetID()))
      {
         return Formatter::Format(_T("A POP3 session is holding the mailbox of {0}. It is released when that session ends, ")
                                  _T("or at once by clearing the mailbox lock."), account->GetAddress());
      }

      std::shared_ptr<NotificationServer> notifications = Application::Instance()->GetNotificationServer();

      if (notifications && notifications->HasMessageSubscribers(account->GetID()))
      {
         return Formatter::Format(_T("An IMAP session has a mailbox of {0} selected. Move it once the client has disconnected."),
                                  account->GetAddress());
      }

      return String();
   }

   bool
   MailboxMove::Move(std::shared_ptr<Account> account, const String &newAddress, bool keepOldAddressAsAlias,
                     MailboxMoveOutcome &outcome, String &errorMessage)
   {
      return Move_(account, newAddress, keepOldAddressAsAlias, true, outcome, errorMessage);
   }

   bool
   MailboxMove::Rename(std::shared_ptr<Account> account, const String &newAddress, String &errorMessage)
   {
      MailboxMoveOutcome outcome;

      return Move_(account, newAddress, false, false, outcome, errorMessage);
   }

   bool
   MailboxMove::Move_(std::shared_ptr<Account> account, const String &newAddress, bool keepOldAddressAsAlias,
                      bool refuseWhileInUse, MailboxMoveOutcome &outcome, String &errorMessage)
   {
      errorMessage = "";

      if (!account || account->GetID() <= 0)
      {
         errorMessage = "There is no such account.";
         return false;
      }

      // The address the account is AT, read from its row rather than taken from
      // the object the caller is holding. A caller that has set Account.Address
      // without saving holds an object whose address no account has: believing
      // it would move a directory that does not exist, write the new address
      // over the row, and leave every alias, list membership, queued recipient
      // and quarantine entry naming an address nothing answers at. The stored
      // row is what those references were written against, so it is what the
      // move is from.
      std::shared_ptr<Account> candidate = std::shared_ptr<Account>(new Account());

      if (!PersistentAccount::ReadObject(candidate, account->GetID()) || candidate->GetID() <= 0)
      {
         errorMessage = "The account could not be read.";
         return false;
      }

      const String oldAddress = candidate->GetAddress();
      const __int64 oldDomainID = candidate->GetDomainID();
      String wanted = newAddress;
      wanted.Trim();

      if (wanted.IsEmpty())
      {
         errorMessage = "A new address is required.";
         return false;
      }

      if (wanted.GetLength() > 255)
      {
         errorMessage = "The new address exceeds the 255 character limit.";
         return false;
      }

      if (wanted.CompareNoCase(oldAddress) == 0 && wanted != oldAddress)
      {
         // Only the spelling changes. The store's directory names are compared
         // without regard to case on Windows and lower-cased on a case-sensitive
         // filesystem (StorePath.h), so there is no directory to move and the
         // move-the-directory step below would refuse because the destination
         // "already exists". Allowed, because fixing the capitalisation of an
         // address is a thing an administrator does.
      }
      else if (wanted == oldAddress)
      {
         errorMessage = "The account is already at that address.";
         return false;
      }

      const String newDomainName = StringParser::ExtractDomain(wanted);

      if (newDomainName.IsEmpty() || wanted.Find(_T("@")) <= 0)
      {
         errorMessage = "The new address must be a full address at a domain this server holds.";
         return false;
      }

      std::shared_ptr<const Domain> cachedDomain = CacheContainer::Instance()->GetDomain(newDomainName);

      if (!cachedDomain || cachedDomain->GetID() <= 0)
      {
         errorMessage = Formatter::Format("This server does not hold the domain {0}. Create it before moving a mailbox into it.", newDomainName);
         return false;
      }

      // Read afresh rather than editing the cached copy: the limitations check
      // walks the domain's accounts, aliases and lists, and it must walk the
      // domain the move is going into.
      std::shared_ptr<Domain> targetDomain = std::shared_ptr<Domain>(new Domain());

      if (!PersistentDomain::ReadObject(targetDomain, cachedDomain->GetID()) || targetDomain->GetID() <= 0)
      {
         errorMessage = Formatter::Format("The domain {0} could not be read.", newDomainName);
         return false;
      }

      // Asked only where the mailbox is going INTO a different domain. A rename
      // inside a domain cannot make a mailbox any less reachable than it
      // already is, and refusing one would take away something that has always
      // worked: setting Account.Address in a deactivated domain, which comes
      // through this same door.
      if (targetDomain->GetID() != oldDomainID && !targetDomain->GetIsActive())
      {
         errorMessage = Formatter::Format("The domain {0} is not active. A mailbox moved into it could not receive mail.", newDomainName);
         return false;
      }

      // A legal hold on the account, or on the domain it is in, refuses a move
      // to another domain (docs/Discovery.md). A domain hold is kept by the
      // domain's id and holds the accounts that are in the domain, so an
      // account moved out of it would leave its hold behind - and could then be
      // deleted, expunged without a copy and swept by retention. Refused for
      // an account-scope hold as well, which would survive the move: a held
      // mailbox stays where the case found it until the hold is released. A
      // rename inside the domain goes ahead, since both kinds of hold stay
      // with it. Every door comes through here: REST, COM's Account.Move and
      // Utilities.MoveMailbox, SCIM's rename and Account.Address saved.
      if (targetDomain->GetID() != oldDomainID && DiscoveryHolds::IsAccountHeld(account->GetID()))
      {
         errorMessage = DiscoveryHolds::MoveRefusal(oldAddress, newDomainName);
         return false;
      }

      // A mailbox key (docs/EncryptionAtRest.md) is sealed to its DOMAIN'S
      // recovery key: moved to another domain it would stay sealed to the old
      // domain's, which no recovery of the new domain can open, which the old
      // domain could then remove, and which would stop the new domain from ever
      // replacing its own. So a keyed mailbox - its key switched on or not -
      // stays in its domain; a rename inside it goes ahead, since the key and
      // its seal stay with it. Every door comes through here, as for a hold.
      // Asked of the keyed set (MailboxKeys::HasKey), and refused for now when
      // it cannot say.
      if (targetDomain->GetID() != oldDomainID)
      {
         bool unknown = false;
         const bool keyed = MailboxKeys::HasKey(account->GetID(), unknown);
         if (keyed || unknown)
         {
            errorMessage = unknown
               ? Formatter::Format("Whether {0} has a mailbox key could not be read just now, so it is not moved to another domain until it can be; try again in a moment.", oldAddress)
               : Formatter::Format("{0} is encrypted at rest, and its key is sealed to the recovery key of its own domain, which {1} cannot open. A mailbox encrypted at rest stays in its domain: to move it, put its mail back in the clear (hmailserver --decrypt-store --account) and remove its key (DELETE /api/v1/accounts/{0}/mailbox-encryption?confirm=forfeit) first. A rename inside its domain is not refused.",
                                   oldAddress, newDomainName);
            return false;
         }
      }

      // The address must be free, as an account, an alias or a distribution
      // list. PreSaveLimitationsCheck below asks the same question and answers
      // "Another object with the same name already exists in this domain",
      // which does not say which of the three it met; the address is the whole
      // subject of this operation, so it is worth the three reads to be able to
      // say.
      std::shared_ptr<Account> occupant = std::shared_ptr<Account>(new Account());

      if (PersistentAccount::ReadObject(occupant, wanted) && occupant->GetID() != account->GetID())
      {
         errorMessage = Formatter::Format("An account already exists at {0}.", wanted);
         return false;
      }

      std::shared_ptr<Alias> existingAlias = std::shared_ptr<Alias>(new Alias());

      if (PersistentAlias::ReadObject(existingAlias, wanted) && existingAlias->GetID() > 0)
      {
         errorMessage = Formatter::Format("An alias already exists at {0}. Delete it, or choose another address.", wanted);
         return false;
      }

      std::shared_ptr<DistributionList> existingList = std::shared_ptr<DistributionList>(new DistributionList());

      if (PersistentDistributionList::ReadObject(existingList, wanted) && existingList->GetID() > 0)
      {
         errorMessage = Formatter::Format("A distribution list already exists at {0}.", wanted);
         return false;
      }

      // Everything the account's own save would refuse - the address's shape,
      // the characters the store cannot put in a directory name, the domain's
      // limits - asked of the address it is moving to, by the code that owns
      // those rules, before anything is changed. The object asked is a copy of
      // the stored row with the new address on it, so what is judged is the
      // account as it would be.
      const long arrivingMaxSize = candidate->GetAccountMaxSize();

      candidate->SetAddress(wanted);
      candidate->SetDomainID(targetDomain->GetID());

      if (!PreSaveLimitationsCheck::CheckLimitations(PersistenceModeNormal, candidate, errorMessage))
         return false;

      // Two of the domain's limits are asked there of NEW accounts only, by an
      // id of zero, and an account arriving from another domain has an id. So
      // they are asked here, and only for a move that crosses domains: without
      // this, moving mailboxes into a domain could take it past the maximum
      // number of accounts and the maximum total size an administrator set,
      // which creating one there could not.
      if (targetDomain->GetID() != oldDomainID)
      {
         if (targetDomain->GetMaxNoOfAccountsEnabled() &&
             targetDomain->GetAccounts()->GetCount() >= targetDomain->GetMaxNoOfAccounts())
         {
            errorMessage = Formatter::Format("The domain {0} already holds the maximum number of accounts, so a mailbox cannot be moved into it.",
                                             newDomainName);
            return false;
         }

         if (targetDomain->GetMaxSizeMB() > 0 &&
             PersistentDomain::GetAllocatedSize(targetDomain) + arrivingMaxSize > targetDomain->GetMaxSizeMB())
         {
            errorMessage = Formatter::Format("The total size of all accounts in {0} would exceed the maximum size for the domain, so a mailbox cannot be moved into it.",
                                             newDomainName);
            return false;
         }
      }

      // An installation that still holds full paths in messagefilename cannot
      // have a mailbox renamed at all: the paths would go on naming the old
      // directory, which this is about to empty. The same refusal
      // PersistentAccount::SaveObject has always made, in the same words.
      if (!PersistentMessage::GetAllMessageFilesArePartialNames())
      {
         errorMessage = "As of hMailServer 5.4, partial file names are stored in the database, rather than the full path\r\n"
                        "To be able to rename the account, you must first migrate your database to the new partial paths scheme. To do\r\n"
                        "this, run Data Directory Synchronizer.";
         return false;
      }

      // The sentence InUseReason writes names an address, and the address it
      // should name is the one the mailbox is at - not whatever the caller's
      // object says. candidate has had the new address written onto it above,
      // so the question is asked of a second copy of the stored row.
      std::shared_ptr<Account> stored = std::shared_ptr<Account>(new Account());

      if (!PersistentAccount::ReadObject(stored, account->GetID()))
         stored = account;

      if (refuseWhileInUse)
      {
         const String inUse = InUseReason(stored);

         if (!inUse.IsEmpty())
         {
            errorMessage = inUse;
            return false;
         }
      }

      // Nothing above this line has changed anything. Everything below it runs
      // with no actor on the thread, so that the statement chokepoint records
      // none of the rows a move writes, and the move is recorded once when the
      // actor is back - see ONE ENTRY ON THE AUDIT TRAIL in MailboxMove.h.
      bool leftBehind = false;
      bool moved = false;

      {
         AuditScope silent((AuditTrail::Actor()));

         moved = Perform_(account, oldAddress, wanted, targetDomain->GetID(), oldDomainID, keepOldAddressAsAlias,
                          outcome, errorMessage, leftBehind);
      }

      if (moved)
      {
         const String detail = Describe(outcome);

         AuditTrail::Instance()->RecordEvent(_T("account"), outcome.new_address, _T("moved"), detail);

         LOG_APPLICATION(detail);
      }
      else if (leftBehind)
      {
         // The one failure that IS an administrative change: something was
         // altered and could not be put back, so the trail has to say so under
         // whoever asked for the move.
         AuditTrail::Instance()->RecordEvent(_T("account"), outcome.old_address, _T("move failed"), errorMessage);
      }

      return moved;
   }

   String
   MailboxMove::Describe(const MailboxMoveOutcome &outcome)
   {
      // Formatter::Format takes at most five arguments, so this is built in
      // three sentences rather than one call.
      String detail = Formatter::Format(_T("{0} moved to {1}: {2} message(s) in {3} folder(s). "),
                                        outcome.old_address, outcome.new_address, outcome.messages, outcome.folders);

      detail += Formatter::Format(_T("{0} alias(es) re-pointed, {1} list membership(s), {2} queued recipient(s), {3} quarantined message(s). "),
                                  outcome.aliases_repointed, outcome.list_memberships, outcome.queued_recipients,
                                  outcome.quarantine_entries);

      if (outcome.alias_kept)
         detail += Formatter::Format(_T("The old address is kept as an alias to {0}."), outcome.new_address);
      else if (outcome.old_address.CompareNoCase(outcome.new_address) == 0)
         detail += _T("Only the spelling changed, so the old spelling goes on reaching the mailbox and no alias was made.");
      else
         detail += _T("The old address is not kept.");

      if (outcome.references_incomplete)
         detail += _T(" Some of the references to the old address could not be rewritten; the application log says which.");

      return detail;
   }

   bool
   MailboxMove::Perform_(std::shared_ptr<Account> account, const String &oldAddress, const String &wanted,
                         __int64 newDomainID, __int64 oldDomainID, bool keepOldAddressAsAlias,
                         MailboxMoveOutcome &outcome, String &errorMessage, bool &leftBehind)
   {
      const String newDomainName = StringParser::ExtractDomain(wanted);

      leftBehind = false;

      outcome = MailboxMoveOutcome();
      outcome.account_id = account->GetID();
      outcome.old_address = oldAddress;
      outcome.new_address = wanted;
      outcome.old_domain = StringParser::ExtractDomain(oldAddress);
      outcome.new_domain = newDomainName;
      outcome.alias_kept = false;
      outcome.messages = CountMessages_(account->GetID());
      outcome.folders = CountFolders_(account->GetID());

      const __int64 journalID = OpenJournal_(account->GetID(), oldAddress, wanted, keepOldAddressAsAlias);
      outcome.journal_id = journalID;

      // ---------------------------------------------------------- the store
      // Compared without regard to case, which is the store's own policy read
      // from both ends: on a case-sensitive filesystem StoreDirectoryName has
      // already lower-cased both, so this is an exact comparison; on Windows it
      // is what NTFS itself would answer, which is why changing only the
      // capitalisation of an address has no directory to move. Comparing
      // exactly would send such a rename into MoveDirectory_, where NTFS says
      // the destination exists and the rename is refused.
      const bool sameDirectory =
         StoreDirectoryName(StringParser::ExtractAddress(oldAddress))
            .CompareNoCase(StoreDirectoryName(StringParser::ExtractAddress(wanted))) == 0 &&
         StoreDirectoryName(StringParser::ExtractDomain(oldAddress))
            .CompareNoCase(StoreDirectoryName(newDomainName)) == 0;

      if (!sameDirectory)
      {
         // Written BEFORE the copy, not after it. A copy of a big mailbox takes
         // minutes, and a process killed inside one has to leave a row that says
         // the trees are somewhere between the two addresses - not one that says
         // nothing has been changed.
         CloseJournal_(journalID, StageMovingStore, outcome.messages,
                       Formatter::Format(_T("Copying the mailbox and Sieve trees from {0} to {1}."), oldAddress, wanted));

         bool carried = false;

         if (!MoveStore_(oldAddress, wanted, carried, errorMessage))
         {
            CloseJournal_(journalID, StageRolledBack, outcome.messages,
                          _T("The mailbox was not moved and nothing was changed: ") + errorMessage);
            return false;
         }

         // What was carried, not that the step ran: a mailbox nothing has ever
         // been delivered to has no directory, and saying its store moved would
         // be saying the one thing a caller would use this to tell apart.
         outcome.store_moved = carried;
      }

      CloseJournal_(journalID, StageStoreMoved, outcome.messages, _T("The mailbox and Sieve trees are at the new address."));

      // --------------------------------------------------------- the account
      if (!WriteAccountRow_(account->GetID(), wanted, newDomainID))
      {
         errorMessage = Formatter::Format("The account row for {0} could not be written, so the move was undone.", oldAddress);

         if (outcome.store_moved)
         {
            String putBack;
            bool putBackCarried = false;

            if (!MoveStore_(wanted, oldAddress, putBackCarried, putBack))
            {
               errorMessage = Formatter::Format("The account row for {0} could not be written, and its mailbox could not be moved back: {1} "
                                                "The mailbox is on disk at {2} and the account still answers at {3}.",
                                                oldAddress, putBack, wanted, oldAddress);

               ErrorManager::Instance()->ReportError(ErrorManager::High, 6940, "MailboxMove::Perform_", errorMessage);
               CloseJournal_(journalID, StageRollbackFailed, outcome.messages, errorMessage);
               leftBehind = true;
               return false;
            }
         }

         CloseJournal_(journalID, StageRolledBack, outcome.messages, errorMessage);
         return false;
      }

      CloseJournal_(journalID, StageAccountWritten, outcome.messages, _T("The account row carries the new address."));

      // The account row now carries the new address, so every path built from
      // here on is built under it. The caches are dropped HERE, before the
      // references are rewritten rather than at the end of the move, because
      // what reads them is delivery: until they are dropped, a delivery running
      // beside this one goes on building its path from the old address. Then
      // the old directory is swept once for anything that landed in it while
      // that was still true - which cannot be done before the caches are clear,
      // or the sweep races the thing it is cleaning up after.
      ForgetCaches_(account, oldAddress, oldDomainID, newDomainID);

      if (!sameDirectory)
         SweepLateArrivals_(oldAddress, wanted);

      // ------------------------------------------------------ the references
      // Each of these is one statement, so each is all-or-nothing in itself,
      // and none of them can leave mail undeliverable on its own: an alias that
      // was not re-pointed still names an address the alias left behind
      // answers. They are counted rather than checked, and a failure is
      // reported as an error rather than undoing a move that has already
      // succeeded - putting the account back at this point would be the larger
      // change, not the smaller one.
      outcome.aliases_repointed = RepointAliases_(oldAddress, wanted, outcome.references_incomplete);
      outcome.list_memberships = RewriteListMemberships_(oldAddress, wanted, outcome.references_incomplete);
      outcome.queued_recipients = RewriteQueuedRecipients_(account->GetID(), oldAddress, wanted, outcome.references_incomplete);
      outcome.quarantine_entries = RewriteQuarantine_(oldAddress, wanted, outcome.references_incomplete);

      CloseJournal_(journalID, StageReferencesRewritten, outcome.messages,
                    _T("Aliases, list memberships, queued recipients and quarantine entries rewritten."));

      // ----------------------------------------------------------- the alias
      // Not for a change of capitalisation alone: the old spelling already
      // reaches the account, because an address is matched without regard to
      // case, and an alias at it would be refused as a duplicate of the account
      // itself - which would put HM6941 in the ERROR log, saying that mail to
      // the old address will be refused, about mail that is delivered.
      if (keepOldAddressAsAlias && oldAddress.CompareNoCase(wanted) != 0)
      {
         String aliasError;

         if (LeaveAlias_(oldDomainID, oldAddress, wanted, aliasError))
         {
            outcome.alias_kept = true;
         }
         else
         {
            // The move has happened. Saying so and naming what is missing is
            // right; undoing a completed move because a forwarding entry could
            // not be written is not.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6941, "MailboxMove::Perform_",
               Formatter::Format(_T("{0} was moved to {1}, but the alias that would have kept the old address working could not be created: {2} ")
                                 _T("Mail to {0} will be refused until an alias is added by hand."), oldAddress, wanted, aliasError));
         }
      }

      // The in-memory object the caller holds now says what the database says.
      account->SetAddress(wanted);
      account->SetDomainID(newDomainID);

      CloseJournal_(journalID, StageComplete, outcome.messages, Describe(outcome));

      return true;
   }

   // ------------------------------------------------------------------ steps

   bool
   MailboxMove::MoveStore_(const String &oldAddress, const String &newAddress, bool &carried, String &errorMessage)
   {
      carried = false;

      const String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();

      // The domain component comes from the address being moved FROM and the
      // one being moved TO, which is the whole difference between this and the
      // rename that only ever worked inside one domain: NameChanger took both
      // from the old address, so a move to another domain put the mailbox in
      // the old domain's directory under the new name, where nothing would ever
      // look for it.
      const String oldDirectory =
         FileUtilities::Combine(FileUtilities::Combine(dataDirectory, StoreDirectoryName(StringParser::ExtractDomain(oldAddress))),
                                StoreDirectoryName(StringParser::ExtractAddress(oldAddress)));

      const String newDirectory =
         FileUtilities::Combine(FileUtilities::Combine(dataDirectory, StoreDirectoryName(StringParser::ExtractDomain(newAddress))),
                                StoreDirectoryName(StringParser::ExtractAddress(newAddress)));

      bool mailboxCarried = false;

      if (!MoveDirectory_(oldDirectory, newDirectory, mailboxCarried, errorMessage))
         return false;

      // The Sieve tree is not under the mailbox directory. A rename that left it
      // behind left the account's filter at the old address, where nothing would
      // read it again, and the user's mail silently stopped being filed.
      bool sieveCarried = false;

      if (!MoveDirectory_(SieveStorage::GetAccountDirectory(oldAddress),
                          SieveStorage::GetAccountDirectory(newAddress), sieveCarried, errorMessage))
      {
         // Put the mailbox back, so that a failure here is not half a move.
         String ignored;
         bool ignoredCarried = false;
         MoveDirectory_(newDirectory, oldDirectory, ignoredCarried, ignored);
         return false;
      }

      // Either tree having moved is something to put back, which is what the
      // caller uses this for; and a mailbox that has no directory of its own but
      // does have a Sieve tree has still had its store moved.
      carried = mailboxCarried || sieveCarried;

      return true;
   }

   void
   MailboxMove::SweepLateArrivals_(const String &oldAddress, const String &newAddress)
   {
      // Between the old tree being deleted and the account row carrying the new
      // address, a delivery that was already under way writes its .eml under the
      // OLD directory, and re-creates it to do so. Nothing in the server records
      // a delivery in progress, so this cannot be waited for; what it can be is
      // swept up afterwards, which is what this is. CopyDirectory merges into a
      // tree that already exists - it creates only the directories that are
      // missing and copies the files it finds - so the arrivals land beside the
      // messages that were carried, under the file names hm_messages already
      // holds.
      const String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();

      const String oldDirectory =
         FileUtilities::Combine(FileUtilities::Combine(dataDirectory, StoreDirectoryName(StringParser::ExtractDomain(oldAddress))),
                                StoreDirectoryName(StringParser::ExtractAddress(oldAddress)));

      const String newDirectory =
         FileUtilities::Combine(FileUtilities::Combine(dataDirectory, StoreDirectoryName(StringParser::ExtractDomain(newAddress))),
                                StoreDirectoryName(StringParser::ExtractAddress(newAddress)));

      // The caller only sweeps where the directory changed, and this says so
      // again rather than trust that: the last thing this does is delete the
      // directory it swept, and sweeping a mailbox into itself would delete it.
      if (oldDirectory.CompareNoCase(newDirectory) == 0)
         return;

      if (!FileUtilities::Exists(oldDirectory))
         return;

      String errorMessage;

      try
      {
         if (FileUtilities::CopyDirectory(oldDirectory, newDirectory, errorMessage))
         {
            FileUtilities::DeleteDirectory(oldDirectory, true);
            return;
         }
      }
      catch (const std::exception &exception)
      {
         errorMessage = exception.what();
      }
      catch (...)
      {
         errorMessage = "An unknown error was raised while copying the directory.";
      }

      ErrorManager::Instance()->ReportError(ErrorManager::High, 6948, "MailboxMove::SweepLateArrivals_",
         Formatter::Format(_T("{0} was moved to {1}, but mail delivered to the old address while the move was running is still in {2} and could not be carried across: {3} ")
                           _T("Those messages are listed for the mailbox and cannot be read until the files are moved into {4} by hand."),
                           oldAddress, newAddress, oldDirectory, errorMessage, newDirectory));
   }

   bool
   MailboxMove::MoveDirectory_(const String &from, const String &to, bool &carried, String &errorMessage)
   {
      carried = false;

      if (from.IsEmpty() || to.IsEmpty())
      {
         errorMessage = "The mailbox directory could not be named.";
         return false;
      }

      if (!FileUtilities::Exists(from))
      {
         // Nothing to move, which is two cases and neither is a failure. A
         // mailbox nothing has been delivered to has no directory yet, and the
         // first delivery makes the tree at the new address. And a move that
         // was interrupted after this step has the tree at the new address
         // already, so a second attempt - the one docs/MailboxMove.md tells an
         // administrator to make - finds this step done and carries on.
         //
         // Asked BEFORE the destination check on purpose: with the order the
         // other way round, an interrupted move could never be finished, because
         // the destination existing is exactly what "already moved" looks like.
         return true;
      }

      if (FileUtilities::Exists(to))
      {
         // Something is there and something is here, which is not a move: it
         // would put one mailbox's mail inside another's directory.
         errorMessage = Formatter::Format("{0} cannot be moved to {1} because {1} already exists.", from, to);
         return false;
      }

      // A copy and then a delete, not a rename: a file in the tree may be open,
      // and a rename of the whole tree then fails where a copy of it does not.
      // It is also what makes this step undoable - the source is intact until
      // the copy has succeeded.
      //
      // CopyDirectory does not report a failure part way through a tree, it
      // THROWS: boost::filesystem::copy_file has no error_code overload there,
      // and the source check raises std::logic_error. An exception let out here
      // would leave a partly copied tree at the destination, a journal row at
      // the stage before this one, and a caller with no sentence to show - and
      // every later attempt would then be refused, because the destination
      // existing is the one thing this refuses. So it is caught, the half tree
      // is removed, and the reason becomes the refusal.
      try
      {
         if (!FileUtilities::CopyDirectory(from, to, errorMessage))
         {
            FileUtilities::DeleteDirectory(to, true);
            return false;
         }
      }
      catch (const std::exception &exception)
      {
         const String reason = exception.what();

         errorMessage = Formatter::Format("{0} could not be copied to {1}: {2}", from, to, reason);
         FileUtilities::DeleteDirectory(to, true);
         return false;
      }
      catch (...)
      {
         errorMessage = Formatter::Format("{0} could not be copied to {1}.", from, to);
         FileUtilities::DeleteDirectory(to, true);
         return false;
      }

      FileUtilities::DeleteDirectory(from, true);

      carried = true;

      return true;
   }

   bool
   MailboxMove::WriteAccountRow_(__int64 accountID, const String &newAddress, __int64 newDomainID)
   {
      // Deliberately a narrow UPDATE of the two columns that make the move,
      // rather than PersistentAccount::SaveObject, for two reasons.
      //
      // SaveObject writes every column from the in-memory object, and the object
      // reaching here may be a cached snapshot: accountlastlogontime is written
      // by direct SQL without refreshing it, so a full save would roll back a
      // sign-in that happened while the move was being decided.
      //
      // And SaveObject's own rename path would run NameChanger::RenameAccount
      // over a store this class has already moved, which would find the old
      // directory gone and the new one present and refuse.
      SQLCommand command("update hm_accounts set accountaddress = @ADDRESS, accountdomainid = @DOMAINID where accountid = @ACCOUNTID");
      command.AddParameter("@ADDRESS", newAddress);
      command.AddParameter("@DOMAINID", newDomainID);
      command.AddParameter("@ACCOUNTID", accountID);

      return Db()->Execute(command);
   }

   int
   MailboxMove::RepointAliases_(const String &oldAddress, const String &newAddress, bool &incomplete)
   {
      // An alias whose VALUE is the old address: mail to it was delivered to
      // this mailbox and must go on being. Aliases are not per-domain here on
      // purpose - an alias in one domain may point at an account in another,
      // which is exactly the case a move creates.
      SQLCommand count("select count(*) as c from hm_aliases where aliasvalue = @OLD");
      count.AddParameter("@OLD", oldAddress);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(count);

      if (!recordset || recordset->IsEOF())
         return 0;

      const int rows = (int) recordset->GetLongValue("c");

      if (rows == 0)
         return 0;

      SQLCommand command("update hm_aliases set aliasvalue = @NEW where aliasvalue = @OLD");
      command.AddParameter("@NEW", newAddress);
      command.AddParameter("@OLD", oldAddress);

      if (!Db()->Execute(command))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6942, "MailboxMove::RepointAliases_",
            Formatter::Format(_T("{0} of the aliases pointing at {1} could not be re-pointed at {2}. Mail to them is delivered only while the old address is kept."),
                              rows, oldAddress, newAddress));

         // A count of what was written, and a flag to say the zero is a failure
         // rather than nothing to do - the summary, the journal and the audit
         // entry all read these, and "0 alias(es) re-pointed" on its own reads
         // as a move that had none.
         incomplete = true;
         return 0;
      }

      // Which aliases were re-pointed is not known here - it is one statement,
      // which is what makes it all-or-nothing - so the alias cache is emptied
      // rather than picked at. It holds names read from the database and
      // refills itself.
      Cache<Alias>::Instance()->Clear();

      return rows;
   }

   int
   MailboxMove::RewriteListMemberships_(const String &oldAddress, const String &newAddress, bool &incomplete)
   {
      SQLCommand count("select count(*) as c from hm_distributionlistsrecipients where distributionlistrecipientaddress = @OLD");
      count.AddParameter("@OLD", oldAddress);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(count);

      if (!recordset || recordset->IsEOF())
         return 0;

      const int rows = (int) recordset->GetLongValue("c");

      if (rows == 0)
         return 0;

      SQLCommand command("update hm_distributionlistsrecipients set distributionlistrecipientaddress = @NEW "
                         "where distributionlistrecipientaddress = @OLD");
      command.AddParameter("@NEW", newAddress);
      command.AddParameter("@OLD", oldAddress);

      if (!Db()->Execute(command))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6943, "MailboxMove::RewriteListMemberships_",
            Formatter::Format(_T("{0} distribution list membership(s) of {1} could not be moved to {2}. The lists deliver to the old address, which works only while it is kept as an alias."),
                              rows, oldAddress, newAddress));
         incomplete = true;
         return 0;
      }

      return rows;
   }

   int
   MailboxMove::RewriteQueuedRecipients_(__int64 accountID, const String &oldAddress, const String &newAddress, bool &incomplete)
   {
      // Mail in flight. A recipient row already resolved to this account
      // (recipientlocalaccountid) is delivered to whatever address it names, so
      // without this the message would be filed under a directory the move has
      // just emptied. recipientoriginaladdress is what the sender wrote and is
      // left exactly as it was: it is evidence, and a bounce quotes it.
      SQLCommand count("select count(*) as c from hm_messagerecipients where recipientlocalaccountid = @ACCOUNTID and recipientaddress = @OLD");
      count.AddParameter("@ACCOUNTID", accountID);
      count.AddParameter("@OLD", oldAddress);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(count);

      if (!recordset || recordset->IsEOF())
         return 0;

      const int rows = (int) recordset->GetLongValue("c");

      if (rows == 0)
         return 0;

      SQLCommand command("update hm_messagerecipients set recipientaddress = @NEW "
                         "where recipientlocalaccountid = @ACCOUNTID and recipientaddress = @OLD");
      command.AddParameter("@NEW", newAddress);
      command.AddParameter("@ACCOUNTID", accountID);
      command.AddParameter("@OLD", oldAddress);

      if (!Db()->Execute(command))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6944, "MailboxMove::RewriteQueuedRecipients_",
            Formatter::Format(_T("{0} message(s) in the delivery queue still name {1}, which has moved to {2}. They are delivered while the old address is kept as an alias and bounce otherwise."),
                              rows, oldAddress, newAddress));
         incomplete = true;
         return 0;
      }

      return rows;
   }

   int
   MailboxMove::RewriteQuarantine_(const String &oldAddress, const String &newAddress, bool &incomplete)
   {
      // Quarantined mail is held for its RECIPIENTS, named as text in one
      // comma-separated column, and an account reaches its own held mail by
      // matching that text. See MaxQuarantineRowsExamined for why this reads
      // rather than searching.
      SQLCommand select("select quarantineid, quarantinerecipients from hm_quarantine");
      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(select);

      if (!recordset)
         return 0;

      std::vector<std::pair<__int64, String> > rewrites;
      int examined = 0;

      while (!recordset->IsEOF() && examined < MaxQuarantineRowsExamined)
      {
         examined++;

         const __int64 id = recordset->GetInt64Value("quarantineid");
         const String recipients = recordset->GetStringValue("quarantinerecipients");

         std::vector<String> parts = StringParser::SplitString(recipients, _T(","));
         String rebuilt;
         bool matched = false;

         for (size_t i = 0; i < parts.size(); i++)
         {
            String candidate = parts[i];
            candidate.Trim();

            if (candidate.IsEmpty())
               continue;

            if (candidate.CompareNoCase(oldAddress) == 0)
            {
               candidate = newAddress;
               matched = true;
            }

            if (!rebuilt.IsEmpty())
               rebuilt += _T(",");

            rebuilt += candidate;
         }

         if (matched && !rebuilt.IsEmpty())
            rewrites.push_back(std::make_pair(id, rebuilt));

         recordset->MoveNext();
      }

      int written = 0;

      for (const auto &rewrite : rewrites)
      {
         SQLCommand command("update hm_quarantine set quarantinerecipients = @RECIPIENTS where quarantineid = @ID");
         command.AddParameter("@RECIPIENTS", rewrite.second);
         command.AddParameter("@ID", rewrite.first);

         if (Db()->Execute(command))
            written++;
      }

      if (written != (int) rewrites.size())
      {
         incomplete = true;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6945, "MailboxMove::RewriteQuarantine_",
            Formatter::Format(_T("{0} quarantined message(s) still name {1}, which has moved to {2}. They are not listed for the moved mailbox."),
                              (int) rewrites.size() - written, oldAddress, newAddress));
      }

      return written;
   }

   bool
   MailboxMove::LeaveAlias_(__int64 domainID, const String &oldAddress, const String &newAddress, String &errorMessage)
   {
      std::shared_ptr<Alias> alias = std::shared_ptr<Alias>(new Alias());

      alias->SetDomainID(domainID);
      alias->SetName(oldAddress);
      alias->SetValue(newAddress);
      alias->SetIsActive(true);

      if (!PersistentAlias::SaveObject(alias, errorMessage, PersistenceModeNormal))
      {
         if (errorMessage.IsEmpty())
            errorMessage = "The alias could not be saved.";

         return false;
      }

      return true;
   }

   void
   MailboxMove::ForgetCaches_(std::shared_ptr<Account> account, const String &oldAddress,
                              __int64 oldDomainID, __int64 newDomainID)
   {
      // The account under BOTH names: the cache is keyed by address as well as
      // by id, so the old name has to go or the next sign-in at it would be
      // answered from memory with an object whose paths point at a directory
      // that is no longer there.
      Cache<Account>::Instance()->RemoveObject(oldAddress);
      Cache<Account>::Instance()->RemoveObject(account->GetID());

      // Both domains, because each has a count of its accounts and a list of
      // them, and one has lost an account and the other has gained one.
      std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());

      if (PersistentDomain::ReadObject(domain, oldDomainID))
         CacheContainer::Instance()->RemoveDomain(domain);

      if (newDomainID != oldDomainID)
      {
         std::shared_ptr<Domain> target = std::shared_ptr<Domain>(new Domain());

         if (PersistentDomain::ReadObject(target, newDomainID))
            CacheContainer::Instance()->RemoveDomain(target);
      }

      // The folder tree and the messages in it are cached against the ACCOUNT
      // ID and so are still correct, but the objects in them were read while the
      // account had the old address; dropping them costs one re-read and removes
      // the whole class of question about what a cached copy remembers.
      IMAPFolderContainer::Instance()->UncacheAccount(account->GetID());
      MessagesContainer::Instance()->UncacheAccount(account->GetID());
      AccountSizeCache::Instance()->Reset(account->GetID());
   }

   int
   MailboxMove::CountMessages_(__int64 accountID)
   {
      SQLCommand command("select count(*) as c from hm_messages where messageaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountID);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return 0;

      return (int) recordset->GetLongValue("c");
   }

   int
   MailboxMove::CountFolders_(__int64 accountID)
   {
      SQLCommand command("select count(*) as c from hm_imapfolders where folderaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountID);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return 0;

      return (int) recordset->GetLongValue("c");
   }

   // ---------------------------------------------------------------- journal

   __int64
   MailboxMove::OpenJournal_(__int64 accountID, const String &oldAddress, const String &newAddress, bool keepAlias)
   {
      SQLStatement statement;
      statement.SetTable("hm_mailboxmoves");
      statement.AddColumnInt64("mmaccountid", accountID);
      statement.AddColumn("mmoldaddress", oldAddress);
      statement.AddColumn("mmnewaddress", newAddress);
      statement.AddColumn("mmstage", (long) StageStarted);
      statement.AddColumn("mmkeepalias", (long) (keepAlias ? 1 : 0));
      statement.AddColumn("mmmessages", (long) 0);
      statement.AddColumn("mmstarted", Time::GetCurrentDateTime());
      statement.AddColumn("mmfinished", String());
      statement.AddColumn("mmdetail", String());
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("mmid");

      __int64 id = 0;

      if (!Db()->Execute(statement, &id))
      {
         // The journal is a record, not a permission. A move that cannot be
         // journalled still happens - refusing it would make an unwritable
         // table into an outage - and the reason is reported so that the gap in
         // the journal is explained rather than silent.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6946, "MailboxMove::OpenJournal_",
            Formatter::Format(_T("The move of {0} to {1} could not be journalled. It is recorded on the audit trail and in the application log, and hm_mailboxmoves has no row for it."),
                              oldAddress, newAddress));
         return 0;
      }

      return id;
   }

   void
   MailboxMove::CloseJournal_(__int64 journalID, int stage, int messages, const String &detail)
   {
      if (journalID <= 0)
         return;

      const bool finished = stage == StageComplete || stage == StageRolledBack || stage == StageRollbackFailed;

      SQLCommand command("update hm_mailboxmoves set mmstage = @STAGE, mmmessages = @MESSAGES, mmfinished = @FINISHED, mmdetail = @DETAIL where mmid = @ID");
      command.AddParameter("@STAGE", stage);
      command.AddParameter("@MESSAGES", messages);
      command.AddParameter("@FINISHED", finished ? Time::GetCurrentDateTime() : String());
      command.AddParameter("@DETAIL", detail.Left(255));
      command.AddParameter("@ID", journalID);

      Db()->Execute(command);
   }

   void
   MailboxMove::ReadRecord_(std::shared_ptr<DALRecordset> recordset, MailboxMoveRecord &record)
   {
      record.id = recordset->GetInt64Value("mmid");
      record.account_id = recordset->GetInt64Value("mmaccountid");
      record.old_address = recordset->GetStringValue("mmoldaddress");
      record.new_address = recordset->GetStringValue("mmnewaddress");
      record.stage = (int) recordset->GetLongValue("mmstage");
      record.keep_alias = recordset->GetLongValue("mmkeepalias") != 0;
      record.messages = (int) recordset->GetLongValue("mmmessages");
      record.started = recordset->GetStringValue("mmstarted");
      record.finished = recordset->GetStringValue("mmfinished");
      record.detail = recordset->GetStringValue("mmdetail");
   }

   bool
   MailboxMove::List(__int64 accountID, int limit, std::vector<MailboxMoveRecord> &rows)
   {
      rows.clear();

      if (limit <= 0)
         limit = 100;

      if (limit > MaxJournalRows)
         limit = MaxJournalRows;

      SQLCommand command;

      if (accountID > 0)
      {
         command.SetQueryString(_T("select mmid, mmaccountid, mmoldaddress, mmnewaddress, mmstage, mmkeepalias, mmmessages, ")
                                _T("mmstarted, mmfinished, mmdetail from hm_mailboxmoves where mmaccountid = @ACCOUNTID order by mmid desc"));
         command.AddParameter("@ACCOUNTID", accountID);
      }
      else
      {
         command.SetQueryString(_T("select mmid, mmaccountid, mmoldaddress, mmnewaddress, mmstage, mmkeepalias, mmmessages, ")
                                _T("mmstarted, mmfinished, mmdetail from hm_mailboxmoves order by mmid desc"));
      }

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);

      if (!recordset)
         return false;

      // Bounded here rather than with a row-limiting clause, which is TOP on two
      // backends and LIMIT on the other two: the journal holds one row per move
      // an administrator made by hand, so there is nothing to gain from four
      // spellings of the same sentence.
      while (!recordset->IsEOF() && (int) rows.size() < limit)
      {
         MailboxMoveRecord record;
         ReadRecord_(recordset, record);
         rows.push_back(record);

         recordset->MoveNext();
      }

      return true;
   }

   bool
   MailboxMove::Newest(__int64 accountID, MailboxMoveRecord &row)
   {
      std::vector<MailboxMoveRecord> rows;

      if (!List(accountID, 1, rows) || rows.empty())
         return false;

      row = rows[0];
      return true;
   }

   AnsiString
   MailboxMove::OutcomeJson(const MailboxMoveOutcome &outcome)
   {
      return "{" + JsonField("old_address", outcome.old_address) + "," +
             JsonField("new_address", outcome.new_address) + "," +
             JsonField("old_domain", outcome.old_domain) + "," +
             JsonField("new_domain", outcome.new_domain) + "," +
             JsonNumber("account_id", outcome.account_id) + "," +
             JsonBool("alias_kept", outcome.alias_kept) + "," +
             JsonBool("store_moved", outcome.store_moved) + "," +
             JsonBool("references_incomplete", outcome.references_incomplete) + "," +
             JsonNumber("messages", outcome.messages) + "," +
             JsonNumber("folders", outcome.folders) + "," +
             JsonNumber("aliases_repointed", outcome.aliases_repointed) + "," +
             JsonNumber("list_memberships", outcome.list_memberships) + "," +
             JsonNumber("queued_recipients", outcome.queued_recipients) + "," +
             JsonNumber("quarantine_entries", outcome.quarantine_entries) + "," +
             JsonNumber("journal_id", outcome.journal_id) + "}";
   }

   AnsiString
   MailboxMove::RecordJson(const MailboxMoveRecord &record)
   {
      return "{" + JsonNumber("id", record.id) + "," +
             JsonNumber("account_id", record.account_id) + "," +
             JsonField("old_address", record.old_address) + "," +
             JsonField("new_address", record.new_address) + "," +
             JsonNumber("stage", record.stage) + "," +
             JsonField("stage_name", StageName(record.stage)) + "," +
             JsonBool("complete", record.stage == StageComplete) + "," +
             JsonBool("keep_alias", record.keep_alias) + "," +
             JsonNumber("messages", record.messages) + "," +
             JsonField("started", record.started) + "," +
             JsonField("finished", record.finished) + "," +
             JsonField("detail", record.detail) + "}";
   }

   AnsiString
   MailboxMove::RecordsJson(const std::vector<MailboxMoveRecord> &records)
   {
      AnsiString json = "{\"moves\":[";

      for (size_t i = 0; i < records.size(); i++)
      {
         if (i > 0)
            json += ",";

         json += RecordJson(records[i]);
      }

      json += "]}";

      return json;
   }

   void
   MailboxMove::ReportInterrupted()
   {
      // Once per process. StartServers runs again on every Reinitialize - which
      // the regression suite does between fixtures - and an interrupted move is
      // a fact about the store, not about this start, so saying it again at each
      // one would turn one incident into a stream.
      static bool reported = false;

      if (reported)
         return;

      reported = true;

      std::vector<MailboxMoveRecord> rows;

      if (!List(0, MaxJournalRows, rows))
         return;

      for (const MailboxMoveRecord &record : rows)
      {
         if (record.stage == StageComplete || record.stage == StageRolledBack || record.stage == StageRollbackFailed)
            continue;

         ErrorManager::Instance()->ReportError(ErrorManager::High, 6947, "MailboxMove::ReportInterrupted",
            Formatter::Format(_T("The move of {0} to {1}, started at {2}, did not finish: it stopped after \"{3}\". ")
                              _T("hMailServer\\docs\\MailboxMove.md says what that stage leaves behind and how to finish or undo it by hand. Journal row {4}."),
                              record.old_address, record.new_address, record.started, StageName(record.stage), record.id));
      }
   }
}
