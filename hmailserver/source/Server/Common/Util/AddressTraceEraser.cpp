// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "AddressTraceEraser.h"
#include "../Persistence/PersistentArchiveIndex.h"
#include "../Application/Logger.h"
#include <boost/filesystem.hpp>
#include "../Persistence/PersistentBounce.h"
#include "../Persistence/PersistentComplaint.h"

#include "FileUtilities.h"
#include "../Application/Application.h"
#include "../Application/ErrorManager.h"
#include "../Application/IniFileSettings.h"
#include "../AntiSpam/QuarantineStore.h"
#include "ContactGroups.h"
#include "ApiSends.h"
#include "CompromiseDetector.h"
#include "ConfidentialMessages.h"
#include "DiscoveryHolds.h"
#include "InboundWebhooks.h"
#include "OrganisationCa.h"
#include "ProtectedMessages.h"
#include "../BO/Alias.h"
#include "../BO/DistributionList.h"
#include "../Cache/Cache.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../Util/Parsing/StringParser.h"

#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   bool
   AddressTraceEraser::Erase(const String &address, bool includeArchive, int &removedCount)
   {
      return Erase(address, includeArchive, false, removedCount);
   }

   bool
   AddressTraceEraser::Erase(const String &address, bool includeArchive, bool includeLogs, int &removedCount)
   {
      int heldCount = 0;
      return Erase(address, includeArchive, includeLogs, removedCount, heldCount);
   }

   bool
   AddressTraceEraser::Erase(const String &address, bool includeArchive, bool includeLogs, int &removedCount, int &heldCount)
   {
      removedCount = 0;
      heldCount = 0;

      if (address.IsEmpty() || address.Find(_T("@")) <= 0)
         return false;

      bool anyFailed = false;

      // Each store is swept independently and a failure in one does not stop
      // the others - stopping half way through an erasure leaves more behind,
      // not less, which is the same reasoning the account-deletion cascade
      // records. What failed is reported at the end.
      std::vector<String> failedStores;

      // Message trace: the sender and recipient of every traced delivery.
      int count = DeleteAddressRows_("hm_messagetrace", "mtsender", "mtrecipient", address);
      if (count < 0)
         failedStores.push_back(_T("the message trace"));
      else
         removedCount += count;

      // The send API's record of what each key sent (ApiSends.h): the sends the
      // address made, and the address taken out of the recipients of the rest -
      // the key's record of its other recipients stays.
      count = ApiSends::EraseAddress(address);
      if (count < 0)
         failedStores.push_back(_T("the send API's record of sends"));
      else
         removedCount += count;

      // The inbound webhooks' queue (InboundWebhooks.h): the deliveries the
      // address sent, with the messages they kept - a kept one is never swept -
      // and the address taken out of the recipients of the rest.
      count = InboundWebhooks::Instance()->EraseAddress(address);
      if (count < 0)
         failedStores.push_back(_T("the inbound webhooks' queue"));
      else
         removedCount += count;

      // Greylisting triplets: sender and recipient addresses, otherwise removed
      // only by age.
      count = DeleteAddressRows_("hm_greylisting_triplets", "glsenderaddress", "glrecipientaddress", address);
      if (count < 0)
         failedStores.push_back(_T("the greylisting triplets"));
      else
         removedCount += count;

      // Distribution-list memberships. This changes what the list delivers,
      // deliberately: an erasure request is precisely a request to stop being
      // on things.
      count = DeleteAddressRows_("hm_distributionlistsrecipients", "distributionlistrecipientaddress", "", address);
      if (count < 0)
         failedStores.push_back(_T("distribution-list memberships"));
      else
         removedCount += count;

      // Aliases where the address is either half: as the NAME it is the
      // person's own address existing as an alias, as the VALUE it is a
      // forward still sending somebody's mail to them.
      count = DeleteAddressRows_("hm_aliases", "aliasname", "aliasvalue", address);
      if (count < 0)
         failedStores.push_back(_T("aliases"));
      else
         removedCount += count;

      // The alias and distribution-list collections are cached; rows deleted
      // underneath a cache would keep resolving until the next restart, which
      // for an erasure is the difference between done and not done.
      Cache<Alias>::Instance()->Clear();
      Cache<DistributionList>::Instance()->Clear();

      // Bounce counts and suppression-list entries, in every sending domain. This
      // is the one store where erasing an address changes what the server does next:
      // an address removed from a suppression list is sent to again. An erasure is
      // the person asking to be forgotten, which includes being forgotten as dead.
      count = PersistentBounce::EraseAddress(address);
      if (count < 0)
         failedStores.push_back(_T("the bounce counts and suppression lists"));
      else
         removedCount += count;

      // The abuse reports naming the address, as the reader who complained and as
      // the sender complained about. The complaint's effect on the suppression list
      // went with the entry above; this is the report itself, which names who
      // complained about what and is exactly what an erasure is asked to forget.
      count = PersistentComplaint::EraseAddress(address);
      if (count < 0)
         failedStores.push_back(_T("the complaints"));
      else
         removedCount += count;

      // The first-contact memory: the address as a sender other accounts have
      // heard from, and as a recipient they have written to. The person's own
      // memory went with their account.
      String lowered = address;
      lowered.ToLower();
      count = DeleteAddressRows_("hm_knownsenders", "ksaddress", "", lowered);
      if (count < 0)
         failedStores.push_back(_T("the first-contact memory"));
      else
         removedCount += count;

      // Other people's address books: the rows that name the address, collected
      // from mail or written by hand. The row goes rather than the address being
      // blanked, because a card with no address is not the contact the owner
      // made, and the vCard column would still carry it.
      count = DeleteAddressRows_("hm_contacts", "contactaddress", "", lowered);
      if (count < 0)
         failedStores.push_back(_T("the address books"));
      else
         removedCount += count;

      // And the groups in those address books that named the address as a
      // member by address (ContactGroups.h): each is rewritten without it. A
      // group that named the contact by its UID names nothing now that the
      // row is gone, and the UID carries nothing of the person.
      count = ContactGroups::ForgetAddress(lowered);
      if (count < 0)
         failedStores.push_back(_T("the contact groups"));
      else
         removedCount += count;

      // The certificates a domain's S/MIME authority issued for the address
      // (OrganisationCa.h): a valid one is revoked, and the address and the
      // certificate itself - whose subject is the person's name - are taken
      // off the row. The serial and the revocation stay, because the root's
      // CRL must go on listing a certificate that is still out there.
      count = OrganisationCa::EraseAddress(lowered);
      if (count < 0)
         failedStores.push_back(_T("the S/MIME authority's record of certificates"));
      else
         removedCount += count;

      // The certificates other accounts keep of the address as a correspondent
      // (kind 2 of hm_smimekeys, taken from a signed message or from the
      // directory): the certificate's subject is the person. The address is
      // stored lower-cased (RestApiSmime.cpp). The account's own certificates
      // (kind 1) are the account's, and go with it.
      count = DeleteAddressRows_("hm_smimekeys", "smimeaddress", "", lowered, "smimekind = 2");
      if (count < 0)
         failedStores.push_back(_T("the correspondents' S/MIME certificates"));
      else
         removedCount += count;

      // And their OpenPGP keys (kind 2 of hm_pgpring, OpenPgpRing.h), whose user
      // id names the person the same way. Compared lower-cased: a key filed
      // under the address its own user id carries keeps that address's case.
      count = DeleteAddressRows_("hm_pgpring", "lower(pgraddress)", "", lowered, "pgrkind = 2");
      if (count < 0)
         failedStores.push_back(_T("the correspondents' OpenPGP keys"));
      else
         removedCount += count;

      // Whom other accounts have written to - the compromised-account baseline
      // (CompromiseDetector.h), stored folded to lower case. The account forgets
      // one correspondent, and the next message to the address counts as one to
      // a stranger, which is what an erasure asks.
      count = DeleteAddressRows_("hm_accountrecipients", "arcaddress", "", lowered);
      if (count < 0)
         failedStores.push_back(_T("the recipients accounts remember writing to"));
      else
         removedCount += count;

      // Whom an out-of-office reply has answered (hm_autoreplies, stored
      // lower-cased): the address is answered again by a reply that is still on,
      // as a sender the account has not heard from.
      count = DeleteAddressRows_("hm_autoreplies", "arsender", "", lowered);
      if (count < 0)
         failedStores.push_back(_T("the out-of-office memory"));
      else
         removedCount += count;

      // Whether the legal holds could be read. When they could not,
      // DiscoveryHolds answers "held" to every question, which keeps every
      // deletion below safe - and would have every store count what it left as
      // kept by a hold when there may be no case at all. So the state is taken
      // before the first store a hold can keep and again after the last (a
      // failed read is retried five seconds later, so one look at the end could
      // miss it), and an erasure that met unread holds fails naming them, and
      // says nothing about a legal hold.
      bool holdsUnread = DiscoveryHolds::ReadFailed();

      // Messages held for review (CompromiseDetector.h) that the address sent or
      // is a recipient of: discarded whole, as the quarantine's entries below
      // are, and as the review page's Discard does - the message itself names
      // the address, and a message released after the erasure would carry it
      // on. One whose account is under a legal hold stays.
      int held = 0;
      count = CompromiseDetector::EraseAddress(address, held);
      heldCount += held;
      if (count < 0)
         failedStores.push_back(_T("the messages held for review"));
      else
         removedCount += count;

      // Messages that expire (ConfidentialMessages.h): the address taken out of
      // the recipients of a message somebody else sent, which the others go on
      // reading; a message it sent, or one left with no recipient, ended as a
      // withdrawal ends it and its row removed. A held account's stay.
      held = 0;
      count = ConfidentialMessages::EraseAddress(address, held);
      heldCount += held;
      if (count < 0)
         failedStores.push_back(_T("the messages that expire"));
      else
         removedCount += count;

      // Password-protected messages (ProtectedMessages.h) to the address, and
      // the replies it wrote: ended, the ciphertext deleted and the row removed.
      // A held account's stay.
      held = 0;
      count = ProtectedMessages::EraseAddress(address, held);
      heldCount += held;
      if (count < 0)
         failedStores.push_back(_T("the password-protected messages"));
      else
         removedCount += count;

      bool quarantineFailed = false;
      removedCount += EraseQuarantine_(address, quarantineFailed);
      if (quarantineFailed)
         failedStores.push_back(_T("the quarantine"));

      if (includeArchive)
      {
         bool archiveFailed = false;
         held = 0;
         removedCount += EraseArchive_(address, archiveFailed, held);
         heldCount += held;
         if (archiveFailed)
            failedStores.push_back(_T("the archive"));
      }

      holdsUnread = DiscoveryHolds::ReadFailed() || holdsUnread;
      if (holdsUnread)
         failedStores.push_back(_T("the legal holds, which could not be read, so everything a hold might cover was left in place (HM7332)"));

      if (includeLogs)
      {
         int linesChanged = 0;
         if (!Logger::Instance()->EraseFromLogs(address, linesChanged))
            failedStores.push_back(_T("the logs"));
         removedCount += linesChanged;
      }

      if (!failedStores.empty())
      {
         // With the logs rewritten, the error names no address: a line naming it
         // written a moment after every other line that named it was changed
         // would undo what was asked. The caller has the address; the stores to
         // retry are what the line is for.
         const String whose = includeLogs ? String(_T("an address whose log lines were rewritten")) : address;
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6250, "AddressTraceEraser::Erase",
            Formatter::Format(_T("While erasing the traces of {0}, these could not be fully removed and should be retried: {1}."),
               whose, StringParser::JoinVector(failedStores, _T(", "))));
         anyFailed = true;
      }

      String logMessage;
      logMessage.Format(_T("Erased %d stored trace(s) of an address on an operator's request."), removedCount);
      LOG_APPLICATION(logMessage);

      // What a legal hold kept is said, once, beside the count: the operator
      // answering the person needs to know that something was kept, and why.
      // Not when the holds could not be read: what was left then was left for
      // want of an answer, not for a case, and the failure above says so.
      if (heldCount > 0 && !holdsUnread)
      {
         String heldMessage;
         heldMessage.Format(_T("The erasure left %d item(s) in place under a legal hold (docs/Discovery.md): archived copies, or messages held for review, that expire or are password-protected, of a held account."), heldCount);
         LOG_APPLICATION(heldMessage);
      }

      return !anyFailed;
   }

   int
   AddressTraceEraser::DeleteAddressRows_(const AnsiString &table, const AnsiString &firstColumn, const AnsiString &secondColumn, const String &address,
                                          const AnsiString &condition)
   {
      AnsiString where = "(" + firstColumn + " = @ADDRESS1";
      if (!secondColumn.IsEmpty())
         where += " or " + secondColumn + " = @ADDRESS2";
      where += ")";
      if (!condition.IsEmpty())
         where += " and " + condition;

      SQLCommand countCommand("select count(*) as c from " + table + " where " + where);
      countCommand.AddParameter("@ADDRESS1", address);
      if (!secondColumn.IsEmpty())
         countCommand.AddParameter("@ADDRESS2", address);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(countCommand);
      if (!recordset)
         return -1;

      const int count = recordset->GetLongValue("c");

      if (count == 0)
         return 0;

      SQLCommand deleteCommand("delete from " + table + " where " + where);
      deleteCommand.AddParameter("@ADDRESS1", address);
      if (!secondColumn.IsEmpty())
         deleteCommand.AddParameter("@ADDRESS2", address);

      if (!Application::Instance()->GetDBManager()->Execute(deleteCommand))
         return -1;

      return count;
   }

   int
   AddressTraceEraser::EraseQuarantine_(const String &address, bool &failed)
   {
      failed = false;

      // Matched in memory rather than with LIKE, because the recipients column
      // is a comma-separated list and "ann@example.test" LIKE-matches inside
      // "joann@example.test". The list call is the administration surface's own,
      // so the shapes agree with what a reviewer sees.
      //
      // The column is truncated at 1000 characters on write; an address that
      // fell off the end of a very long recipient list is not matchable here,
      // by anything.
      const std::vector<QuarantinedMessage> messages = QuarantineStore::List(1000000);

      int removed = 0;

      for (const QuarantinedMessage &message : messages)
      {
         bool matches = (message.sender.CompareNoCase(address) == 0);

         if (!matches)
         {
            std::vector<String> recipients = StringParser::SplitString(message.recipients, ",");
            for (String recipient : recipients)
            {
               recipient.Trim();
               if (recipient.CompareNoCase(address) == 0)
               {
                  matches = true;
                  break;
               }
            }
         }

         if (!matches)
            continue;

         if (QuarantineStore::Delete(message.id))
            removed++;
         else
            failed = true;
      }

      return removed;
   }

   // True when a string cannot safely be one segment of a filesystem path:
   // empty, all dots (. / .. / ...), or containing a separator. Static file
   // scope - it is only needed here.
   static bool IsUnsafePathSegment_(const String &segment)
   {
      if (segment.IsEmpty())
         return true;

      if (segment.Find(_T("\\")) >= 0 || segment.Find(_T("/")) >= 0)
         return true;

      for (int i = 0; i < segment.GetLength(); i++)
         if (segment[i] != _T('.'))
            return false;

      return true;   // reached only when every character was a dot
   }

   // A path as the set of held copies compares it: one case, one separator and
   // never two in a row, so that the index row of a copy (the archiver joins
   // its path with FileUtilities::Combine) and the file the sweep meets (the
   // archive directory joined by hand) are one copy.
   static String HeldCopyKey_(const String &path)
   {
      String key = path;
      key.Replace(_T("\\"), _T("/"));
      while (key.Find(_T("//")) >= 0)
         key.Replace(_T("//"), _T("/"));
      key.ToLower();
      return key;
   }

   int
   AddressTraceEraser::EraseArchive_(const String &address, bool &failed, int &held)
   {
      failed = false;
      held = 0;

      const String archiveDir = IniFileSettings::Instance()->GetArchiveDir();
      if (archiveDir.IsEmpty())
         return 0;

      // The per-user archive tree is <ArchiveDir>\<domain>\<localpart>, both
      // halves lower-cased by the code that writes it. It holds the received
      // copies and the Sent- copies alike, which is what makes one directory
      // delete the whole per-address sweep.
      std::vector<String> parts = StringParser::SplitString(address, "@");
      if (parts.size() != 2)
         return 0;

      String localPart = parts[0];
      String domainPart = parts[1];
      localPart.ToLower();
      domainPart.ToLower();

      // Neither half may be all-dots or carry a separator: the archive path is
      // built by concatenation, so a local part of ".." would make DeleteDirectory
      // climb to the domain folder and remove every user's archive, and a "\\"
      // would point it anywhere. An address that shaped is not one this server
      // would have written an archive for, so refusing to erase is correct - the
      // caller's COM validation guarantees a single "@" but not this.
      if (IsUnsafePathSegment_(localPart) || IsUnsafePathSegment_(domainPart))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6251, "AddressTraceEraser::EraseArchive_",
            Formatter::Format(_T("The archive was not swept for {0}: its address does not form a safe directory name."), address));
         failed = true;
         return 0;
      }

      // The index rows that name the address go with the files, held copies
      // excepted: a hold is a promise that nothing removes the record. The
      // held rows' paths come back, so that what the hold kept is counted once
      // per copy with the files the sweep below keeps.
      std::vector<String> heldPaths;
      if (PersistentArchiveIndex::RemoveByAddress(address, heldPaths) < 0)
         failed = true;

      std::set<String> heldCopies;
      for (const String &heldPath : heldPaths)
         heldCopies.insert(HeldCopyKey_(heldPath));
      held = (int) heldCopies.size();

      const String userArchive = archiveDir + FileUtilities::PathSeparator + domainPart + FileUtilities::PathSeparator + localPart;

      if (!FileUtilities::Exists(userArchive))
         return 0;

      // File by file rather than the tree at once: a copy under legal hold keeps
      // its file as well as its index row, because a hold that kept the row and
      // lost the file would be a promise the server broke while appearing to
      // keep it. Directories left empty go; the person's directory itself goes
      // when nothing held remains in it.
      boost::system::error_code errorCode;
      boost::filesystem::path root(userArchive.begin(), userArchive.end());
      std::vector<boost::filesystem::path> directories;
      int deleted = 0;
      int kept = 0;

      for (boost::filesystem::recursive_directory_iterator it(root, errorCode);
           it != boost::filesystem::recursive_directory_iterator(); it.increment(errorCode))
      {
         if (errorCode)
            break;

         const boost::filesystem::path current = it->path();

         if (boost::filesystem::is_directory(current, errorCode))
         {
            directories.push_back(current);
            continue;
         }

         if (!boost::filesystem::is_regular_file(current, errorCode))
            continue;

         const String filePath = String(current.wstring().c_str());

         if (PersistentArchiveIndex::IsHeld(filePath))
         {
            kept++;
            heldCopies.insert(HeldCopyKey_(filePath));
            continue;
         }

         if (!boost::filesystem::remove(current, errorCode) || errorCode)
         {
            failed = true;
            errorCode.clear();
            continue;
         }

         deleted++;
      }

      // Deepest first, so a directory is tried after its children were.
      std::sort(directories.begin(), directories.end(),
                [](const boost::filesystem::path &a, const boost::filesystem::path &b) { return a.wstring().size() > b.wstring().size(); });
      directories.push_back(root);

      for (const boost::filesystem::path &directory : directories)
      {
         if (boost::filesystem::is_empty(directory, errorCode) && !errorCode)
            boost::filesystem::remove(directory, errorCode);
         errorCode.clear();
      }

      if (kept > 0)
      {
         String logMessage;
         logMessage.Format(_T("The archive sweep for %s left %d cop%s under legal hold in place."), address.c_str(), kept, kept == 1 ? _T("y") : _T("ies"));
         LOG_APPLICATION(logMessage);
      }

      held = (int) heldCopies.size();
      return deleted > 0 ? 1 : 0;
   }
}
